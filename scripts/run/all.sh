#!/usr/bin/env bash
# Build + launch the whole HFT stack (server, trading, wire_observer) with host
# latency tunings (governor, hugepages, affinity, SCHED_FIFO, rlimits). Tunings
# are saved and restored on exit. Usage: sudo ./scripts/run/all.sh (or scripts/all.sh).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

# CONFIG (override via env)
HUGEPAGES_2M="${HFT_HUGEPAGES_2M:-1024}"        # 1024 * 2MiB = 2 GiB
SERVER_CPUS="${HFT_SERVER_CPUS:-2,3}"
TRADING_CPUS="${HFT_TRADING_CPUS:-4,5,6}"
OBSERVER_CPUS="${HFT_OBSERVER_CPUS:-7}"
RT_PRIO_SERVER="${HFT_RT_PRIO_SERVER:-80}"
RT_PRIO_TRADING="${HFT_RT_PRIO_TRADING:-90}"
RT_PRIO_OBSERVER="${HFT_RT_PRIO_OBSERVER:-50}"

# CMake pin knobs must match the taskset masks above.
export HFT_SERVER_FEED_CORE="${HFT_SERVER_FEED_CORE:-2}"
export HFT_SERVER_ORDER_RECV_CORE="${HFT_SERVER_ORDER_RECV_CORE:-3}"
export HFT_CLIENT_UDP_RECV_CORE="${HFT_CLIENT_UDP_RECV_CORE:-4}"
export HFT_CLIENT_ORDER_SEND_CORE="${HFT_CLIENT_ORDER_SEND_CORE:-5}"
export HFT_ENGINE_DISPATCH_CORE="${HFT_ENGINE_DISPATCH_CORE:-6}"

export HFT_AFXDP_IFACE="${HFT_AFXDP_IFACE:-lo}"
export HFT_AFXDP_QUEUE="${HFT_AFXDP_QUEUE:-0}"
export HFT_AFXDP_FORCE_ZEROCOPY="${HFT_AFXDP_FORCE_ZEROCOPY:-0}"
export HFT_AFXDP_PREFER_SKB="${HFT_AFXDP_PREFER_SKB:-1}"
# AF_XDP TX is broken on `lo`; set HFT_AFXDP_TX=1 only on a real NIC.
export HFT_AFXDP_TX="${HFT_AFXDP_TX:-0}"

SERVER_MODE="${HFT_SERVER_MODE:-4}"
SERVER_SPEED="${HFT_SERVER_SPEED:-1.0}"           # 1.0 = wall-clock real time
SERVER_FEED_PORT="${HFT_SERVER_FEED_PORT:-5000}"
SERVER_FEED_IP="${HFT_SERVER_FEED_IP:-127.0.0.1}"
SERVER_ITCH_FILE="${HFT_SERVER_ITCH_FILE:-data/01302020.NASDAQ_ITCH50}"
export HFT_FEED_DELAY_S="${HFT_FEED_DELAY_S:-30}" # server holds the feed this long before pushing

# Market-maker tuning (read by trading at startup; override any subset).
export HFT_MM_HALF_SPREAD_TICKS="${HFT_MM_HALF_SPREAD_TICKS:-1}"   # floor for valid quotes
export HFT_MM_REQUOTE_TICKS="${HFT_MM_REQUOTE_TICKS:-0}"           # 0 = requote on every event
export HFT_MM_PER_SYMBOL_RPS="${HFT_MM_PER_SYMBOL_RPS:-500}"
export HFT_MM_AGG_RPS="${HFT_MM_AGG_RPS:-5000}"
export HFT_MM_BURST="${HFT_MM_BURST:-200}"
export HFT_MM_QUOTE_QTY="${HFT_MM_QUOTE_QTY:-100}"
export HFT_MM_EMA_ALPHA="${HFT_MM_EMA_ALPHA:-0.15}"
export HFT_MM_INV_AVERSION="${HFT_MM_INV_AVERSION:-0.01}"
export HFT_MM_MAX_INVENTORY="${HFT_MM_MAX_INVENTORY:-500}"

# Per-symbol MM stats dump to stderr; default OFF (the dashboard shows these).
export HFT_PRINT_MM_STATS="${HFT_PRINT_MM_STATS:-0}"

# Risk rate-limit ceiling; bump above the compile-time cap so MM aggression shows.
export HFT_RISK_RPS="${HFT_RISK_RPS:-10000}"
export HFT_RISK_BURST="${HFT_RISK_BURST:-1000}"

