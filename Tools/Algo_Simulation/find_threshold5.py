from dataclasses import dataclass
from typing import List, Optional, Dict

MIN_I, MAX_I = 40, 170  # 有效区间

@dataclass
class Peak:
    idx: int
    amp: int
    left: int
    right: int
    prominence: float
    role: str = "unknown"


def smooth(signal, w=5):
    if w <= 1:
        return signal[:]
    out = []
    half = w // 2
    for i in range(len(signal)):
        s = 0
        c = 0
        for j in range(i-half, i+half+1):
            if 0 <= j < len(signal):
                s += signal[j]; c += 1
        out.append(s / c)
    return out

def detect_peaks(sig, base_thresh):
    peaks = []
    n = len(sig)
    for i in range(1, n-1):
        if sig[i] > sig[i-1] and sig[i] >= sig[i+1] and sig[i] >= base_thresh:
            # temporary single-point peak; refine bounds
            left = i
            while left > 0 and sig[left-1] <= sig[left] and sig[left-1] > 0:
                left -= 1
                if sig[left] < base_thresh * 0.4:
                    break
            right = i
            while right < n-1 and sig[right+1] <= sig[right] and sig[right+1] > 0:
                right += 1
                if sig[right] < base_thresh * 0.4:
                    break
            # valley-based prominence (simplified)
            valley_left = min(sig[max(0, left-2):left+1]) if left > 0 else sig[left]
            valley_right = min(sig[right:right+3]) if right < n-1 else sig[right]
            prom = max(sig[i] - valley_left, sig[i] - valley_right)
            
            peaks.append(Peak(idx=i, amp=int(sig[i]), left=left, right=right,
                              prominence=prom))
    return peaks

def merge_close_peaks(peaks: List[Peak], min_prom: float):
    if not peaks:
        return peaks
    peaks.sort(key=lambda p: p.idx)
    merged = []
    current = peaks[0]
    for p in peaks[1:]:
        # overlap or near
        if p.idx - current.idx <= 3 and (min(p.amp, current.amp) > 0.7 * max(p.amp, current.amp)):
            # merge by choosing higher
            if p.amp > current.amp:
                current.idx = p.idx
                current.amp = p.amp
            current.left = min(current.left, p.left)
            current.right = max(current.right, p.right)
            current.prominence = max(current.prominence, p.prominence)
        else:
            if current.prominence >= min_prom:
                merged.append(current)
            current = p
    if current.prominence >= min_prom:
        merged.append(current)
    return merged

def classify_peaks(peaks: List[Peak], valid_len: int):
    if not peaks:
        return None, None
    # sort
    peaks.sort(key=lambda p: p.idx)
    if len(peaks) == 1:
        p = peaks[0]
        # midpoint heuristic
        if p.idx > valid_len * 0.55:
            p.role = "liquid"
            return None, p
        else:
            p.role = "edge"
            return p, None

    # 多峰情况
    # 先假定最左峰是边沿
    edge = peaks[0]
    edge.role = "edge"

    if len(peaks) == 2:
        liquid = peaks[-1]
        liquid.role = "liquid"
        return edge, liquid

    # 使用局部窗口长度(valid_len)而非全局MAX_I来划分右侧候选区
    # “右侧区域”取从edge向右40%窗口长度处开始
    right_idx = int(edge.idx + (valid_len - edge.idx) * 0.4)
    right_peaks = [pk for pk in peaks[1:] if pk.idx > right_idx]

    # 兜底：若右侧候选为空，直接用最右峰作为液面峰，防止max([])报错
    rightmost = peaks[-1]
    if not right_peaks:
        liquid = rightmost
    else:
        max_right_peak = max(right_peaks, key=lambda pk: pk.amp)
        liquid = max_right_peak

    liquid.role = "liquid"

    for pk in peaks:
        if pk.role == "unknown":
            pk.role = "interf"
    return edge, liquid

def compute_thresholds(raw):
    sub = raw[MIN_I:MAX_I+1]
    sm = smooth(sub, w=5)

    base_thresh = 250

    peaks = detect_peaks(sm, base_thresh*0.9)  # 稍微放宽初捕捉
    min_prom = 60
    peaks = merge_close_peaks(peaks, min_prom=min_prom)

    if len(peaks)<=1:
        return {
            "edge_idx": None, "liquid_idx": None,
            "t1": 400, "x": 60,
            "t2": 250, "y": 152,
            "peaks": [], 
            "notes": "No container"
        }

    
    edge, liquid = classify_peaks(peaks, len(sub))

    # index restore
    def restore_idx(local_idx): 
        return local_idx + MIN_I

    # Interference peaks
    interf_peaks = []
    if edge and liquid:
        for p in peaks:
            if p.role == "interf" and edge.idx < p.idx < liquid.idx:
                interf_peaks.append(p)

    # Determine x and t1
    edge_extension = False
    interf_peaks_right = []
    edge_cluster_right = edge.right
    edge_cluster_amp = edge.amp

    if interf_peaks:
        interf_max = max(interf_peaks, key=lambda p: p.amp)
        if interf_max.amp >= edge.amp:
            edge_extension = True
            edge_cluster_right = interf_max.right
            edge_cluster_amp = interf_max.amp
            interf_peaks_right = [p for p in interf_peaks if p.idx > interf_max.idx]
        
        
    x = restore_idx(min(edge_cluster_right + 3, liquid.idx - 3))
    t1 = edge_cluster_amp + 0.3*edge_cluster_amp

    # Determine y
    if not interf_peaks or not interf_peaks_right:
        y_local = 152
    else:
        y_local = max(p.right for p in interf_peaks)
        consecutive = 0
        interf_right = y_local
        for i in range(interf_right+1, liquid.idx):
            if sm[i] < 250:
                consecutive += 1
                if consecutive >= 3:
                    y_local = i
                    break
            else:
                consecutive = 0
    
    y = restore_idx(min(y_local, liquid.idx - 2))

    # Determine t2
    if edge_extension:
        if interf_peaks_right:
            int_max_amp = max(p.amp for p in interf_peaks_right)
        else:
            int_max_amp = 250
    else:
        if interf_peaks:
            int_max_amp = max(p.amp for p in interf_peaks)
        else:
            int_max_amp = 250
    t2 = int_max_amp + 0.3*int_max_amp


    # Adjust ordering
    if y < x:
        m = y
        y = x
        x = m
    elif y == x:
        y += 1

    result = {
        "edge_idx": restore_idx(edge.idx) if edge and edge.role == "edge" else None,
        "liquid_idx": restore_idx(liquid.idx) if liquid else None,
        "t1": int(round(t1)),
        "x": x,
        "t2": int(round(t2)),
        "y": y,
        "peaks": [
            {
                "idx": restore_idx(p.idx),
                "amp": p.amp,
                "prom": round(p.prominence,2),
                "left": restore_idx(p.left),
                "right": restore_idx(p.right),
                "role": p.role
            } for p in peaks
        ],
    }
    return result

