/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  * @version        : 5.6
  * @note           : 增加了自动重启机制
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "dma.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdbool.h>
#include "ultrasonic_threshold.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

// 在这里设置开关： 1 = PWM模式, 0 = GPIO模式
#define USE_PWM_MODE 1

// 水泵启停函数
void pump_start(void);
void pump_stop(void);


/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* -------- 调试和测试开关 -------- */
#define DEBUG_ENABLE 1       // 调试输出开关
#define TEST_MODE_ENABLE 0   // 测试模式（上位机测试时设为1）

// [新增] 无解析超时复位配置
#define NO_PARSE_RESET_MS            10000  // 10秒未成功解析则执行恢复
#define USE_HARD_RESET_ON_NO_PARSE   1      // 1=整机复位(NVIC_SystemReset)，0=仅复位传感器

#if DEBUG_ENABLE
  #define DEBUG_PRINT(fmt, ...) debug_printf(fmt, ##__VA_ARGS__)
#else
  #define DEBUG_PRINT(fmt, ...) ((void)0)
#endif

/* -------- DMA接收缓冲区 -------- */
#define DMA_RX_BUF_SIZE 2048
static uint8_t dma_rx_buf[DMA_RX_BUF_SIZE];
static uint16_t dma_last_pos = 0;

// [新增] 记录最后一次成功解析时间戳
static uint32_t last_parse_ok_ms = 0;

/* -------- 解析缓冲区 -------- */
#define PARSE_BUF_SIZE 2048
static uint8_t parse_buf[PARSE_BUF_SIZE];
static uint16_t parse_len = 0;

/* -------- 数据结构 -------- */
#define S_COUNT 224

typedef struct {
  int a;
  int b;
  int s[S_COUNT];
  bool valid;
} SensorFrame;

static SensorFrame current_frame;

/* -------- 移动平均 -------- */
#define MAX_MA_WINDOW 16
static int a_hist[MAX_MA_WINDOW];
static uint32_t a_sum = 0;
static uint16_t a_hist_count = 0;
static uint16_t a_hist_pos = 0;
static int a_filtered = 0;
static uint16_t a_ma_window = 16;
static int last_a=0;
static bool a_changed = false;
// 新增：记录最近一次的 b，以及是否需要强回波校验
static int last_b = 0;
static bool require_strong_echo = false;

/* -------- 状态机 -------- */
typedef enum {
  STATE_DETECT = 0,
  STATE_CONFIGURE,
  STATE_VERIFY,     // <-- [新状态] 阈值验证阶段
  STATE_MEASURE,
  STATE_WAIT
} AppState;

static AppState app_state = STATE_DETECT;

/* -------- 状态切换确认机制 -------- */
#define DETECT_CONFIRM_COUNT_MAX 15
#define WAIT_CONFIRM_COUNT_MAX 3
#define MEASURE_CONFIRM_COUNT_MAX 2
#define MEASURE_GRACE_MS 4000      // 测量阶段进入后的降敏时间窗
static int state_confirm_count = 0;
#define VERIFY_COUNT_MAX 8      // <-- [新定义] 验证阶段的连续确认次数
static int verify_confirm_count = 0; // <-- [新变量] 验证计数器
static uint32_t measure_enter_ms = 0; // 记录进入MEASURE的时间，用于降敏

/* -------- 配置参数 -------- */
static int x_param = 60;
static int t1_param = 400;
static int y_param =152;
static int t2_param =250;
static int edge_distance = 160;
static int alarm_distance_check = 0; // 用于VERIFY阶段的距离判断 (x*430/225)
static int stop_distance = 0;        // MEASURE阶段使用的停止距离

/* 阈值 */
#define THRESH_DETECT_STOP   250
#define THRESH_MEASURE_HIGH  295

/* -------- 测试统计 -------- */
#if TEST_MODE_ENABLE
static uint32_t test_frame_count = 0;
static uint32_t test_parse_fail = 0;
static uint32_t test_last_report = 0;
#endif

