#!/usr/bin/env python3
# Enhanced analyze_compare.py
# Demonstrates RL improvement in latency, throughput, and CPU usage visually.

import sys, os, glob
import numpy as np
import matplotlib.pyplot as plt

def read_latency(path):
    times, delays = [], []
    if not os.path.exists(path):
        return np.array([]), np.array([])
    with open(path) as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            t, exp, act, delay = line.strip().split(",")
            times.append(float(t))
            delays.append(float(delay))
    if not times:
        return np.array([]), np.array([])
    times = np.array(times) - times[0]
    return times, np.array(delays)

def read_throughput_worker(path):
    times, iters = [], []
    if not os.path.exists(path):
        return np.array([]), np.array([])
    with open(path) as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            t, it = line.strip().split(",")
            times.append(float(t))
            iters.append(int(it))
    if not times:
        return np.array([]), np.array([])
    times = np.array(times) - times[0]
    return times, np.array(iters)

def aggregate_workers(dirpath, mode):
    files = sorted(glob.glob(os.path.join(dirpath, f"worker_*_{mode}.csv")))
    total_iters, durations = 0, []
    for f in files:
        t, it = read_throughput_worker(f)
        if len(t) == 0:
            continue
        total_iters += it[-1]
        durations.append(t[-1] - t[0])
    return total_iters, (max(durations) if durations else 0)

def read_pidstat(cpu_file):
    """
    Parse pidstat log and extract CPU usage (%CPU) values for relevant processes.
    Dynamically detects the '%CPU' column.
    """
    cpu_values = []
    cpu_col = None
    header_found = False

    if not os.path.exists(cpu_file):
        print(f"[!] Missing pidstat log: {cpu_file}")
        return np.array(cpu_values)

    with open(cpu_file) as f:
        for line in f:
            parts = line.strip().split()
            if not parts:
                continue

            # detect header line containing '%CPU'
            if not header_found and "%CPU" in parts:
                cpu_col = parts.index("%CPU")
                header_found = True
                continue

            # skip until header found
            if not header_found:
                continue

            # after header found, pick valid numeric entries
            if len(parts) > cpu_col:
                try:
                    # filter only workload processes (optional)
                    if "throughput_work" in parts[-1] or "latency_probe" in parts[-1]:
                        val = float(parts[cpu_col])
                        cpu_values.append(val)
                except ValueError:
                    pass

    return np.array(cpu_values)

def percentile_stats(delays):
    if len(delays) == 0:
        return None, None, None
    return np.median(delays), np.percentile(delays, 90), np.percentile(delays, 95)