# 示例调用
if __name__ == "__main__":
    # data = [0,22,101,164,106,154,961,2650,4075,4131,3324,2569,2036,1587,1230,984,765,566,480,755,1447,
    #            1954,1816,1406,1093,875,685,555,451,341,274,212,150,138,125,100,93,62,18,21,45,46,31,27,20,
    #            24,10,17,16,2,26,5,40,20,31,7,22,38,9,46,11,33,2,32,20,21,7,43,20,53,57,25,18,5,29,21,47,28,
    #            19,6,0,0,0,0,0,1,0,0,0,10,46,41,91,191,320,575,828,1033,1306,1550,1647,1644,1541,1342,1116,
    #            914,751,622,512,416,339,294,267,258,267,237,194,203,189,154,134,116,68,31,55,87,65,33,66,81,
    #            58,58,47,6,5,4,35,48,39,52,42,21,21,18,18,14,10,34,32,47,66,191,320,575,828,1033,1306,1550,
    #            1647,1644,1541,1342,1116,914,751,62,47,64,42,35,24,31,36,31,26,21,27,41,48,62,37,26,41,57,32,
    #            19,64,38,34,42,41,23,15,24,25,38,36,17,62,40,10,53,33,17,20,24,12,2,13,5,5,23,29,38,27,23,39,
    #            35,47,71,44,5,6,51]  # 你的 224 点数组
    #data = [0,22,101,178,143,238,1382,3967,6440,7042,6224,5189,4283,3520,2948,2466,2013,1655,1453,2049,3973,5851,6139,5340,4451,3670,2992,2459,2060,1720,1416,1201,1000,792,661,567,480,409,360,376,387,370,411,495,541,581,644,649,577,495,427,406,418,349,203,127,214,526,1000,1635,2384,3174,3923,4537,5010,5256,5092,4599,4023,3518,3182,2992,2861,2778,2704,2583,2418,2197,1950,1717,1466,1224,1024,808,565,364,233,130,58,82,102,150,165,129,190,222,154,178,234,254,263,229,191,180,153,125,143,148,226,730,1681,2981,4515,6200,7875,9334,10369,10693,10108,8796,7304,6132,5398,4936,4561,4136,3628,3095,2590,2184,1981,1966,1943,1804,1634,1475,1295,1121,986,911,846,742,625,514,430,351,253,140,116,152,227,280,223,192,208,197,214,239,221,215,236,222,209,205,208,222,221,210,162,93,84,144,220,282,336,370,362,340,294,185,116,362,723,1120,1567,2042,2486,2827,2975,2871,2596,2264,1918,1619,1384,1147,937,790,703,650,621,606,587,568,533,504,502,503,514,557,601,627,653,646,589,502,391,288,202,90,36,77,129,205]
    data = [0,22,101,195,175,210,1392,4051,6518,7118,6312,5233,4305,3521,2879,2361,1920,1594,1431,2055,3988,5859,6140,5333,4448,3671,2974,2436,2042,1707,1412,1151,937,792,661,528,441,401,406,474,583,739,931,1117,1288,1406,1454,1469,1444,1384,1396,1484,1511,1440,1338,1216,998,646,158,529,1236,1931,2636,3243,3672,3903,3846,3473,2949,2506,2200,1929,1735,1681,1653,1625,1588,1468,1278,1066,861,653,482,411,437,461,507,553,538,499,438,363,297,258,263,265,241,223,233,239,237,224,197,150,114,110,88,49,125,289,576,844,1021,1125,1154,1170,1379,1773,2346,3098,3755,4071,3976,3562,2965,2352,1838,1454,1453,1969,2650,3094,3149,2907,2502,2075,1705,1394,1175,1019,854,713,635,623,704,802,874,989,1111,1173,1190,1159,1074,978,921,915,895,847,841,854,820,781,773,737,662,631,619,574,494,385,277,242,262,290,292,271,259,235,201,162,107,236,589,960,1384,1882,2345,2682,2848,2791,2556,2253,1946,1684,1481,1284,1096,963,863,782,751,733,685,643,640,598,519,461,385,315,263,198,156,142,176,258,278,235,226,215,198,213,195,163]
    out = compute_thresholds(data)
    print(out)