# Observability stack: rabbitmq + postgres + control (dashboard on :8080)
HFT_STACK="${HFT_STACK:-1}"                      # 1 = bring it up, 0 = skip
HFT_STACK_DOWN_ON_EXIT="${HFT_STACK_DOWN_ON_EXIT:-0}"  # 1 = docker compose down on exit
DASHBOARD_PORT="${HFT_DASHBOARD_PORT:-8080}"

LOG_DIR="${HFT_LOG_DIR:-results/run_$(date +%Y%m%d_%H%M%S)}"
mkdir -p "$LOG_DIR" run

# root / sudo handling
if [[ $EUID -ne 0 ]]; then
    echo "[boot] re-exec under sudo to apply kernel tunings and AF_XDP" >&2
    exec sudo --preserve-env=HFT_HUGEPAGES_2M,HFT_SERVER_CPUS,HFT_TRADING_CPUS,HFT_OBSERVER_CPUS,\
HFT_RT_PRIO_SERVER,HFT_RT_PRIO_TRADING,HFT_RT_PRIO_OBSERVER,\
HFT_SERVER_FEED_CORE,HFT_SERVER_ORDER_RECV_CORE,HFT_CLIENT_UDP_RECV_CORE,HFT_CLIENT_ORDER_SEND_CORE,HFT_ENGINE_DISPATCH_CORE,\
HFT_AFXDP_IFACE,HFT_AFXDP_QUEUE,HFT_AFXDP_FORCE_ZEROCOPY,HFT_AFXDP_PREFER_SKB,HFT_AFXDP_TX,HFT_HOUSEKEEPING_CORE,\
HFT_SERVER_MODE,HFT_SERVER_SPEED,HFT_SERVER_FEED_PORT,HFT_SERVER_FEED_IP,HFT_SERVER_ITCH_FILE,HFT_LOG_DIR,\
HFT_STACK,HFT_STACK_DOWN_ON_EXIT,HFT_DASHBOARD_PORT,HFT_FEED_DELAY_S,\
HFT_MM_HALF_SPREAD_TICKS,HFT_MM_REQUOTE_TICKS,HFT_MM_PER_SYMBOL_RPS,HFT_MM_AGG_RPS,HFT_MM_BURST,\
HFT_MM_QUOTE_QTY,HFT_MM_EMA_ALPHA,HFT_MM_INV_AVERSION,HFT_MM_MAX_INVENTORY,HFT_PRINT_MM_STATS,\
HFT_RISK_RPS,HFT_RISK_BURST \
        bash "$0" "$@"
fi

# save current kernel state so we can restore it on exit
declare -A SAVED
save() { SAVED["$1"]="$(cat "$1" 2>/dev/null || echo "")"; }
restore() {
    for k in "${!SAVED[@]}"; do
        [[ -w "$k" ]] && echo "${SAVED[$k]}" > "$k" 2>/dev/null || true
    done
}

save /sys/kernel/mm/transparent_hugepage/enabled
save /sys/kernel/mm/transparent_hugepage/defrag
save /proc/sys/vm/nr_hugepages
save /proc/sys/vm/swappiness
save /proc/sys/kernel/numa_balancing
save /sys/devices/system/cpu/intel_pstate/no_turbo

declare -A SAVED_GOV
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [[ -r "$g" ]] && SAVED_GOV["$g"]="$(cat "$g")"
done
restore_governors() {
    for k in "${!SAVED_GOV[@]}"; do
        [[ -w "$k" ]] && echo "${SAVED_GOV[$k]}" > "$k" 2>/dev/null || true
    done
}

# cleanup: kill children, restore host tunings
PIDS=()
cleanup() {
    echo
    echo "[shutdown] stopping children: ${PIDS[*]:-none}"
    for pid in "${PIDS[@]:-}"; do
        [[ -z "$pid" ]] && continue
        kill -TERM "$pid" 2>/dev/null || true
    done
    sleep 1
    for pid in "${PIDS[@]:-}"; do
        [[ -z "$pid" ]] && continue
        kill -KILL "$pid" 2>/dev/null || true
    done
    echo "[shutdown] restoring kernel knobs"
    restore
    restore_governors
    # Re-enable C-states + restart irqbalance.
    for cpu in 2 3 4 5 6 7; do
        for s in /sys/devices/system/cpu/cpu${cpu}/cpuidle/state*/disable; do
            [[ -w "$s" ]] && echo 0 > "$s" 2>/dev/null || true
        done
    done
    systemctl start irqbalance 2>/dev/null || true
    if [[ "$HFT_STACK" == "1" && "$HFT_STACK_DOWN_ON_EXIT" == "1" ]]; then
        echo "[shutdown] docker compose down"
        docker compose down 2>/dev/null || true
    elif [[ "$HFT_STACK" == "1" ]]; then
        echo "[shutdown] leaving docker stack up (set HFT_STACK_DOWN_ON_EXIT=1 to stop it)"
    fi
    echo "[shutdown] done. logs in $LOG_DIR"
}
trap cleanup EXIT INT TERM