/* -------- 函数声明 -------- */
static void uart_dma_start(void);
static uint16_t dma_get_pos(void);
static void dma_rx_flush(void);
static void process_dma_data(void);
static bool parse_sensor_frame(const uint8_t *data, uint16_t len, SensorFrame *frame);
static int  find_last_frame_start(const uint8_t *data, uint16_t len);
static void clear_parse_buffer(void);
static void update_a_moving_average(int a, uint16_t window);
static void reset_moving_average(void);
static void send_at(const char *fmt, ...);
static bool wait_for_ok(uint32_t timeout_ms);
static void debug_printf(const char *fmt, ...);

// [新增] 恢复函数声明
static void system_hard_reset(void);
static void sensor_soft_recover(void);

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* 启动DMA接收 */
static void uart_dma_start(void)
{
  HAL_UART_Receive_DMA(&huart1, dma_rx_buf, DMA_RX_BUF_SIZE);
}

/* 获取当前DMA写指针位置（环形缓冲） */
static uint16_t dma_get_pos(void)
{
  return (uint16_t)(DMA_RX_BUF_SIZE - __HAL_DMA_GET_COUNTER(huart1.hdmarx));
}

/* 刷新读取指针：丢弃环形缓冲中的未读数据 */
static void dma_rx_flush(void)
{
  dma_last_pos = dma_get_pos();
}

/* 处理DMA接收的数据，追加到解析缓冲 */
static void process_dma_data(void)
{
  uint16_t current_pos = dma_get_pos();

  if (current_pos != dma_last_pos) {
    if (current_pos > dma_last_pos) {
      // 未发生环绕
      uint16_t data_len = current_pos - dma_last_pos;
      if (parse_len + data_len < PARSE_BUF_SIZE) {
        memcpy(&parse_buf[parse_len], &dma_rx_buf[dma_last_pos], data_len);
        parse_len += data_len;
      } else {
        clear_parse_buffer();
      }
    } else {
      // 发生环绕
      uint16_t len1 = DMA_RX_BUF_SIZE - dma_last_pos;
      uint16_t len2 = current_pos;
      if (parse_len + len1 + len2 < PARSE_BUF_SIZE) {
        memcpy(&parse_buf[parse_len], &dma_rx_buf[dma_last_pos], len1);
        parse_len += len1;
        memcpy(&parse_buf[parse_len], &dma_rx_buf[0], len2);
        parse_len += len2;
      } else {
        clear_parse_buffer();
      }
    }

    dma_last_pos = current_pos;
  }
}

/* 清空解析缓冲区 */
static void clear_parse_buffer(void)
{
  memset(parse_buf, 0, parse_len);
  parse_len = 0;
}

/* 查找最后一个 'a:' 的起始位置 */
static int find_last_frame_start(const uint8_t *data, uint16_t len)
{
  if (!data || len < 2) return -1;
  for (int i = (int)len - 2; i >= 0; i--) {
    if (data[i] == 'a' && data[i+1] == ':') {
      return i;
    }
  }
  return -1;
}

