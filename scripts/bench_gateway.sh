#!/usr/bin/env bash
set -euo pipefail

BASE_URL="${1:-http://127.0.0.1:8080}"
DURATION="${DURATION:-30s}"
WARMUP_DURATION="${WARMUP_DURATION:-10s}"
THREADS="${THREADS:-8}"
CONNECTIONS_LIST="${CONNECTIONS_LIST:-50 100 200 400 800}"
OUTPUT_DIR="${OUTPUT_DIR:-bench_results}"
LUA_SCRIPT="${LUA_SCRIPT:-scripts/wrk_mixed.lua}"

if ! command -v wrk >/dev/null 2>&1; then
  echo "[ERROR] wrk not found. Install it first, e.g.: sudo apt-get install -y wrk" >&2
  exit 1
fi

if [[ ! -f "$LUA_SCRIPT" ]]; then
  echo "[ERROR] Lua script not found: $LUA_SCRIPT" >&2
  exit 1
fi

mkdir -p "$OUTPUT_DIR"
TS="$(date +%Y%m%d_%H%M%S)"
CSV_FILE="$OUTPUT_DIR/summary_${TS}.csv"
LOG_FILE="$OUTPUT_DIR/run_${TS}.log"

cat > "$CSV_FILE" <<'EOF'
phase,connections,threads,duration,requests_per_sec,transfer_per_sec,avg_latency,p50,p75,p90,p99,non_2xx_3xx,socket_errors
EOF

extract_one() {
  local key="$1"
  local text="$2"
  echo "$text" | awk -v k="$key" '
    index($0, k) == 1 {
      val = $0
      sub("^" k "[[:space:]]*", "", val)
      print val
      exit
    }
  '
}

extract_latency_percentile() {
  local percentile="$1"
  local text="$2"
  echo "$text" | awk -v p="$percentile" '$1==p"%"{print $2; exit}'
}

extract_non2xx() {
  local text="$1"
  echo "$text" | sed -n 's/.*Non-2xx or 3xx responses:[[:space:]]*\([0-9]\+\).*/\1/p' | head -n1
}

extract_socket_errors() {
  local text="$1"
  local line
  line="$(echo "$text" | grep -E "Socket errors:" || true)"
  if [[ -z "$line" ]]; then
    echo "0"
    return
  fi
  local c t to h
  c="$(echo "$line" | sed -n 's/.*connect \([0-9]\+\).*/\1/p')"
  t="$(echo "$line" | sed -n 's/.*read \([0-9]\+\).*/\1/p')"
  to="$(echo "$line" | sed -n 's/.*write \([0-9]\+\).*/\1/p')"
  h="$(echo "$line" | sed -n 's/.*timeout \([0-9]\+\).*/\1/p')"
  c="${c:-0}"
  t="${t:-0}"
  to="${to:-0}"
  h="${h:-0}"
  echo $((c + t + to + h))
}

append_csv_row() {
  local phase="$1"
  local conn="$2"
  local duration="$3"
  local output="$4"

  local reqps transfer avg p50 p75 p90 p99 non2xx sockerr
  reqps="$(extract_one "Requests/sec:" "$output")"
  transfer="$(extract_one "Transfer/sec:" "$output")"
  avg="$(echo "$output" | awk '/Latency/{print $2; exit}')"
  p50="$(extract_latency_percentile 50 "$output")"
  p75="$(extract_latency_percentile 75 "$output")"
  p90="$(extract_latency_percentile 90 "$output")"
  p99="$(extract_latency_percentile 99 "$output")"
  non2xx="$(extract_non2xx "$output")"
  sockerr="$(extract_socket_errors "$output")"

  reqps="${reqps:-NA}"
  transfer="${transfer:-NA}"
  avg="${avg:-NA}"
  p50="${p50:-NA}"
  p75="${p75:-NA}"
  p90="${p90:-NA}"
  p99="${p99:-NA}"
  non2xx="${non2xx:-0}"
  sockerr="${sockerr:-0}"

  echo "$phase,$conn,$THREADS,$duration,$reqps,$transfer,$avg,$p50,$p75,$p90,$p99,$non2xx,$sockerr" >> "$CSV_FILE"
}

run_case() {
  local phase="$1"
  local conn="$2"
  local duration="$3"

  echo "[INFO] $phase: c=$conn t=$THREADS d=$duration" | tee -a "$LOG_FILE"
  local cmd_output
  cmd_output="$(wrk --latency -t"$THREADS" -c"$conn" -d"$duration" -s "$LUA_SCRIPT" "$BASE_URL" 2>&1)"
  echo "$cmd_output" | tee -a "$LOG_FILE"
  append_csv_row "$phase" "$conn" "$duration" "$cmd_output"
  echo "" | tee -a "$LOG_FILE"
}

echo "[INFO] Base URL: $BASE_URL" | tee -a "$LOG_FILE"
echo "[INFO] Lua script: $LUA_SCRIPT" | tee -a "$LOG_FILE"
echo "[INFO] CSV output: $CSV_FILE" | tee -a "$LOG_FILE"
echo "[INFO] Log output: $LOG_FILE" | tee -a "$LOG_FILE"
echo "" | tee -a "$LOG_FILE"

echo "[STEP] Warmup" | tee -a "$LOG_FILE"
run_case "warmup" "50" "$WARMUP_DURATION"

echo "[STEP] Staircase benchmark" | tee -a "$LOG_FILE"
for conn in $CONNECTIONS_LIST; do
  run_case "staircase" "$conn" "$DURATION"
done

echo "[DONE] Benchmark finished." | tee -a "$LOG_FILE"
echo "[DONE] CSV: $CSV_FILE" | tee -a "$LOG_FILE"
echo "[DONE] LOG: $LOG_FILE" | tee -a "$LOG_FILE"