# observability stack, brought up first so it doesn't fight trading for CPU
if [[ "$HFT_STACK" == "1" ]]; then
    if ! command -v docker >/dev/null 2>&1; then
        echo "[stack] docker not found; skipping observability stack" >&2
    else
        # Recreate rabbitmq if it has stale creds so new env vars take effect.
        if docker inspect hft-rabbitmq-1 >/dev/null 2>&1; then
            current_user=$(docker inspect -f \
                '{{range .Config.Env}}{{println .}}{{end}}' hft-rabbitmq-1 \
                2>/dev/null | grep '^RABBITMQ_DEFAULT_USER=' | cut -d= -f2 \
                || true)
            if [[ "$current_user" != "admin" ]]; then
                echo "[stack] rabbitmq has stale creds (user='${current_user:-<none>}') -- recreating"
                docker compose down rabbitmq control >> "$LOG_DIR/stack.log" 2>&1 || true
            fi
        fi
        echo "[stack] docker compose up rabbitmq postgres control (--force-recreate control)"
        docker compose up -d --build --force-recreate control postgres rabbitmq \
            > "$LOG_DIR/stack.log" 2>&1
        # wait for the control service to answer on :$DASHBOARD_PORT
        for _ in $(seq 1 60); do
            if curl -fsS "http://localhost:$DASHBOARD_PORT/" >/dev/null 2>&1; then
                break
            fi
            sleep 1
        done
        if curl -fsS "http://localhost:$DASHBOARD_PORT/" >/dev/null 2>&1; then
            echo "[stack] dashboard ready at http://localhost:$DASHBOARD_PORT"
        else
            echo "[stack] WARN dashboard not responding on :$DASHBOARD_PORT"
            echo "[stack]      docker compose logs control | tail -40"
            docker compose logs control 2>&1 | tail -40 || true
        fi
    fi
fi

# build (Release, with pinning macros baked in)
echo "[build] cmake configure + build"
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DHFT_SERVER_FEED_CORE="$HFT_SERVER_FEED_CORE" \
    -DHFT_SERVER_ORDER_RECV_CORE="$HFT_SERVER_ORDER_RECV_CORE" \
    -DHFT_CLIENT_UDP_RECV_CORE="$HFT_CLIENT_UDP_RECV_CORE" \
    -DHFT_CLIENT_ORDER_SEND_CORE="$HFT_CLIENT_ORDER_SEND_CORE" \
    -DHFT_ENGINE_DISPATCH_CORE="$HFT_ENGINE_DISPATCH_CORE" \
    >/dev/null
cmake --build build -j

for b in build/bin/server build/bin/trading build/bin/wire_observer; do
    [[ -x "$b" ]] || { echo "[build] missing $b" >&2; exit 1; }
done

# host tunings
echo "[tune] CPU governor -> performance, turbo on"
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [[ -w "$g" ]] && echo performance > "$g" || true
done
[[ -w /sys/devices/system/cpu/intel_pstate/no_turbo ]] && \
    echo 0 > /sys/devices/system/cpu/intel_pstate/no_turbo || true

echo "[tune] transparent hugepages -> always"
echo always > /sys/kernel/mm/transparent_hugepage/enabled
echo always > /sys/kernel/mm/transparent_hugepage/defrag

echo "[tune] compacting memory to defrag for 2MiB hugepages"
[[ -w /proc/sys/vm/compact_memory ]] && echo 1 > /proc/sys/vm/compact_memory || true