/* 严格解析：a/b 仅数字；s 仅 [0-9,] 和 行结束符 */
static bool parse_sensor_frame(const uint8_t *data, uint16_t len, SensorFrame *frame)
{
  if (!data || !frame || len < 50) {
    return false;
  }

  // 转成以0结尾字符串
  static char str_buf[PARSE_BUF_SIZE + 1];
  uint16_t copy_len = (len < PARSE_BUF_SIZE) ? len : PARSE_BUF_SIZE;
  memcpy(str_buf, data, copy_len);
  str_buf[copy_len] = '\0';

  // 必须从缓冲区起始位置就是一帧的起点 "a:"
  if (copy_len < 2 || str_buf[0] != 'a' || str_buf[1] != ':') {
    return false;
  }

  // 定位 a:, b:, s:
  char *p_a = str_buf;       // 起始必须是 "a:"
  char *p_b = strstr(p_a + 2, "b:");
  if (!p_b) { return false; }
  char *p_s = strstr(p_b + 2, "s:");
  if (!p_s) { return false; }

  // 解析 a（仅数字）
  frame->a = 0;
  p_a += 2;
  if (*p_a < '0' || *p_a > '9') { return false; }
  while (*p_a >= '0' && *p_a <= '9') { frame->a = frame->a * 10 + (*p_a - '0'); p_a++; }

  // 解析 b（仅数字）
  frame->b = 0;
  p_b += 2;
  if (*p_b < '0' || *p_b > '9') { return false; }
  while (*p_b >= '0' && *p_b <= '9') { frame->b = frame->b * 10 + (*p_b - '0'); p_b++; }

  // 解析 s（仅 [0-9,]，遇到其他字符直接失败；行结束 \r 或 \n 结束解析）
  p_s += 2;
  int s_count = 0;
  int num = 0;
  bool reading_num = false;

  while (*p_s != '\0' && s_count < S_COUNT) {
    char c = *p_s;
    if (c >= '0' && c <= '9') {
      reading_num = true;
      num = num * 10 + (c - '0');
    } else if (c == ',') {
      if (!reading_num) { return false; }
      frame->s[s_count++] = num;
      num = 0;
      reading_num = false;
    } else if (c == '\r' || c == '\n') {
      if (reading_num) {
        frame->s[s_count++] = num;
        num = 0;
        reading_num = false;
      }
      break; // 行结束
    } else {
      // 空格、负号、字母等一律视为非法
      return false;
    }
    p_s++;
  }

  //测试发现有时s_count会卡在223，原因暂未知
  //临时解决方案：直接将最后一位填为0
  if (s_count == S_COUNT-1){
	  frame->s[s_count++] = 0;
  }

  if (s_count >= S_COUNT) {
    frame->valid = true;
    return true;
  }

  return false;
}

/* 更新移动平均 */
static void update_a_moving_average(int a, uint16_t window)
{
  if (window > MAX_MA_WINDOW) window = MAX_MA_WINDOW;
  a_ma_window = window;

  if (a_hist_count < a_ma_window) {
    a_sum += a;
    a_hist[a_hist_pos++] = a;
    if (a_hist_pos >= a_ma_window) a_hist_pos = 0;
    a_hist_count++;
  } else {
    a_sum -= a_hist[a_hist_pos];
    a_sum += a;
    a_hist[a_hist_pos] = a;
    a_hist_pos++;
    if (a_hist_pos >= a_ma_window) a_hist_pos = 0;
  }

  a_filtered = (a_hist_count > 0) ? (int)(a_sum / a_hist_count) : a;
}

/* 重置移动平均 */
static void reset_moving_average(void)
{
  memset(a_hist, 0, sizeof(a_hist));
  a_sum = 0;
  a_hist_count = 0;
  a_hist_pos = 0;
  a_filtered = 0;
}

/* 发送AT命令 */
static void send_at(const char *fmt, ...)
{
  char buf[64];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  if (n > 0 && n < (int)sizeof(buf)) {
    HAL_UART_Transmit(&huart1, (uint8_t*)buf, (uint16_t)n, 100);
    DEBUG_PRINT("[TX] %s", buf);
  }
}

/* 等待OK响应（不污染全局 dma_last_pos，成功后统一 flush） */
static bool wait_for_ok(uint32_t timeout_ms)
{
  uint32_t start = HAL_GetTick();
  static uint8_t ok_buf[64];
  uint16_t ok_len = 0;
  uint16_t search_start_pos = 0;

  uint16_t ok_last_pos = dma_get_pos(); // 本地读指针

  while ((HAL_GetTick() - start) < timeout_ms) {
    uint16_t current_pos = dma_get_pos();

    if (current_pos != ok_last_pos) {
      if (current_pos > ok_last_pos) {
        uint16_t len = current_pos - ok_last_pos;
        for (uint16_t i = 0; i < len && ok_len < sizeof(ok_buf) - 1; i++) {
          ok_buf[ok_len++] = dma_rx_buf[ok_last_pos + i];
        }
      } else {
        uint16_t len1 = DMA_RX_BUF_SIZE - ok_last_pos;
        uint16_t len2 = current_pos;
        for (uint16_t i = 0; i < len1 && ok_len < sizeof(ok_buf) - 1; i++) {
          ok_buf[ok_len++] = dma_rx_buf[ok_last_pos + i];
        }
        for (uint16_t i = 0; i < len2 && ok_len < sizeof(ok_buf) - 1; i++) {
          ok_buf[ok_len++] = dma_rx_buf[i];
        }
      }
      ok_last_pos = current_pos;

      // 搜索 "OK"
      for (uint16_t i = search_start_pos; i + 1 < ok_len; i++) {
        if (ok_buf[i] == 'O' && ok_buf[i+1] == 'K') {
          DEBUG_PRINT("[AT] OK received\r\n");
          dma_rx_flush(); // 丢弃OK/回显，不让其进入 parse_buf
          return true;
        }
      }
      search_start_pos = (ok_len > 1) ? (ok_len - 1) : 0;
    }

    HAL_Delay(1);
  }
  DEBUG_PRINT("[AT] OK timeout\r\n");
  return false;
}

