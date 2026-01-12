import matplotlib.pyplot as plt
import numpy as np
from copy import deepcopy

# Import from your existing file
# Ensure find_threshold5.py is in the same directory or PYTHONPATH
try:
    from find_threshold5 import (
        smooth, detect_peaks, merge_close_peaks, classify_peaks, 
        compute_thresholds, MIN_I, MAX_I, Peak
    )
except ImportError:
    # Fallback if running in a way where import fails (e.g. different cwd)
    import sys
    import os
    sys.path.append(os.path.dirname(os.path.abspath(__file__)))
    from find_threshold5 import (
        smooth, detect_peaks, merge_close_peaks, classify_peaks, 
        compute_thresholds, MIN_I, MAX_I, Peak
    )

def plot_step_1_raw(raw_data):
    plt.figure(figsize=(10, 5))
    plt.plot(raw_data, label='Raw Data', color='gray')
    plt.axvline(x=MIN_I, color='k', linestyle='--', label='Valid Range Start')
    plt.axvline(x=MAX_I, color='k', linestyle='--', label='Valid Range End')
    plt.title("Step 1: Raw Data & Valid Range")
    plt.xlabel("Index")
    plt.ylabel("Amplitude")
    plt.legend()
    plt.grid(True)
    plt.show()

def plot_step_2_smooth(raw_data, sub_data, smoothed_data):
    plt.figure(figsize=(10, 5))
    # Plot raw data in background (shifted to match global index)
    global_x = range(MIN_I, MIN_I+len(sub_data))
    plt.plot(global_x, sub_data, label='Raw (Valid Range)', color='lightgray', linewidth=2)
    # Plot smoothed
    plt.plot(global_x, smoothed_data, label='Smoothed', color='blue', linewidth=1.5)
    plt.title("Step 2: Smoothing (Window=5)")
    plt.xlabel("Index")
    plt.ylabel("Amplitude")
    plt.legend()
    plt.grid(True)
    plt.show()

def plot_step_3_peaks_initial(smoothed_data, peaks, base_thresh):
    plt.figure(figsize=(10, 5))
    global_x = range(MIN_I, MIN_I+len(smoothed_data))
    plt.plot(global_x, smoothed_data, color='blue', label='Smoothed Data')
    
    # Plot peaks
    for p in peaks:
        global_idx = p.idx + MIN_I
        plt.plot(global_idx, p.amp, 'x', color='red', markersize=8)
    
    # Add a dummy handle for legend if peaks exist
    if peaks:
        plt.plot([], [], 'x', color='red', markersize=8, label='Detected Peaks')

    plt.axhline(y=base_thresh, color='green', linestyle='--', label=f'Detect Thresh ({base_thresh:.1f})')
    plt.title(f"Step 3: Initial Peak Detection ({len(peaks)} peaks)")
    plt.xlabel("Index")
    plt.ylabel("Amplitude")
    plt.legend()
    plt.grid(True)
    plt.show()

def plot_step_4_peaks_merged(smoothed_data, peaks):
    plt.figure(figsize=(10, 5))
    global_x = range(MIN_I, MIN_I+len(smoothed_data))
    plt.plot(global_x, smoothed_data, color='blue', label='Smoothed Data')
    
    for p in peaks:
        global_idx = p.idx + MIN_I
        plt.plot(global_idx, p.amp, 'o', color='orange', markersize=8)
        
    if peaks:
        plt.plot([], [], 'o', color='orange', markersize=8, label='Merged Peaks')
    
    plt.title(f"Step 4: Merged Peaks ({len(peaks)} peaks)")
    plt.xlabel("Index")
    plt.ylabel("Amplitude")
    plt.legend()
    plt.grid(True)
    plt.show()

def plot_step_5_classified(smoothed_data, peaks):
    plt.figure(figsize=(10, 5))
    global_x = range(MIN_I, MIN_I+len(smoothed_data))
    plt.plot(global_x, smoothed_data, color='blue', label='Smoothed Data')
    
    # Use a set to avoid duplicate labels in legend
    labels_seen = set()
    
    for p in peaks:
        global_idx = p.idx + MIN_I
        color = 'gray'
        label = p.role
        marker = '.'
        
        if p.role == 'edge':
            color = 'red'
            marker = '^'
        elif p.role == 'liquid':
            color = 'green'
            marker = '*'
        elif p.role == 'interf':
            color = 'purple'
            marker = 'x'
            
        if label not in labels_seen:
            plt.plot(global_idx, p.amp, marker=marker, color=color, markersize=10, label=label)
            labels_seen.add(label)
        else:
            plt.plot(global_idx, p.amp, marker=marker, color=color, markersize=10)

    plt.title("Step 5: Peak Classification")
    plt.xlabel("Index")
    plt.ylabel("Amplitude")
    plt.legend()
    plt.grid(True)
    plt.show()