echo "[tune] reserving $HUGEPAGES_2M x 2MiB hugepages"
echo "$HUGEPAGES_2M" > /proc/sys/vm/nr_hugepages
actual=$(cat /proc/sys/vm/nr_hugepages)
if [[ "$actual" -lt "$HUGEPAGES_2M" ]]; then
    # Fragmentation blocked full allocation; compact once more and retry.
    echo "[tune] only got $actual; compacting again and retrying"
    echo 1 > /proc/sys/vm/compact_memory 2>/dev/null || true
    sleep 1
    echo "$HUGEPAGES_2M" > /proc/sys/vm/nr_hugepages 2>/dev/null || true
    actual=$(cat /proc/sys/vm/nr_hugepages)
fi
if [[ "$actual" -lt "$HUGEPAGES_2M" ]]; then
    echo "[tune] WARN nr_hugepages=$actual (wanted $HUGEPAGES_2M) -- AF_XDP MAP_HUGETLB will fall back to 4K pages; THP (always) still active"
else
    echo "[tune] nr_hugepages=$actual"
fi

echo "[tune] swappiness=10, numa_balancing=0"
echo 10 > /proc/sys/vm/swappiness
[[ -w /proc/sys/kernel/numa_balancing ]] && echo 0 > /proc/sys/kernel/numa_balancing || true

echo "[tune] dropping pagecache/dentries/inodes (cold start)"
sync
echo 3 > /proc/sys/vm/drop_caches

# Deep latency tunings; each trims a tail-latency source, reverted by the EXIT trap.
echo "[tune] disabling deep C-states on trading cores 2..7"
for cpu in 2 3 4 5 6 7; do
    for s in /sys/devices/system/cpu/cpu${cpu}/cpuidle/state*/disable; do
        [[ -w "$s" ]] && echo 1 > "$s" 2>/dev/null || true
    done
    # also lock min freq to max so the package doesn't downclock mid-burst
    if [[ -w /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_min_freq ]]; then
        cat /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_max_freq \
            > /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_min_freq 2>/dev/null || true
    fi
done

echo "[tune] killing timer migration + schedstats"
save /proc/sys/kernel/timer_migration
save /proc/sys/kernel/sched_schedstats
echo 0 > /proc/sys/kernel/timer_migration 2>/dev/null || true
echo 0 > /proc/sys/kernel/sched_schedstats 2>/dev/null || true

echo "[tune] enabling net.core.busy_poll / busy_read (50us)"
save /proc/sys/net/core/busy_poll
save /proc/sys/net/core/busy_read
echo 50 > /proc/sys/net/core/busy_poll 2>/dev/null || true
echo 50 > /proc/sys/net/core/busy_read 2>/dev/null || true

echo "[tune] enlarging TCP/UDP socket buffers"
save /proc/sys/net/core/rmem_max
save /proc/sys/net/core/wmem_max
save /proc/sys/net/core/rmem_default
save /proc/sys/net/core/wmem_default
echo 134217728 > /proc/sys/net/core/rmem_max     2>/dev/null || true
echo 134217728 > /proc/sys/net/core/wmem_max     2>/dev/null || true
echo  16777216 > /proc/sys/net/core/rmem_default 2>/dev/null || true
echo  16777216 > /proc/sys/net/core/wmem_default 2>/dev/null || true

