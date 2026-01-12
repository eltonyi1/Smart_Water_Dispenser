#ifndef ULTRASONIC_THRESHOLD_H
#define ULTRASONIC_THRESHOLD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 全局有效区间（与 Python 一致）
#define MIN_I             40
#define MAX_I             170
#define VALID_LENGTH      (MAX_I - MIN_I + 1)   // 131

// 算法参数（与 Python 一致）
#define SMOOTH_WINDOW     5
#define BASE_THRESHOLD    250
#define MIN_PROMINENCE    60
#define MAX_PEAKS         32                   // 足够覆盖典型波形中峰数量

// 峰角色
typedef enum {
    ROLE_UNKNOWN = 0,
    ROLE_EDGE    = 1,
    ROLE_LIQUID  = 2,
    ROLE_INTERF  = 3
} PeakRole_e;

// 峰信息
typedef struct {
    int16_t idx;         // 峰顶索引（全局索引：已加 MIN_I）
    int16_t amp;         // 峰值幅度
    int16_t left;        // 峰左边界（全局索引）
    int16_t right;       // 峰右边界（全局索引）
    int16_t prominence;  // 显著性（与 Python 等价）
    uint8_t role;        // 峰角色
} Peak_t;

// 结果
typedef struct {
    int16_t edge_idx;    // 边沿峰顶（全局索引），-1 表示无
    int16_t liquid_idx;  // 液位峰顶（全局索引），-1 表示无
    int16_t t1;          // 边沿阈值
    int16_t x;           // 边沿区间右边界（全局索引）
    int16_t t2;          // 干扰阈值
    int16_t y;           // 干扰区间右边界（全局索引）
    uint8_t edge_extension; // 1 表示边沿被右侧更高干扰峰扩展（使用干扰峰构成边沿簇）
    Peak_t  peaks[MAX_PEAKS];
    uint8_t peak_count;
} ThresholdResult_t;

/**
 * 计算阈值（按新 Python 版本算法）
 * @param raw_data 输入原始数据（至少需要覆盖到 MAX_I 索引，典型 224 点）
 * @param raw_len  原始数据长度
 * @param result   输出结果
 */
void compute_thresholds(const int16_t *raw_data, int16_t raw_len, ThresholdResult_t *result);

#ifdef __cplusplus
}
#endif

#endif // ULTRASONIC_THRESHOLD_H