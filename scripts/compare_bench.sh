#!/usr/bin/env bash
set -euo pipefail

INPUT_DIR="${INPUT_DIR:-bench_results}"
OUTPUT_DIR="${OUTPUT_DIR:-bench_results}"
PHASE="${PHASE:-staircase}"
LATEST_N="${LATEST_N:-5}"

if [[ ! -d "$INPUT_DIR" ]]; then
  echo "[ERROR] Input directory not found: $INPUT_DIR" >&2
  exit 1
fi

mapfile -t files < <(ls -1t "$INPUT_DIR"/summary_*.csv 2>/dev/null || true)
if [[ "${#files[@]}" -eq 0 ]]; then
  echo "[ERROR] No summary CSV files found in: $INPUT_DIR" >&2
  exit 1
fi

if ! [[ "$LATEST_N" =~ ^[0-9]+$ ]] || [[ "$LATEST_N" -eq 0 ]]; then
  echo "[ERROR] LATEST_N must be a positive integer, got: $LATEST_N" >&2
  exit 1
fi

if (( ${#files[@]} > LATEST_N )); then
  files=("${files[@]:0:LATEST_N}")
fi

mkdir -p "$OUTPUT_DIR"
TS="$(date +%Y%m%d_%H%M%S)"
OUT_FILE="$OUTPUT_DIR/compare_${TS}.csv"
TMP_FILE="$(mktemp)"

cleanup() {
  rm -f "$TMP_FILE"
}
trap cleanup EXIT

# Normalize selected summary files into a long table with stable run labels.
# Columns: run,connections,requests_per_sec,p99,non_2xx_3xx,socket_errors
for file in "${files[@]}"; do
  run_label="$(basename "$file" .csv)"
  awk -F',' -v phase="$PHASE" -v run="$run_label" '
    NR == 1 { next }
    $1 == phase {
      print run "," $2 "," $5 "," $11 "," $12 "," $13
    }
  ' "$file" >> "$TMP_FILE"
done

if [[ ! -s "$TMP_FILE" ]]; then
  echo "[ERROR] No rows found for phase='$PHASE' in selected files." >&2
  exit 1
fi

mapfile -t run_labels < <(cut -d',' -f1 "$TMP_FILE" | sort -u)
mapfile -t conn_values < <(cut -d',' -f2 "$TMP_FILE" | sort -n -u)

{
  printf "connections"
  for run in "${run_labels[@]}"; do
    printf ",%s_rps,%s_p99,%s_non_2xx_3xx,%s_socket_errors" "$run" "$run" "$run" "$run"
  done
  printf "\n"

  for conn in "${conn_values[@]}"; do
    printf "%s" "$conn"
    for run in "${run_labels[@]}"; do
      row="$(awk -F',' -v r="$run" -v c="$conn" '$1==r && $2==c {print $0; exit}' "$TMP_FILE")"
      if [[ -n "$row" ]]; then
        rps="$(echo "$row" | cut -d',' -f3)"
        p99="$(echo "$row" | cut -d',' -f4)"
        non2xx="$(echo "$row" | cut -d',' -f5)"
        sockerr="$(echo "$row" | cut -d',' -f6)"
      else
        rps="NA"
        p99="NA"
        non2xx="NA"
        sockerr="NA"
      fi
      printf ",%s,%s,%s,%s" "$rps" "$p99" "$non2xx" "$sockerr"
    done
    printf "\n"
  done
} > "$OUT_FILE"

echo "[DONE] Compared ${#run_labels[@]} runs across ${#conn_values[@]} connection levels."
echo "[DONE] Output: $OUT_FILE"
echo "[INFO] Selected files:"
for file in "${files[@]}"; do
  echo "  - $file"
done