def summarize_and_plot(results_dir):
    baseline = os.path.join(results_dir, "baseline")
    with_rl = os.path.join(results_dir, "with_rl")

    # Input files
    files = {
        "lat_b_ep1": os.path.join(baseline, "latency_ep1_baseline.csv"),
        "lat_b_ep2": os.path.join(baseline, "latency_ep2_baseline.csv"),
        "lat_r_ep1": os.path.join(with_rl, "latency_ep1_rl.csv"),
        "lat_r_ep2": os.path.join(with_rl, "latency_ep2_rl.csv"),
        "pid_b": os.path.join(baseline, "pidstat_baseline.log"),
        "pid_r": os.path.join(with_rl, "pidstat_rl.log")
    }

    # Read latency
    tb1, db1 = read_latency(files["lat_b_ep1"])
    tb2, db2 = read_latency(files["lat_b_ep2"])
    tr1, dr1 = read_latency(files["lat_r_ep1"])
    tr2, dr2 = read_latency(files["lat_r_ep2"])

    # Latency stats
    b1_m, b1_p90, b1_p95 = percentile_stats(db1)
    b2_m, b2_p90, b2_p95 = percentile_stats(db2)
    r1_m, r1_p90, r1_p95 = percentile_stats(dr1)
    r2_m, r2_p90, r2_p95 = percentile_stats(dr2)

    print("\nLatency (Âµs) â€” median, p90, p95")
    print(f"Baseline:  EP1=({b1_m:.2f}, {b1_p90:.2f}, {b1_p95:.2f}) | EP2=({b2_m:.2f}, {b2_p90:.2f}, {b2_p95:.2f})")
    print(f"With RL:   EP1=({r1_m:.2f}, {r1_p90:.2f}, {r1_p95:.2f}) | EP2=({r2_m:.2f}, {r2_p90:.2f}, {r2_p95:.2f})")

    # Improvement %
    latency_improve = ((b2_m - r2_m) / b2_m * 100) if (b2_m and r2_m) else 0

    # Throughput
    total_b1, dur_b1 = aggregate_workers(baseline, "baseline")
    total_b2, dur_b2 = aggregate_workers(baseline, "baseline")
    total_r1, dur_r1 = aggregate_workers(with_rl, "rl")
    total_r2, dur_r2 = aggregate_workers(with_rl, "rl")

    thr_b_ep1 = total_b1 / dur_b1 if dur_b1 else 0
    thr_b_ep2 = total_b2 / dur_b2 if dur_b2 else 0
    thr_r_ep1 = total_r1 / dur_r1 if dur_r1 else 0
    thr_r_ep2 = total_r2 / dur_r2 if dur_r2 else 0

    throughput_improve = ((thr_r_ep2 - thr_b_ep2) / thr_b_ep2 * 100) if thr_b_ep2 else 0

    # === CPU usage (fixed) ===
    cpu_b = read_pidstat(files["pid_b"])
    cpu_r = read_pidstat(files["pid_r"])
    cpu_avg_b = np.mean(cpu_b) if cpu_b.size else 0
    cpu_avg_r = np.mean(cpu_r) if cpu_r.size else 0

    print(f"\nThroughput (iter/s): Baseline EP2={thr_b_ep2:.2f}, RL EP2={thr_r_ep2:.2f}")
    print(f"CPU avg usage: Baseline={cpu_avg_b:.2f}%, With RL={cpu_avg_r:.2f}%")
    print(f"Latency Improvement (EP2): {latency_improve:.2f}%  |  Throughput Improvement (EP2): {throughput_improve:.2f}%")

    # === Plot ===
    fig, axs = plt.subplots(2, 2, figsize=(14, 8))

    # Latency curves
    axs[0,0].plot(tb1, db1, label="Baseline EP1", color='r', alpha=0.6)
    axs[0,0].plot(tb2, db2, label="Baseline EP2", color='orange', alpha=0.6)
    axs[0,0].set_title("Baseline Latency Trend")
    axs[0,0].set_ylabel("Delay (Âµs)")
    axs[0,0].legend()
    axs[0,0].grid(True)

    axs[0,1].plot(tr1, dr1, label="RL EP1", color='blue', alpha=0.6)
    axs[0,1].plot(tr2, dr2, label="RL EP2", color='green', alpha=0.6)
    axs[0,1].set_title("With RL Latency Trend (learning improvement)")
    axs[0,1].legend()
    axs[0,1].grid(True)

    # Throughput comparison
    labels = ["EP1", "EP2"]
    axs[1,0].bar(np.arange(len(labels))-0.15, [thr_b_ep1, thr_b_ep2], width=0.3, color='red', label="Baseline")
    axs[1,0].bar(np.arange(len(labels))+0.15, [thr_r_ep1, thr_r_ep2], width=0.3, color='green', label="With RL")
    axs[1,0].set_xticks(np.arange(len(labels)))
    axs[1,0].set_xticklabels(labels)
    axs[1,0].set_ylabel("Throughput (iter/s)")
    axs[1,0].set_title("Throughput Comparison")
    axs[1,0].legend()
    axs[1,0].grid(True)

    # CPU usage comparison
    axs[1,1].bar(["Baseline", "With RL"], [cpu_avg_b, cpu_avg_r], color=['red', 'green'])
    axs[1,1].set_title("Avg CPU Usage")
    axs[1,1].set_ylabel("CPU %")
    axs[1,1].grid(True)

    fig.suptitle("RL Learning Impact: Latency â†“  Throughput â†‘  CPU Utilization â†“", fontsize=14, fontweight="bold")

    # Annotate improvement
    axs[1,0].text(0.5, max(thr_b_ep2, thr_r_ep2)*0.9,
        f"Latency â†“ {latency_improve:.1f}% | Throughput â†‘ {throughput_improve:.1f}%",
        fontsize=10, ha='center', bbox=dict(facecolor='white', alpha=0.6))

    plt.tight_layout(rect=[0, 0, 1, 0.96])
    out_path = os.path.join(results_dir, "rl_vs_baseline_learning.png")
    plt.savefig(out_path)
    print("\nSaved enhanced comparison plot:", out_path)

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: analyze_compare.py <results_dir>")
        sys.exit(1)
    summarize_and_plot(sys.argv[1])