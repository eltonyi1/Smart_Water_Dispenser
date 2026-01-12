#include "ultrasonic_threshold.h"
#include <string.h>

// 工具函数
static inline int16_t min_i16(int16_t a, int16_t b) { return (a < b) ? a : b; }
static inline int16_t max_i16(int16_t a, int16_t b) { return (a > b) ? a : b; }

// 平滑：窗口内简单平均（窗口<=1时拷贝原数据）
static void smooth_signal(const int16_t *signal, int16_t *output, int16_t len, int16_t window) {
    if (window <= 1) {
        for (int16_t i = 0; i < len; i++) output[i] = signal[i];
        return;
    }
    int16_t half = window / 2;
    for (int16_t i = 0; i < len; i++) {
        int32_t sum = 0;
        int16_t cnt = 0;
        for (int16_t j = i - half; j <= i + half; j++) {
            if (j >= 0 && j < len) { sum += signal[j]; cnt++; }
        }
        output[i] = (int16_t)(sum / (cnt > 0 ? cnt : 1));
    }
}

// 峰值检测（与 Python detect_peaks 一致）
static uint8_t detect_peaks(const int16_t *sig, int16_t len, Peak_t *peaks, int16_t base_thresh) {
    uint8_t peak_count = 0;

    for (int16_t i = 1; i < len - 1 && peak_count < MAX_PEAKS; i++) {
        if (sig[i] > sig[i - 1] && sig[i] >= sig[i + 1] && sig[i] >= base_thresh) {
            int16_t left = i;
            int16_t right = i;
            int16_t thresh_40 = (int16_t)((base_thresh * 4) / 10); // 0.4*base

            // 左边界
            while (left > 0 && sig[left - 1] <= sig[left] && sig[left - 1] > 0) {
                left--;
                if (sig[left] < thresh_40) break;
            }
            // 右边界
            while (right < len - 1 && sig[right + 1] <= sig[right] && sig[right + 1] > 0) {
                right++;
                if (sig[right] < thresh_40) break;
            }

            // 两侧谷值（窗口与 Python 等价）
            int16_t valley_left, valley_right;

            // 左侧：min(sig[max(0,left-2) .. left])
            {
                int16_t start = (left - 2) > 0 ? (left - 2) : 0;
                int16_t end = left; // inclusive
                int16_t m = sig[start];
                for (int16_t k = start + 1; k <= end; k++) m = min_i16(m, sig[k]);
                valley_left = m;
            }
            // 右侧：min(sig[right .. min(right+2, len-1)])
            {
                int16_t end = (right + 2 < len - 1) ? (right + 2) : (len - 1);
                int16_t m = sig[right];
                for (int16_t k = right + 1; k <= end; k++) m = min_i16(m, sig[k]);
                valley_right = m;
            }

            // prominence = max(sig[i]-valley_left, sig[i]-valley_right) = sig[i] - min(valleys)
            int16_t valley_min = min_i16(valley_left, valley_right);
            int16_t prom = sig[i] - valley_min;

            peaks[peak_count].idx = i;         // 先存局部索引
            peaks[peak_count].amp = sig[i];
            peaks[peak_count].left = left;
            peaks[peak_count].right = right;
            peaks[peak_count].prominence = prom;
            peaks[peak_count].role = ROLE_UNKNOWN;
            peak_count++;
        }
    }

    return peak_count;
}

// 冒泡排序（索引升序）
static void sort_peaks_by_idx(Peak_t *peaks, uint8_t count) {
    for (uint8_t i = 0; i + 1 < count; i++) {
        for (uint8_t j = 0; j + 1 < count - i; j++) {
            if (peaks[j].idx > peaks[j + 1].idx) {
                Peak_t t = peaks[j];
                peaks[j] = peaks[j + 1];
                peaks[j + 1] = t;
            }
        }
    }
}

// 合并相近峰（距离<=3 且 幅度相近（>0.7）），保留 prominence>=min_prom
static uint8_t merge_close_peaks(Peak_t *peaks, uint8_t count, int16_t min_prom) {
    if (count == 0) return 0;

    sort_peaks_by_idx(peaks, count);

    uint8_t merged_count = 0;
    Peak_t current = peaks[0];

    for (uint8_t i = 1; i < count; i++) {
        Peak_t *p = &peaks[i];
        int16_t min_amp = min_i16(p->amp, current.amp);
        int16_t max_amp = max_i16(p->amp, current.amp);

        if ((p->idx - current.idx) <= 3 && (min_amp * 10 > max_amp * 7)) {
            // 合并：取更高峰顶
            if (p->amp > current.amp) {
                current.idx = p->idx;
                current.amp = p->amp;
            }
            current.left = min_i16(current.left, p->left);
            current.right = max_i16(current.right, p->right);
            current.prominence = max_i16(current.prominence, p->prominence);
        } else {
            if (current.prominence >= min_prom && merged_count < MAX_PEAKS) {
                peaks[merged_count++] = current;
            }
            current = *p;
        }
    }
    if (current.prominence >= min_prom && merged_count < MAX_PEAKS) {
        peaks[merged_count++] = current;
    }

    return merged_count;
}

