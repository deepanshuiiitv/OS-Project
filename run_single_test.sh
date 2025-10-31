#!/bin/bash
# run_single_test.sh <mode> <outdir>
# mode: baseline | rl
MODE=${1:-baseline}
OUT=${2:-./results_${MODE}}
DURATION=60    # total duration (we will split in two episodes)
EP1=30         # first presentation duration (learning)
GAP=5          # idle gap between episodes
EP2=20         # second presentation (repeat)
LAT_US=10000   # 10ms sleep for latency_probe
CPU_PHASE_WORKERS=4  # number of background CPU workers (we'll use stress)

mkdir -p "$OUT"
echo "Mode: $MODE  Output: $OUT"

# clear kernel logs for clarity (requires sudo)
sudo dmesg -C

# ensure module state
if [ "$MODE" = "baseline" ]; then
  sudo rmmod rl_sched_mod 2>/dev/null || true
else
  # insert module if not present
  if ! lsmod | grep -q rl_sched_mod; then
    sudo insmod ./rl_sched_mod.ko alpha_permille=200 gamma_permille=900 epsilon_permille=300 interval_ms=1000 action_step=5
    sleep 1
  fi
fi

# Start pidstat to capture system CPU per second
pidstat -u 1 > "$OUT/pidstat_${MODE}.log" 2>&1 &
PIDSTAT_PID=$!

# Helper to launch background CPU workers (stress package)
start_cpu_workers() {
  local count=$1
  for i in $(seq 1 $count); do
    # run a CPU-bound busy loop in background (no stress tool required)
    ./throughput_worker.sh $2 "$OUT/worker_${i}_${MODE}.csv" &
    echo $! >> "$OUT/worker_pids_${MODE}.txt"
  done
}

# --- Episode 1 (learning) ---
echo "Episode 1 (learning) start: background CPU-heavy for ${EP1}s"
start_cpu_workers $CPU_PHASE_WORKERS $EP1
# Start latency probe after worker ramp up (probe is the latency-sensitive task)
sleep 1
./latency_probe $LAT_US $EP1 "$OUT/latency_ep1_${MODE}.csv" &
LAT_PID1=$!

sleep $EP1

# kill episode1 workers (they exit on duration). ensure killed if any
if [ -f "$OUT/worker_pids_${MODE}.txt" ]; then
  while read pid; do kill $pid 2>/dev/null || true; done < "$OUT/worker_pids_${MODE}.txt" || true
  rm -f "$OUT/worker_pids_${MODE}.txt"
fi

echo "Episode 1 done. Sleeping gap ${GAP}s"
sleep $GAP

# --- Episode 2 (repeat) ---
echo "Episode 2 (repeat) start: same background for ${EP2}s"
start_cpu_workers $CPU_PHASE_WORKERS $EP2
sleep 1
./latency_probe $LAT_US $EP2 "$OUT/latency_ep2_${MODE}.csv" &
LAT_PID2=$!

sleep $EP2

# cleanup
if [ -f "$OUT/worker_pids_${MODE}.txt" ]; then
  while read pid; do kill $pid 2>/dev/null || true; done < "$OUT/worker_pids_${MODE}.txt" || true
  rm -f "$OUT/worker_pids_${MODE}.txt"
fi

# stop pidstat
kill $PIDSTAT_PID 2>/dev/null || true
wait $PIDSTAT_PID 2>/dev/null || true

# capture dmesg lines for RL actions and any module log messages
dmesg | grep rl_sched_mod > "$OUT/dmesg_rl_${MODE}.log" || true
dmesg > "$OUT/dmesg_all_${MODE}.log" || true

# save process nice snapshot and ps
ps -eo pid,ni,comm > "$OUT/ps_${MODE}.txt"

echo "Run complete. Output in $OUT"
