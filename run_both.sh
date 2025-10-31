#!/bin/bash
# run_both.sh
# Runs baseline (no module) and RL (module loaded) consecutively

OUTDIR=./rl_test_results
mkdir -p "$OUTDIR"

echo "== Running BASELINE (module unloaded) =="
./run_single_test.sh baseline "$OUTDIR/baseline"
sleep 5

echo "== Running RL (module loaded) =="
sudo insmod ./rl_sched_mod.ko alpha_permille=200 gamma_permille=900 epsilon_permille=300 interval_ms=1000 action_step=5
sleep 1
./run_single_test.sh rl "$OUTDIR/with_rl"

# remove module afterwards
sudo rmmod rl_sched_mod 2>/dev/null || true

echo "All runs completed. Results in $OUTDIR"