// 峰分类（与 Python classify_peaks 一致）
static void classify_peaks(Peak_t *peaks, uint8_t count, int16_t valid_len,
                           Peak_t **edge, Peak_t **liquid) {
    *edge = NULL;
    *liquid = NULL;
    if (count == 0) return;

    sort_peaks_by_idx(peaks, count);

    if (count == 1) {
        // idx > 0.55*valid_len => 液位，否则边沿
        if ((int32_t)peaks[0].idx * 100 > (int32_t)valid_len * 55) {
            peaks[0].role = ROLE_LIQUID;
            *liquid = &peaks[0];
        } else {
            peaks[0].role = ROLE_EDGE;
            *edge = &peaks[0];
        }
        return;
    }

    // 多峰：最左为边沿
    peaks[0].role = ROLE_EDGE;
    *edge = &peaks[0];

    if (count == 2) {
        peaks[1].role = ROLE_LIQUID;
        *liquid = &peaks[1];
    } else {
        // 右侧区：edge.idx + 0.4*(valid_len - edge.idx)
        int16_t right_idx = (*edge)->idx + (int16_t)(((int32_t)(valid_len - (*edge)->idx) * 4) / 10);
        // 选择右侧区内幅度最大的峰；若为空，则使用最右峰
        Peak_t *max_right = NULL;
        int16_t max_amp = -32768;
        for (uint8_t i = 1; i < count; i++) {
            if (peaks[i].idx > right_idx && peaks[i].amp > max_amp) {
                max_amp = peaks[i].amp;
                max_right = &peaks[i];
            }
        }
        if (max_right) {
            max_right->role = ROLE_LIQUID;
            *liquid = max_right;
        } else {
            peaks[count - 1].role = ROLE_LIQUID;
            *liquid = &peaks[count - 1];
        }
    }

    // 其余未知设为干扰
    for (uint8_t i = 0; i < count; i++) {
        if (peaks[i].role == ROLE_UNKNOWN) peaks[i].role = ROLE_INTERF;
    }
}

static void set_no_container_defaults(ThresholdResult_t *result) {
    result->edge_idx = -1;
    result->liquid_idx = -1;
    result->t1 = 400;
    result->x  = 60;
    result->t2 = 250;
    result->y  = 152;
    result->peak_count = 0;
    result->edge_extension = 0; // 新增：默认未扩展
}

