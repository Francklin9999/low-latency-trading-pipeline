#!/usr/bin/env bash
# Run an N-second benchmark of the HFT stack into a self-contained report dir
# (perf counters, proc snapshots, sliced dashboard log, preflight, report.md/json).
# Usage: sudo ./scripts/utils/bench_latency.sh [duration_s]
# Env: HFT_BENCH_{DURATION_S,WARMUP_S,OUT_DIR,ASSUME_RUNNING,KEEP_STACK}

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

DURATION="${1:-${HFT_BENCH_DURATION_S:-120}}"
WARMUP="${HFT_BENCH_WARMUP_S:-15}"
OUT_DIR="${HFT_BENCH_OUT_DIR:-results/bench_$(date +%Y%m%d_%H%M%S)}"
ASSUME_RUNNING="${HFT_BENCH_ASSUME_RUNNING:-0}"
KEEP_STACK="${HFT_BENCH_KEEP_STACK:-0}"

mkdir -p "$OUT_DIR/stack_logs"
echo "[bench] output dir: $OUT_DIR"
echo "[bench] warmup=${WARMUP}s  measure=${DURATION}s"

# preflight: machine state that explains the numbers
{
    echo "# preflight"
    echo
    echo "## cpu governor"
    for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        [[ -e $f ]] && echo "$f = $(cat "$f")"
    done | sort -u
    echo
    echo "## isolated CPUs (cmdline isolcpus / nohz_full)"
    cat /proc/cmdline 2>/dev/null | tr ' ' '\n' | grep -E '^(isolcpus|nohz_full|rcu_nocbs)=' || echo "(none)"
    echo
    echo "## hugepages"
    grep -E "Huge|^Hugepagesize" /proc/meminfo 2>/dev/null
    echo
    echo "## perf_event_paranoid"
    cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo "(unavailable)"
    echo
    echo "## kernel + cpu"
    uname -a
    lscpu 2>/dev/null | grep -E '^(Model name|CPU\(s\):|Thread\(s\) per core|Core\(s\) per socket|Socket\(s\):|NUMA node\(s\):|CPU MHz)' || true
} > "$OUT_DIR/preflight.txt"

# stack bring-up
STACK_PID=""
if [[ "$ASSUME_RUNNING" != "1" ]]; then
    echo "[bench] launching scripts/run/all.sh in background"
    # Force the stack to use our bench output dir so its logs live with us.
    HFT_LOG_DIR="$OUT_DIR/stack_logs" \
        nohup bash "$SCRIPT_DIR/../run/all.sh" >"$OUT_DIR/stack_logs/launcher.log" 2>&1 &
    STACK_PID=$!
    echo "[bench] launcher pid=$STACK_PID"

    # wait for trading to come up; pidof is fine since binaries are unique names
    echo "[bench] waiting up to 60s for trading to start..."
    for _ in $(seq 1 60); do
        if pidof trading >/dev/null 2>&1; then break; fi
        sleep 1
    done
fi

TRADING_PID="$(pidof trading 2>/dev/null | awk '{print $1}')"
if [[ -z "$TRADING_PID" ]]; then
    echo "[bench] FATAL: trading process not found" >&2
    [[ -n "$STACK_PID" ]] && kill -INT "$STACK_PID" 2>/dev/null || true
    exit 1
fi
echo "[bench] trading pid=$TRADING_PID"

# warm-up window
echo "[bench] warm-up ${WARMUP}s..."
sleep "$WARMUP"

# measurement window
WINDOW_START_NS="$(date +%s%N)"
cp -f "/proc/$TRADING_PID/status" "$OUT_DIR/proc_trading_before.txt" 2>/dev/null || true
cat "/proc/$TRADING_PID/io" 2>/dev/null >> "$OUT_DIR/proc_trading_before.txt" || true
cat "/proc/$TRADING_PID/sched" 2>/dev/null >> "$OUT_DIR/proc_trading_before.txt" || true

echo "[bench] perf stat for ${DURATION}s on pid $TRADING_PID"
perf stat \
    -e cycles,instructions,cache-references,cache-misses,branch-instructions,branch-misses,context-switches,cpu-migrations,page-faults \
    -p "$TRADING_PID" \
    -o "$OUT_DIR/perf_trading.txt" \
    -- sleep "$DURATION" || {
        echo "[bench] WARN: perf stat failed (paranoid level? missing perf?)" >&2
    }