def plot_step_6_final(raw_data, result):
    data = np.array(raw_data)
    t1, x, t2, y = result["t1"], result["x"], result["t2"], result["y"]

    plt.figure(figsize=(10, 5))
    plt.plot(data, label='Original Data', color='gray', alpha=0.5)
    
    # Highlight valid range
    plt.axvline(x=MIN_I, color='k', linestyle=':', label='Valid Range')
    plt.axvline(x=MAX_I, color='k', linestyle=':')

    # Thresholds
    plt.fill_between(range(MIN_I, x), 0, t1, color='red', alpha=0.3, label=f'Area 1 (t1={t1})')
    if y > x:
        plt.fill_between(range(x, y), 0, t2, color='orange', alpha=0.3, label=f'Area 2 (t2={t2})')

    # Peaks from result
    for p in result.get("peaks", []):
        idx = p["idx"]
        amp = p["amp"]
        role = p["role"]
        marker = 'o'
        c = 'gray'
        if role == 'edge': c='red'; marker='^'
        elif role == 'liquid': c='green'; marker='*'
        elif role == 'interf': c='purple'; marker='x'
        
        # We don't add labels here to avoid clutter, relying on previous plots or adding if needed
        plt.plot(idx, amp, marker, color=c, markersize=8)

    plt.title('Step 6: Final Result')
    plt.xlabel("Index")
    plt.ylabel("Amplitude")
    plt.legend()
    plt.grid(True)
    plt.show()

def visualize_process(raw_data):
    print("Displaying Step 1: Raw Data...")
    plot_step_1_raw(raw_data)
    
    # Data Prep
    sub = raw_data[MIN_I:MAX_I+1]
    
    # 2. Smooth
    print("Displaying Step 2: Smoothing...")
    sm = smooth(sub, w=5)
    plot_step_2_smooth(raw_data, sub, sm)
    
    # 3. Detect
    print("Displaying Step 3: Initial Detection...")
    base_thresh = 250
    detect_thresh = base_thresh * 0.9
    peaks_initial = detect_peaks(sm, detect_thresh)
    # Need deepcopy because merge/classify modifies objects
    plot_step_3_peaks_initial(sm, deepcopy(peaks_initial), detect_thresh)
    
    # 4. Merge
    print("Displaying Step 4: Merging...")
    min_prom = 60
    peaks_merged = merge_close_peaks(deepcopy(peaks_initial), min_prom=min_prom)
    plot_step_4_peaks_merged(sm, deepcopy(peaks_merged))
    
    # 5. Classify
    print("Displaying Step 5: Classification...")
    # Note: classify_peaks modifies the list in-place
    peaks_classified = deepcopy(peaks_merged)
    classify_peaks(peaks_classified, len(sub))
    plot_step_5_classified(sm, peaks_classified)
    
    # 6. Final Calculation
    print("Displaying Step 6: Final Result...")
    final_result = compute_thresholds(raw_data)
    plot_step_6_final(raw_data, final_result)

if __name__ == "__main__":
    # Sample data
    sample_data = [0,21,87,153,157,310,1650,4568,7216,7656,6557,5345,4383,3605,2971,2428,1986,1659,1450,1974,3776,5490,5612,4761,3902,3165,2558,2093,1719,1427,1179,933,744,625,510,397,314,265,235,194,141,126,123,85,55,70,96,119,207,365,488,576,685,741,723,691,654,580,556,588,633,691,692,690,678,545,334,570,1228,2027,2870,3693,4381,4814,4910,4671,4218,3698,3218,2814,2446,2079,1752,1531,1337,1128,960,810,727,753,845,1021,1300,1630,1920,2109,2189,2178,2054,1846,1651,1518,1439,1345,1266,1232,1161,1110,1080,998,887,765,678,613,505,398,341,319,289,239,190,158,173,213,214,186,145,121,122,151,219,273,342,427,505,544,510,436,361,305,257,198,229,437,949,1764,2828,4049,5307,6530,7566,8175,8259,7905,7236,6426,5659,4982,4472,4206,4119,4152,4299,4519,4714,4777,4659,4355,3935,3533,3213,2931,2673,2472,2282,2064,1876,1731,1589,1442,1297,1203,1162,1110,1014,897,760,569,366,188,98,218,289,305,257,144,93,106,98,91,38,16,53,70,42,5,36,99,140,149,182,215,198,145,147,167,162,210,315,370,380,374,364]
    
    visualize_process(sample_data)