void compute_thresholds(const int16_t *raw_data, int16_t raw_len, ThresholdResult_t *result) {
    // 初始化
    memset(result, 0, sizeof(ThresholdResult_t));

    // 入参检查：需要至少覆盖到 MAX_I
    if (raw_data == NULL || raw_len <= MAX_I) {
        set_no_container_defaults(result);
        return;
    }

    // 提取有效区间（局部数组）
    int16_t sub[VALID_LENGTH];
    for (int16_t i = 0; i < VALID_LENGTH; i++) {
        sub[i] = raw_data[MIN_I + i];
    }

    // 平滑
    int16_t sm[VALID_LENGTH];
    smooth_signal(sub, sm, VALID_LENGTH, SMOOTH_WINDOW);

    // 峰检测（放宽 0.9x）
    Peak_t peaks[MAX_PEAKS];
    int16_t base_relaxed = (int16_t)((BASE_THRESHOLD * 9) / 10); // 225
    uint8_t count = detect_peaks(sm, VALID_LENGTH, peaks, base_relaxed);

    // 合并
    count = merge_close_peaks(peaks, count, MIN_PROMINENCE);

    // 无容器/不足峰
    if (count <= 1) {
        set_no_container_defaults(result);
        return;
    }

    // 分类
    Peak_t *edge = NULL, *liquid = NULL;
    classify_peaks(peaks, count, VALID_LENGTH, &edge, &liquid);

    // 将峰信息保存为“全局索引”
    result->peak_count = count;
    for (uint8_t i = 0; i < count; i++) {
        result->peaks[i] = peaks[i];
        result->peaks[i].idx  = (int16_t)(peaks[i].idx  + MIN_I);
        result->peaks[i].left = (int16_t)(peaks[i].left + MIN_I);
        result->peaks[i].right= (int16_t)(peaks[i].right+ MIN_I);
    }

    if (edge == NULL || liquid == NULL) {
        set_no_container_defaults(result);
        return;
    }

    // 还原边沿/液位全局索引
    int16_t edge_idx_global   = (int16_t)(edge->idx + MIN_I);
    int16_t liquid_idx_global = (int16_t)(liquid->idx + MIN_I);
    result->edge_idx = edge_idx_global;
    result->liquid_idx = liquid_idx_global;

    // 收集干扰峰（在边沿与液位之间，基于局部索引判断）
    Peak_t *interf_list[MAX_PEAKS];
    uint8_t interf_cnt = 0;
    for (uint8_t i = 0; i < count; i++) {
        // 原 peaks[i].idx 是局部索引
        if (peaks[i].role == ROLE_INTERF && peaks[i].idx > edge->idx && peaks[i].idx < liquid->idx) {
            interf_list[interf_cnt++] = &peaks[i];
        }
    }

    // 计算 x 与 t1
    uint8_t edge_extension = 0;
    int16_t edge_cluster_right = edge->right; // 局部索引
    int16_t edge_cluster_amp   = edge->amp;
    Peak_t *interf_right_list[MAX_PEAKS];
    uint8_t interf_right_cnt = 0;

    if (interf_cnt > 0) {
        // 找最大干扰峰
        Peak_t *imax = interf_list[0];
        for (uint8_t i = 1; i < interf_cnt; i++) {
            if (interf_list[i]->amp > imax->amp) imax = interf_list[i];
        }
        if (imax->amp >= edge->amp) {
            edge_extension = 1;
            edge_cluster_right = imax->right;
            edge_cluster_amp   = imax->amp;
            // 收集其右侧干扰峰
            for (uint8_t i = 0; i < interf_cnt; i++) {
                if (interf_list[i]->idx > imax->idx) {
                    interf_right_list[interf_right_cnt++] = interf_list[i];
                }
            }
        }
    }

    // 新增：输出 edge_extension
    result->edge_extension = edge_extension;

    // x（全局）：min(edge_cluster_right+3, liquid.idx-3) + MIN_I
    {
        int16_t x_local = edge_cluster_right + 3;
        int16_t cap = liquid->idx - 3;
        if (x_local > cap) x_local = cap;
        result->x = (int16_t)(x_local + MIN_I);
    }

    // t1：edge_cluster_amp + 0.15*edge_cluster_amp
    result->t1 = (int16_t)(edge_cluster_amp + (int16_t)(edge_cluster_amp * 3  / 20));

    // 计算 y（局部）
    int16_t y_local;
    if (interf_cnt == 0 || interf_right_cnt == 0) {
        // Python 里直接给本地索引 152（之后再与 liquid 限制取 min）
        y_local = 152;
    } else {
        // 右侧边界 = 所有干扰峰的 right 最大值
        int16_t right_max = interf_list[0]->right;
        for (uint8_t i = 1; i < interf_cnt; i++) {
            if (interf_list[i]->right > right_max) right_max = interf_list[i]->right;
        }
        y_local = right_max;

        // 从 right_max+1 开始，找连续 3 点 sm[i] < 250
        uint8_t consecutive = 0;
        for (int16_t i = right_max + 1; i < liquid->idx; i++) {
            if (i >= 0 && i < VALID_LENGTH && sm[i] < 250) {
                consecutive++;
                if (consecutive >= 3) { y_local = i; break; }
            } else {
                consecutive = 0;
            }
        }
    }
    // y（全局）：min(y_local, liquid.idx-2) + MIN_I
    {
        int16_t cap = liquid->idx - 2;
        if (y_local > cap) y_local = cap;
        result->y = (int16_t)(y_local + MIN_I);
    }

    // 计算 t2
    int16_t int_max_amp = 250;
    if (edge_extension) {
        if (interf_right_cnt > 0) {
            int_max_amp = interf_right_list[0]->amp;
            for (uint8_t i = 1; i < interf_right_cnt; i++) {
                if (interf_right_list[i]->amp > int_max_amp) int_max_amp = interf_right_list[i]->amp;
            }
        }
    } else {
        if (interf_cnt > 0) {
            int_max_amp = interf_list[0]->amp;
            for (uint8_t i = 1; i < interf_cnt; i++) {
                if (interf_list[i]->amp > int_max_amp) int_max_amp = interf_list[i]->amp;
            }
        }
    }
    // t2 = int_max_amp + 0.2*int_max_amp
    result->t2 = (int16_t)(int_max_amp + (int16_t)((int_max_amp * 2 + 5) / 10));

    // 调整 x/y 顺序
    if (result->y < result->x) {
        int16_t tmp = result->y; result->y = result->x; result->x = tmp;
    } else if (result->y == result->x) {
        result->y = (int16_t)(result->y + 1);
    }
}