# Stop irqbalance and pin every steerable IRQ to core 0, off the trading cores.
echo "[tune] stopping irqbalance + steering IRQs to core 0"
systemctl stop irqbalance 2>/dev/null || true
for irq in /proc/irq/*/smp_affinity; do
    [[ -w "$irq" ]] && echo 1 > "$irq" 2>/dev/null || true   # mask 1 = cpu 0
done

# Housekeeping (telemetry, control) threads inside trading land here, off 4..6.
export HFT_HOUSEKEEPING_CORE="${HFT_HOUSEKEEPING_CORE:-0}"

# Persist core dumps for post-mortem (idempotent).
COREDUMP_CONF=/etc/systemd/coredump.conf.d/99-hft.conf
if [[ ! -f "$COREDUMP_CONF" ]] || ! grep -q "^Storage=external" "$COREDUMP_CONF" 2>/dev/null; then
    echo "[tune] enabling persistent core dumps (-> $COREDUMP_CONF)"
    mkdir -p /etc/systemd/coredump.conf.d
    cat > "$COREDUMP_CONF" <<'EOF'
[Coredump]
Storage=external
Compress=yes
ProcessSizeMax=4G
ExternalSizeMax=4G
EOF
    systemctl daemon-reload 2>/dev/null || true
fi
ulimit -c unlimited || true
echo "[tune] core dumps -> coredumpctl list trading"

# rlimits for this shell and its children
ulimit -l unlimited || true   # memlock (hugepages, AF_XDP UMEM, mlock)
ulimit -r 99 || true          # max RT priority
ulimit -n 1048576 || true     # fds
ulimit -s unlimited || true

# pre-fault binaries so the first packet doesn't pay a major fault
echo "[tune] warming binaries into pagecache"
for b in build/bin/server build/bin/trading build/bin/wire_observer; do
    cat "$b" > /dev/null
done

# AF_XDP interface hygiene: detach any stale XDP program (a crashed run leaves one
# attached and the next run silently sees zero packets) and kill leftover binaries.
echo "[net]  clearing any stale XDP program from $HFT_AFXDP_IFACE"
existing="$(ip -details link show dev "$HFT_AFXDP_IFACE" 2>/dev/null | grep -oE 'xdp(generic|drv|offload)?' | sort -u || true)"
if [[ -n "$existing" ]]; then
    echo "[net]  found attached: $existing"
fi
ip link set dev "$HFT_AFXDP_IFACE" xdpgeneric off 2>/dev/null || true
ip link set dev "$HFT_AFXDP_IFACE" xdpdrv     off 2>/dev/null || true
ip link set dev "$HFT_AFXDP_IFACE" xdp        off 2>/dev/null || true

# stale binaries from a previous run will fight for the AF_XDP socket / ports
for proc in trading wire_observer server; do
    if pgrep -x "$proc" >/dev/null 2>&1; then
        echo "[net]  killing stale $proc"
        pkill -TERM -x "$proc" 2>/dev/null || true
    fi
done
sleep 0.5
for proc in trading wire_observer server; do
    pkill -KILL -x "$proc" 2>/dev/null || true
done

# bring the iface fully up (lo is normally up, but this is cheap insurance)
ip link set dev "$HFT_AFXDP_IFACE" up 2>/dev/null || true

# launch: chrt -f = SCHED_FIFO, taskset masks layered over the in-binary pinning;
# stdbuf keeps output line-buffered and process substitution tees to the log.
echo "[run]    server   on CPUs $SERVER_CPUS  prio FIFO/$RT_PRIO_SERVER  speed=${SERVER_SPEED}x  pre-feed delay=${HFT_FEED_DELAY_S}s"
stdbuf -oL -eL taskset -c "$SERVER_CPUS" \
    chrt -f "$RT_PRIO_SERVER" \
    ./build/bin/server "$SERVER_MODE" "$SERVER_FEED_PORT" "$SERVER_FEED_IP" "$SERVER_ITCH_FILE" "$SERVER_SPEED" \
    > >(stdbuf -oL sed -u 's/^/[server]   /' | tee "$LOG_DIR/server.log") 2>&1 &
PIDS+=($!)

# small stagger so the server has its listening sockets up before trading binds
sleep 1

echo "[run]    trading  on CPUs $TRADING_CPUS prio FIFO/$RT_PRIO_TRADING (iface=$HFT_AFXDP_IFACE)"
stdbuf -oL -eL taskset -c "$TRADING_CPUS" \
    chrt -f "$RT_PRIO_TRADING" \
    ./build/bin/trading \
    > >(stdbuf -oL sed -u 's/^/[trading]  /' | tee "$LOG_DIR/trading.log") 2>&1 &
PIDS+=($!)

echo "[run]    observer on CPUs $OBSERVER_CPUS prio FIFO/$RT_PRIO_OBSERVER"
stdbuf -oL -eL taskset -c "$OBSERVER_CPUS" \
    chrt -f "$RT_PRIO_OBSERVER" \
    ./build/bin/wire_observer \
    > >(stdbuf -oL sed -u 's/^/[observer] /' | tee "$LOG_DIR/observer.log") 2>&1 &
PIDS+=($!)

echo
echo "[ok] all three processes launched. PIDs: ${PIDS[*]}"
if [[ "$HFT_STACK" == "1" ]]; then
    echo "[ok] dashboard:  http://localhost:$DASHBOARD_PORT"
    echo "[ok] rabbitmq:   http://localhost:15672 (guest/guest)"
fi
echo "[ok] live output below (also mirrored to $LOG_DIR/*.log) -- Ctrl-C to stop"
echo

# wait for any child to exit; trap on EXIT/INT/TERM tears the rest down
while true; do
    for pid in "${PIDS[@]}"; do
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "[watch] PID $pid exited; tearing down"
            exit 1
        fi
    done
    sleep 1
done