/* 调试输出 */
static void debug_printf(const char *fmt, ...)
{
#if DEBUG_ENABLE
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n > 0 && n < (int)sizeof(buf)) {
    HAL_UART_Transmit(&huart2, (uint8_t*)buf, (uint16_t)n, 100);
  }
#endif
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART1_UART_Init();
  MX_USART2_UART_Init();
  MX_TIM3_Init();
  /* USER CODE BEGIN 2 */
#if (USE_PWM_MODE == 1)
  // PWM 模式：什么也不用做，CubeIDE 已经初始化好了
  // 我们将在 pump_start() 中启动它
#else
  // GPIO 模式：手动将 PB0 重新初始化为标准 GPIO 输出
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  GPIO_InitStruct.Pin = GPIO_PIN_0;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  // 可以在这里设置一个默认的初始状态
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET); // 默认停止(低电平)
#endif

  uart_dma_start();

#if !TEST_MODE_ENABLE
  HAL_Delay(100);
//  send_at("AT+RESET\r\n");
//  wait_for_ok(1000);
  send_at("AT+STOP\r\n");
  wait_for_ok(200);
  dma_rx_flush();

  send_at("AT+DEBUG=1\r\n");
  wait_for_ok(500);

//  send_at("AT+S3=60\r\n");
//  wait_for_ok(500);
//
//  send_at("AT+T2=400\r\n");
//  wait_for_ok(500);
//
//  send_at("AT+S4=152\r\n");
//  wait_for_ok(500);
//
//  send_at("AT+T3=250\r\n");
//  wait_for_ok(500);

  send_at("AT+REBOOT\r\n");
  wait_for_ok(500);
  dma_rx_flush(); // 丢弃启动回显/提示
