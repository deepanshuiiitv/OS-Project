#!/bin/bash
# throughput_worker.sh <duration_sec> <out_log>
DUR=${1:-30}
OUT=${2:-throughput_worker.log}
end=$(( $(date +%s) + DUR ))

# Print header
echo "#time_s,iteration" > "$OUT"

iter=0
while [ $(date +%s) -lt $end ]; do
  # small CPU-bound job (short)
  for i in $(seq 1 10000); do
    : $(( (i * 1234567) % 1000003 ))
  done
  iter=$((iter+1))
  printf "%.6f,%d\n" "$(date +%s.%N)" "$iter" >> "$OUT"
  # tiny sleep so it's not saturating full CPU always (adjust if needed)
  sleep 0.01
done
