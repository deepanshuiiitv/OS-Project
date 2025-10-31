#!/bin/bash
# cpu_worker.sh <num_workers> <duration_sec>
NUM=${1:-4}
DUR=${2:-60}

# a simple busy-loop worker that does CPU work then sleeps little
worker() {
  local id=$1
  local end=$(( $(date +%s) + DUR ))
  while [ $(date +%s) -lt $end ]; do
    # busy work: calculate some primes-ish loop
    for i in {1..200000}; do
      : $(( (i * i) % 1000003 ))
    done
    # small sleep to avoid overheating
    sleep 0.01
  done
}

for i in $(seq 1 $NUM); do
  worker $i >/dev/null 2>&1 &    # run in background
done

# just keep script alive while background jobs run
wait
