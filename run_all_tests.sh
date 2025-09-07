#!/bin/bash
# Master test script for RL scheduler module
# Runs ~20 test cases with and without rl_sched_mod.ko

MOD=rl_sched_mod
RESULTS_DIR=results
LOGFILE=summary.log

mkdir -p $RESULTS_DIR
echo "[*] RL Scheduler Test Run $(date)" > $RESULTS_DIR/$LOGFILE

# Function: insert module if not loaded
load_module() {
    if lsmod | grep -q $MOD; then
        sudo rmmod $MOD
    fi
    sudo insmod ${MOD}.ko || { echo "[!] Failed to insert module"; exit 1; }
}

# Function: remove module
unload_module() {
    if lsmod | grep -q $MOD; then
        sudo rmmod $MOD
    fi
}

# Function: run workload + collect snapshot
run_case() {
    case_id=$1
    desc=$2
    cmd=$3
    mode=$4   # withRL / noRL
    out_file="$RESULTS_DIR/case${case_id}_${mode}.log"

    echo "==== CASE $case_id ($mode): $desc ====" | tee -a $RESULTS_DIR/$LOGFILE
    echo "Command: $cmd" >> $out_file
    echo "Start: $(date)" >> $out_file

    # start dmesg capture in background (only for withRL)
    if [ "$mode" = "withRL" ]; then
        sudo dmesg -wH > $out_file.dmesg 2>&1 &
        DMESG_PID=$!
    fi

    # run workload
    eval "$cmd &"
    WPID=$!

    # sample 5 times while workload runs
    for i in {1..5}; do
        echo "--- Snapshot $i ---" >> $out_file
        ps -eo pid,comm,nice,pcpu --sort=-pcpu | head -15 >> $out_file
        sleep 2
    done

    wait $WPID 2>/dev/null

    if [ "$mode" = "withRL" ]; then
        kill $DMESG_PID
    fi

    echo "End: $(date)" >> $out_file
    echo "Results saved: $out_file" >> $RESULTS_DIR/$LOGFILE
}

# ===============================
# Test Cases
# ===============================
tests=(
  "1|Single CPU-bound|stress --cpu 1 --timeout 10"
  "2|Two CPU-bound|stress --cpu 2 --timeout 10"
  "3|Three CPU-bound|stress --cpu 3 --timeout 10"
  "4|CPU + Sleep| (stress --cpu 1 --timeout 10 &) ; sleep 5"
  "5|Short burst| dd if=/dev/zero of=/dev/null bs=1M count=200"
  "6|Interactive (bash + stress)| (stress --cpu 1 --timeout 10 &) ; sleep 2 ; echo 'typing...'"
  "7|I/O bound| dd if=/dev/zero of=testfile bs=1M count=300"
  "8|CPU + I/O mix| (stress --cpu 1 --timeout 10 &) ; dd if=/dev/zero of=testfile bs=1M count=300"
  "9|High nice stress| nice -n 10 stress --cpu 1 --timeout 10"
  "10|Low nice stress| nice -n -10 stress --cpu 1 --timeout 10"
  "11|Zombie process| (sleep 1 &) ; kill -9 $!"
  "12|Kernel thread observe| ps -eLf | grep '\['"
  "13|Short interval RL| stress --cpu 1 --timeout 10"
  "14|Long interval RL| stress --cpu 1 --timeout 10"
  "15|Clamp max nice| stress --cpu 1 --timeout 10"
  "16|Clamp min nice| stress --cpu 1 --timeout 10"
  "17|Many processes| stress --cpu 5 --timeout 10"
  "18|Background GUI app (if available)| echo 'open browser manually during test'"
  "19|System idle| sleep 5"
  "20|Kill mid-run| (stress --cpu 2 --timeout 20 &) ; sleep 5 ; kill %1"
)

# ===============================
# Run all tests
# ===============================
for t in "${tests[@]}"; do
    IFS="|" read -r id desc cmd <<< "$t"

    # ---- Run with RL ----
    load_module
    run_case $id "$desc" "$cmd" "withRL"
    unload_module

    # ---- Run without RL ----
    run_case $id "$desc" "$cmd" "noRL"

    echo "" >> $RESULTS_DIR/$LOGFILE
done

echo "[*] All tests complete. Check $RESULTS_DIR/"
