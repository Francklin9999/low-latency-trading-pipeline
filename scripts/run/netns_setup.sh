#!/usr/bin/env bash
# Single-host multicast testbed: two netns (srv 10.0.0.1 / cli 10.0.0.2) joined by
# a veth pair, exercising the multicast feed + AF_XDP path without a second machine.
# Usage (self-elevates): netns_setup.sh {up|down|status|run}

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

NS_SRV="${HFT_NS_SRV:-srv}"
NS_CLI="${HFT_NS_CLI:-cli}"
VETH_SRV="${HFT_VETH_SRV:-veth-srv}"
VETH_CLI="${HFT_VETH_CLI:-veth-cli}"
IP_SRV="${HFT_IP_SRV:-10.0.0.1}"
IP_CLI="${HFT_IP_CLI:-10.0.0.2}"
MCAST_GROUP="${HFT_FEED_MCAST_GROUP:-239.1.1.1}"

# self-elevate to root: ip netns + sysctl need CAP_NET_ADMIN / CAP_SYS_ADMIN
if [[ $EUID -ne 0 ]]; then
    echo "[netns] re-exec under sudo" >&2
    exec sudo --preserve-env=HFT_NS_SRV,HFT_NS_CLI,HFT_VETH_SRV,HFT_VETH_CLI,\
HFT_IP_SRV,HFT_IP_CLI,HFT_FEED_MCAST_GROUP,HFT_SERVER_MODE,HFT_SERVER_SPEED,\
HFT_SERVER_ITCH_FILE,HFT_FEED_DELAY_S,HFT_AFXDP_QUEUE,HFT_AFXDP_FORCE_ZEROCOPY,\
HFT_AFXDP_PREFER_SKB \
        bash "$0" "$@"
fi

cd "$REPO_ROOT"

# netns plumbing
cmd_up() {
    # Idempotent: tear down anything left over from a prior run, then build.
    cmd_down >/dev/null 2>&1 || true

    ip netns add "$NS_SRV"
    ip netns add "$NS_CLI"

    # veth pair is created in the root ns, then each end moved into a namespace.
    ip link add "$VETH_SRV" type veth peer name "$VETH_CLI"
    ip link set "$VETH_SRV" netns "$NS_SRV"
    ip link set "$VETH_CLI" netns "$NS_CLI"

    ip -n "$NS_SRV" addr add "$IP_SRV/24" dev "$VETH_SRV"
    ip -n "$NS_CLI" addr add "$IP_CLI/24" dev "$VETH_CLI"

    ip -n "$NS_SRV" link set lo up
    ip -n "$NS_CLI" link set lo up
    ip -n "$NS_SRV" link set "$VETH_SRV" up
    ip -n "$NS_CLI" link set "$VETH_CLI" up

    # Route admin-scoped multicast (239/8) over the veth in each namespace.
    ip -n "$NS_SRV" route add 239.0.0.0/8 dev "$VETH_SRV"
    ip -n "$NS_CLI" route add 239.0.0.0/8 dev "$VETH_CLI"

    # rp_filter drops multicast on veth; disable it on the receiver side or the
    # socket never sees the frames.
    ip netns exec "$NS_CLI" sysctl -qw net.ipv4.conf.all.rp_filter=0
    ip netns exec "$NS_CLI" sysctl -qw "net.ipv4.conf.$VETH_CLI.rp_filter=0"

    # IGMP querier on a veth is jittery; force v2 so joins are deterministic.
    ip netns exec "$NS_CLI" sysctl -qw net.ipv4.conf.all.force_igmp_version=2

    echo "[netns] up"
    echo "  srv: $NS_SRV  ($VETH_SRV @ $IP_SRV)"
    echo "  cli: $NS_CLI  ($VETH_CLI @ $IP_CLI)"
    echo "  multicast group: $MCAST_GROUP"
}

cmd_down() {
    ip netns del "$NS_SRV" 2>/dev/null || true
    ip netns del "$NS_CLI" 2>/dev/null || true
    # Handles the case where the veth link was never moved into a namespace.
    ip link del "$VETH_SRV" 2>/dev/null || true
    ip link del "$VETH_CLI" 2>/dev/null || true
    echo "[netns] down"
}

cmd_status() {
    echo "--- namespaces ---"
    ip netns list | grep -E "^($NS_SRV|$NS_CLI)( |$)" || echo "(none)"
    for ns in "$NS_SRV" "$NS_CLI"; do
        if ip netns list | grep -q "^$ns"; then
            echo "--- $ns ---"
            ip -n "$ns" -br addr
            ip -n "$ns" route show
        fi
    done
}