WINDOW_END_NS="$(date +%s%N)"
cp -f "/proc/$TRADING_PID/status" "$OUT_DIR/proc_trading_after.txt" 2>/dev/null || true
cat "/proc/$TRADING_PID/io" 2>/dev/null >> "$OUT_DIR/proc_trading_after.txt" || true
cat "/proc/$TRADING_PID/sched" 2>/dev/null >> "$OUT_DIR/proc_trading_after.txt" || true

# slice dashboard audit log to the measurement window
DASH_DIR="results/dashboard_logs"
# Concatenate today's + yesterday's log (window may cross midnight UTC) and filter.
TODAY="$(date -u +%Y%m%d)"
YESTERDAY="$(date -u -d 'yesterday' +%Y%m%d 2>/dev/null || true)"
DASH_FILES=()
[[ -n "$YESTERDAY" && -f "$DASH_DIR/dashboard_${YESTERDAY}.txt" ]] && DASH_FILES+=("$DASH_DIR/dashboard_${YESTERDAY}.txt")
[[ -f "$DASH_DIR/dashboard_${TODAY}.txt" ]] && DASH_FILES+=("$DASH_DIR/dashboard_${TODAY}.txt")

if (( ${#DASH_FILES[@]} == 0 )); then
    echo "[bench] WARN: no dashboard log found under $DASH_DIR -- report will be empty" >&2
    : > "$OUT_DIR/dashboard_window.jsonl"
else
    echo "[bench] slicing ${DASH_FILES[*]}"
    python3 - "$WINDOW_START_NS" "$WINDOW_END_NS" "$OUT_DIR/dashboard_window.jsonl" "${DASH_FILES[@]}" <<'PY'
import json, sys, re
start_ns = int(sys.argv[1])
end_ns   = int(sys.argv[2])
out_path = sys.argv[3]
src_paths = sys.argv[4:]

# Keep records whose ns timestamp (one of these fields, or ev.ts, or ISO loggedAt)
# falls in the window. Cheap heuristic, good enough.
NS_FIELDS = ("ackObserveTs", "orderObserveTs", "serverRecvTs", "clientSendTs")

iso_re = re.compile(r'"loggedAt":"([^"]+)"')

def iso_to_ns(s):
    # 2026-05-16T00:00:00.661Z
    from datetime import datetime, timezone
    return int(datetime.fromisoformat(s.replace("Z","+00:00")).timestamp() * 1e9)

kept = 0
with open(out_path, "w") as out:
    for p in src_paths:
        with open(p, "r", errors="replace") as f:
            for line in f:
                try:
                    rec = json.loads(line)
                except Exception:
                    continue
                ts = None
                ev = rec.get("event") or {}
                for k in NS_FIELDS:
                    v = rec.get(k) or ev.get(k)
                    if isinstance(v, (int, float)):
                        ts = int(v); break
                if ts is None:
                    v = ev.get("ts")
                    if isinstance(v, (int, float)):
                        ts = int(v)
                if ts is None:
                    m = iso_re.search(line)
                    if m:
                        try: ts = iso_to_ns(m.group(1))
                        except Exception: ts = None
                if ts is not None and start_ns <= ts <= end_ns:
                    out.write(line if line.endswith("\n") else line + "\n")
                    kept += 1
print(f"[slice] kept {kept} records", file=sys.stderr)
PY
fi

# window metadata
cat > "$OUT_DIR/window.json" <<EOF
{
  "start_ns": $WINDOW_START_NS,
  "end_ns":   $WINDOW_END_NS,
  "warmup_s": $WARMUP,
  "duration_s": $DURATION,
  "trading_pid": $TRADING_PID
}
EOF

# tear down (unless told to keep)
if [[ -n "$STACK_PID" && "$KEEP_STACK" != "1" ]]; then
    echo "[bench] tearing down stack (pid $STACK_PID)"
    kill -INT "$STACK_PID" 2>/dev/null || true
    wait "$STACK_PID" 2>/dev/null || true
fi

# build the report
if command -v python3 >/dev/null 2>&1; then
    echo "[bench] generating report"
    python3 "$SCRIPT_DIR/bench_report.py" "$OUT_DIR"
else
    echo "[bench] WARN: python3 missing; skipping report generation" >&2
fi

echo "[bench] done. report at $OUT_DIR/report.md"