#endif
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_6, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_7, GPIO_PIN_RESET);

  app_state = STATE_DETECT;
  current_frame.valid = false;

  // [新增] 初始化“最后一次成功解析时间”为当前时间，避免上电即触发
  last_parse_ok_ms = HAL_GetTick();

  DEBUG_PRINT("\r\n=== System Init OK ===\r\n");

  uint32_t last_process = HAL_GetTick();
  uint32_t frame_timeout = HAL_GetTick();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
	    uint32_t now = HAL_GetTick();

	    // 定期处理DMA数据
	    if (now - last_process >= 50) {
	      last_process = now;
	      process_dma_data();

	      // 解析尝试
	      if (parse_len > 50) {
	        // 如果解析成功
	        if (parse_sensor_frame(parse_buf, parse_len, &current_frame)) {
	          #if TEST_MODE_ENABLE
	          test_frame_count++;
	          #endif

	          update_a_moving_average(current_frame.a, a_ma_window);
	          last_a = current_frame.a;
	          last_b = current_frame.b;           // 新增：记录最近的 b
	          a_changed = true;

	          // [新增] 记录成功解析时间
	          last_parse_ok_ms = now;

	          // 解析成功：从缓冲区中移除“最后一个 a:”之前的数据
	          int cut = find_last_frame_start(parse_buf, parse_len);
	          if (cut > 0) {
	            uint16_t remain = parse_len - (uint16_t)cut;
	            memmove(parse_buf, parse_buf + cut, remain);
	            parse_len = remain;
	          } else {
	            // 仅有当前这帧的 a:，清空缓冲
	            clear_parse_buffer();
	          }

	          frame_timeout = now;
	          DEBUG_PRINT("a=%d, a_filtered=%d, [PARSE] SUCCESS\r\n", current_frame.a, a_filtered);

	        } else {
	          #if TEST_MODE_ENABLE
	          test_parse_fail++;
	          #endif

              if (current_frame.a != last_a){
	        	update_a_moving_average(current_frame.a, a_ma_window);
	        	last_a = current_frame.a;
                a_changed = true;
                DEBUG_PRINT("a=%d, a_filtered=%d\r\n", current_frame.a, a_filtered);
	            }

	          // 解析失败：尝试剪到最后一个 a:
	          int cut = find_last_frame_start(parse_buf, parse_len);
	          if (cut > 0) {
	            uint16_t remain = parse_len - (uint16_t)cut;
	            memmove(parse_buf, parse_buf + cut, remain);
	            parse_len = remain;
	          }

	          // 超时或过满则清空
	          if (now - frame_timeout > 2000) {
	            clear_parse_buffer();
	            frame_timeout = now;
	          } else if (parse_len > 1500) {
	            clear_parse_buffer();
	            frame_timeout = now;
	          }
	        }

	      }

	      // 测试统计
	      #if TEST_MODE_ENABLE
	      if (now - test_last_report > 5000) {
	        test_last_report = now;
	        DEBUG_PRINT("\r\n--- Test Report ---\r\n");
	        DEBUG_PRINT("Frames OK: %lu\r\n", test_frame_count);
	        DEBUG_PRINT("Parse fail: %lu\r\n", test_parse_fail);
	        DEBUG_PRINT("a_filtered: %d\r\n", a_filtered);
	        DEBUG_PRINT("State: %d\r\n\r\n", app_state);
	      }
	      #endif
	    }

        // [新增] 无新数据解析看门狗：10秒无成功解析则恢复
        if ((HAL_GetTick() - last_parse_ok_ms) > NO_PARSE_RESET_MS) {
          DEBUG_PRINT("[WATCHDOG] No parsed frame for %lu ms, perform %s reset\r\n",
                      (unsigned long)(HAL_GetTick() - last_parse_ok_ms),
                      (USE_HARD_RESET_ON_NO_PARSE ? "MCU" : "sensor"));
          if (USE_HARD_RESET_ON_NO_PARSE) {
            system_hard_reset(); // 不会返回
          } else {
            sensor_soft_recover(); // 仅复位传感器，继续运行
            last_parse_ok_ms = HAL_GetTick(); // 避免立刻再次触发
          }
        }

	  // 状态机
  #if !TEST_MODE_ENABLE  // 正常模式才运行状态机
	  switch (app_state) {
		case STATE_DETECT:
		{
		  a_ma_window = 16;

		  if (current_frame.valid) {
			if (a_filtered < THRESH_DETECT_STOP) {
			  state_confirm_count++;

			  if (state_confirm_count >= DETECT_CONFIRM_COUNT_MAX) {
				DEBUG_PRINT("[DETECT->CONFIGURE] Confirmed!\r\n");

				send_at("AT+STOP\r\n");
				wait_for_ok(200);
				dma_rx_flush();

				app_state = STATE_CONFIGURE;

				state_confirm_count = 0;
			  }
			} else {
			  state_confirm_count = 0;
			}

			current_frame.valid = false;
		  }
		  break;
		}

		case STATE_CONFIGURE:
		{
		  // 调用超声波阈值算法
		  ThresholdResult_t thresh_result;
		  int16_t s_array[S_COUNT];
		  for (int i = 0; i < S_COUNT; i++) {
		    s_array[i] = (int16_t)current_frame.s[i];
		  }

		  compute_thresholds(s_array, S_COUNT, &thresh_result);

		  DEBUG_PRINT("\r\n=== CONFIGURE RESULT ===\r\n");
		  DEBUG_PRINT("edge_idx: %d\r\n", thresh_result.edge_idx);
		  DEBUG_PRINT("liquid_idx: %d\r\n", thresh_result.liquid_idx);
		  DEBUG_PRINT("x: %d\r\n", thresh_result.x);
		  DEBUG_PRINT("t1: %d\r\n", thresh_result.t1);
		  DEBUG_PRINT("y: %d\r\n", thresh_result.y);
		  DEBUG_PRINT("t2: %d\r\n", thresh_result.t2);
		  DEBUG_PRINT("peak_count: %d\r\n", thresh_result.peak_count);
		  DEBUG_PRINT("edge_extension: %d\r\n", thresh_result.edge_extension); // <-- 新增打印

		  // 检查是否检测到容器
		  if (thresh_result.edge_idx < 0 || thresh_result.liquid_idx < 0) {
		    DEBUG_PRINT("[CONFIGURE] No container detected, skip to WAIT\r\n");
		    DEBUG_PRINT("========================\r\n\r\n");

            HAL_GPIO_WritePin(GPIOA, GPIO_PIN_6, GPIO_PIN_SET);

		    send_at("AT+REBOOT\r\n");
		    wait_for_ok(500);
            dma_rx_flush(); // 丢弃启动回显/提示

		    reset_moving_average();
		    a_ma_window = 16;
		    state_confirm_count = 0;
		    current_frame.valid = false;
            stop_distance = 0; // <-- 无容器时重置，避免旧值残留

		    app_state = STATE_WAIT;
		    break;
		  }

		  // 计算参数
		  x_param = thresh_result.x;
		  t1_param = thresh_result.t1;
		  y_param = thresh_result.y;
		  t2_param = thresh_result.t2;
		  edge_distance = (int)((thresh_result.edge_idx * 430) / 225);
		  // [新逻辑] 计算VERIFY阶段要使用的距离阈值 (来自 x_param)
		  alarm_distance_check = (int)((x_param * 430) / 225);

          DEBUG_PRINT("Calculated edge_distance: %d\r\n", edge_distance);
          // [新逻辑] 打印VERIFY的阈值
          DEBUG_PRINT("Calculated alarm_distance_check: %d\r\n", alarm_distance_check);

          // [新逻辑] 计算 stop_distance
          if (thresh_result.edge_extension == 0) {
            stop_distance = (int)(((x_param + 1) * 430) / 225);
          } else {
            stop_distance = edge_distance + 20;
          }
          // 新增：在 edge_extension==0 时启用强回波校验
          require_strong_echo = (thresh_result.edge_extension == 0);
          DEBUG_PRINT("Calculated stop_distance: %d, strong_echo_check=%d\r\n", stop_distance, require_strong_echo);
          DEBUG_PRINT("========================\r\n\r\n");

		  send_at("AT+S3=%d\r\n", x_param);
		  wait_for_ok(500);

		  send_at("AT+S4=%d\r\n", y_param);
		  wait_for_ok(500);

		  send_at("AT+T2=%d\r\n", t1_param);
		  wait_for_ok(500);

		  send_at("AT+T3=%d\r\n", t2_param);
		  wait_for_ok(500);

		  send_at("AT+REBOOT\r\n");
		  wait_for_ok(500);
		  dma_rx_flush();

		  reset_moving_average();
		  a_ma_window = 3; // 验证和测量阶段使用快速均值
		  state_confirm_count = 0;
		  verify_confirm_count = 0; // [新逻辑] 重置验证计数器
		  current_frame.valid = false;

          HAL_GPIO_WritePin(GPIOA, GPIO_PIN_7, GPIO_PIN_SET);

		  app_state = STATE_VERIFY; // [修改] 下一状态改为VERIFY
		  DEBUG_PRINT("[CONFIGURE->VERIFY]\r\n");
		  break;
		}

        /* --- [新状态] --- */
		case STATE_VERIFY:
		{
		  // 保持在3点均值，快速响应
		  a_ma_window = 3;

		  if (current_frame.valid) {
			int raw_a = current_frame.a;
			int raw_b = current_frame.b;
			current_frame.valid = false;

			// 检查：原始a值是否 < 我们根据x设置的距离阈值
			// 如果是，说明t1阈值被边沿噪声突破了
			if (raw_a <= alarm_distance_check) {
			  // 失败：检测到回波 < 设定值
			  DEBUG_PRINT("[VERIFY] Failed! a=%d < %d. Adjusting T1.\r\n", raw_a, alarm_distance_check);

			  // 按要求增加阈值 = b + 50
              if (raw_b > t1_param){
                t1_param = raw_b + 50;
              }
              else {
                t1_param += 20; // 保底增加20
              }

			  DEBUG_PRINT("[VERIFY] New t1_param = %d\r\n", t1_param);

			  // 停止
			  send_at("AT+STOP\r\n");
			  wait_for_ok(200);
			  dma_rx_flush();

			  // 重新配置T1 (对应AT+T2)
			  send_at("AT+T2=%d\r\n", t1_param);
			  wait_for_ok(500);
			  dma_rx_flush();

			  // 重启
			  send_at("AT+REBOOT\r\n");
			  wait_for_ok(150);
			  dma_rx_flush();

			  // 重置计数和均值
			  verify_confirm_count = 0;
			  reset_moving_average();

			} else {
			  // 成功：未检测到突破
			  verify_confirm_count++;
			  DEBUG_PRINT("[VERIFY] OK (%d/%d). a=%d\r\n", verify_confirm_count, VERIFY_COUNT_MAX, raw_a);

			  if (verify_confirm_count >= VERIFY_COUNT_MAX) {
				// 连续8次成功，进入测量
				DEBUG_PRINT("[VERIFY->MEASURE] Verification complete!\r\n");

				app_state = STATE_MEASURE;

				verify_confirm_count = 0; // 重置验证计数
				state_confirm_count = 0;  // 重置状态计数
				reset_moving_average();   // 测量阶段重新开始计算均值
				a_ma_window = 3;          // 确保测量阶段是3点均值
        measure_enter_ms = now; // 记录进入时间，刚开始降敏
				pump_start();
			  }
			}
		  }
		  break;
		}
        /* --- [新状态结束] --- */

		case STATE_MEASURE:
		{
          if (a_hist_count >= a_ma_window) { // a_ma_window 此时为 3
            // 新增：组合触发条件
            int b_threshold = t1_param / 3;
            bool stop_cond = (a_filtered < stop_distance);
            if (require_strong_echo) {
              stop_cond = stop_cond && (last_b > b_threshold);
            }

            bool measure_armed = (measure_enter_ms != 0) && ((now - measure_enter_ms) >= MEASURE_GRACE_MS);

            if (measure_armed && stop_cond) {
              if (a_changed){
                state_confirm_count++;
                a_changed = false;

                if (state_confirm_count >= MEASURE_CONFIRM_COUNT_MAX ) {
                  DEBUG_PRINT("[MEASURE->WAIT] ALERT ON! a_f=%d < %d, b=%d > %d, strong=%d\r\n",
                              a_filtered, stop_distance, last_b, b_threshold, require_strong_echo);
                  pump_stop();
                  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_7, GPIO_PIN_RESET);
                  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_6, GPIO_PIN_SET);

                  send_at("AT+STOP\r\n");
                  wait_for_ok(100);
                  dma_rx_flush();

//                  send_at("AT+DEBUG=1\r\n");
//                  wait_for_ok(200);
//                  // 恢复默认阈值
//                  send_at("AT+S3=60\r\n");
//                  wait_for_ok(200);
//
//                  send_at("AT+T2=400\r\n");
//                  wait_for_ok(200);
//
//                  send_at("AT+S4=152\r\n");
//                  wait_for_ok(200);

                  send_at("AT+T3=250\r\n");
                  wait_for_ok(200);

                  send_at("AT+REBOOT\r\n");
                  wait_for_ok(500);
                  dma_rx_flush();

                  reset_moving_average();
                  a_ma_window = 16;
                  current_frame.valid = false;

                  app_state = STATE_WAIT;
                  state_confirm_count = 0;
                }
              }
            } else {
              state_confirm_count = 0;
            }
          }

		  break;
		}

		case STATE_WAIT:
		{
		  if (current_frame.valid) {
			if (a_filtered > THRESH_MEASURE_HIGH) {
			  state_confirm_count++;

			  if (state_confirm_count >= WAIT_CONFIRM_COUNT_MAX) {
				DEBUG_PRINT("[WAIT->DETECT] Reset!\r\n");

				send_at("AT+STOP\r\n");
				wait_for_ok(100);
				dma_rx_flush();

				send_at("AT+DEBUG=1\r\n");
				wait_for_ok(200);
				// 恢复默认阈值
				send_at("AT+S3=60\r\n");
				wait_for_ok(200);

				send_at("AT+T2=400\r\n");
				wait_for_ok(200);

				send_at("AT+S4=152\r\n");
				wait_for_ok(200);

				send_at("AT+T3=250\r\n");
				wait_for_ok(200);

				HAL_GPIO_WritePin(GPIOA, GPIO_PIN_6, GPIO_PIN_RESET);
				HAL_GPIO_WritePin(GPIOA, GPIO_PIN_7, GPIO_PIN_RESET); // [修正] 退出WAIT时应熄灭两个灯

				send_at("AT+REBOOT\r\n");
				wait_for_ok(500);
				dma_rx_flush();

				reset_moving_average();
				a_ma_window = 16;
				state_confirm_count = 0;
				current_frame.valid = false;
                stop_distance = 0; // <-- 复位路径重置
                require_strong_echo = false; // 新增：复位强回波校验

                app_state = STATE_DETECT;
              }
			} else {
			  state_confirm_count = 0;
			}

			current_frame.valid = false;
		  }
		  break;
		}

		default:
		  app_state = STATE_DETECT;
		  break;
	  }
  #endif

	  HAL_Delay(5);
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
/**
  * @brief 启动水泵（根据宏定义执行 PWM 或 GPIO High）
  */