# run: up + launch server (srv ns) + trading (cli ns), tear down on exit
cmd_run() {
    # Build first so failures show up before we mess with the host network.
    if [[ ! -x "$REPO_ROOT/build/bin/server" || ! -x "$REPO_ROOT/build/bin/trading" ]]; then
        echo "[netns] building (missing build/bin/server or build/bin/trading)"
        cmake -S "$REPO_ROOT" -B "$REPO_ROOT/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
        cmake --build "$REPO_ROOT/build" -j --target server trading
    fi

    LOG_DIR="${HFT_LOG_DIR:-$REPO_ROOT/results/netns_run_$(date +%Y%m%d_%H%M%S)}"
    mkdir -p "$LOG_DIR"

    PIDS=()
    cleanup() {
        echo
        echo "[netns] shutdown -- stopping children: ${PIDS[*]:-none}"
        for pid in "${PIDS[@]:-}"; do
            [[ -z "$pid" ]] && continue
            kill -TERM "$pid" 2>/dev/null || true
        done
        sleep 1
        for pid in "${PIDS[@]:-}"; do
            [[ -z "$pid" ]] && continue
            kill -KILL "$pid" 2>/dev/null || true
        done
        cmd_down
        echo "[netns] logs in $LOG_DIR"
    }
    trap cleanup EXIT INT TERM

    cmd_up

    # ----- env passed to the two binaries -----
    SERVER_MODE="${HFT_SERVER_MODE:-4}"
    SERVER_SPEED="${HFT_SERVER_SPEED:-1.0}"
    SERVER_FEED_PORT="${HFT_SERVER_FEED_PORT:-5000}"
    SERVER_ITCH_FILE="${HFT_SERVER_ITCH_FILE:-data/01302020.NASDAQ_ITCH50}"
    FEED_DELAY_S="${HFT_FEED_DELAY_S:-5}"

    AFXDP_QUEUE="${HFT_AFXDP_QUEUE:-0}"
    AFXDP_FORCE_ZEROCOPY="${HFT_AFXDP_FORCE_ZEROCOPY:-0}"
    # veth + native XDP is iffy; SKB/generic XDP is the safe path here.
    AFXDP_PREFER_SKB="${HFT_AFXDP_PREFER_SKB:-1}"

    echo "[netns] launching server in $NS_SRV (feed -> $MCAST_GROUP:$SERVER_FEED_PORT)"
    ip netns exec "$NS_SRV" env \
        HFT_FEED_MCAST_IFACE_IP="$IP_SRV" \
        HFT_FEED_DELAY_S="$FEED_DELAY_S" \
        "$REPO_ROOT/build/bin/server" \
            "$SERVER_MODE" "$SERVER_FEED_PORT" "$MCAST_GROUP" \
            "$SERVER_ITCH_FILE" "$SERVER_SPEED" \
        > >(stdbuf -oL sed -u 's/^/[server]  /' | tee "$LOG_DIR/server.log") 2>&1 &
    PIDS+=($!)

    # Let the server's listening sockets settle before trading binds AF_XDP.
    sleep 1

    echo "[netns] launching trading in $NS_CLI (AF_XDP iface=$VETH_CLI, joining $MCAST_GROUP)"
    ip netns exec "$NS_CLI" env \
        HFT_AFXDP_IFACE="$VETH_CLI" \
        HFT_AFXDP_QUEUE="$AFXDP_QUEUE" \
        HFT_AFXDP_FORCE_ZEROCOPY="$AFXDP_FORCE_ZEROCOPY" \
        HFT_AFXDP_PREFER_SKB="$AFXDP_PREFER_SKB" \
        HFT_FEED_MCAST_GROUP="$MCAST_GROUP" \
        HFT_FEED_MCAST_IFACE_IP="$IP_CLI" \
        HFT_REWINDER_IP="$IP_SRV" \
        "$REPO_ROOT/build/bin/trading" \
        > >(stdbuf -oL sed -u 's/^/[trading] /' | tee "$LOG_DIR/trading.log") 2>&1 &
    PIDS+=($!)

    echo
    echo "[netns] running. PIDs: ${PIDS[*]}  --  Ctrl-C to stop"
    echo "[netns] logs streaming to $LOG_DIR/{server,trading}.log"
    echo

    # Wait for any child to exit; trap tears down the rest plus the netns.
    while true; do
        for pid in "${PIDS[@]}"; do
            if ! kill -0 "$pid" 2>/dev/null; then
                echo "[netns] PID $pid exited; tearing down"
                exit 1
            fi
        done
        sleep 1
    done
}

case "${1:-run}" in
    up)     cmd_up ;;
    down)   cmd_down ;;
    status) cmd_status ;;
    run)    cmd_run ;;
    *)      echo "usage: $0 {up|down|status|run}" >&2; exit 2 ;;
esac