void pump_start(void)
{
#if (USE_PWM_MODE == 1)
  // PWM 模式：启动 TIM3 的通道3
  HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_3);
#else
  // GPIO 模式：设置 PB0 为高电平
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_SET);
#endif
}

/**
  * @brief 停止水泵（根据宏定义执行 PWM Stop 或 GPIO Low）
  */
void pump_stop(void)
{
#if (USE_PWM_MODE == 1)
  // PWM 模式：停止 TIM3 的通道3
  HAL_TIM_PWM_Stop(&htim3, TIM_CHANNEL_3);
#else
  // GPIO 模式：设置 PB0 为低电平
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET);
#endif
}

// [新增] 整机软件复位：等效于按下硬复位（由 NVIC 触发）
static void system_hard_reset(void)
{
  DEBUG_PRINT("[WATCHDOG] NVIC_SystemReset()\r\n");
  HAL_Delay(5); // 尽量把日志发出去
  NVIC_SystemReset();
}

// [新增] 仅复位传感器与控制状态，逻辑同 WAIT->DETECT 分支
static void sensor_soft_recover(void)
{
  DEBUG_PRINT("[WATCHDOG] Soft sensor recovery\r\n");

  send_at("AT+STOP\r\n");
  wait_for_ok(100);
  dma_rx_flush();

  send_at("AT+DEBUG=1\r\n");
  wait_for_ok(200);

  // 恢复默认阈值
  send_at("AT+S3=60\r\n");
  wait_for_ok(200);

  send_at("AT+T2=400\r\n");
  wait_for_ok(200);

  send_at("AT+S4=152\r\n");
  wait_for_ok(200);

  send_at("AT+T3=250\r\n");
  wait_for_ok(200);

  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_6, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_7, GPIO_PIN_RESET);

  send_at("AT+REBOOT\r\n");
  wait_for_ok(500);
  dma_rx_flush();

  reset_moving_average();
  a_ma_window = 16;
  state_confirm_count = 0;
  current_frame.valid = false;
  stop_distance = 0;
  require_strong_echo = false;

  app_state = STATE_DETECT;
}
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
