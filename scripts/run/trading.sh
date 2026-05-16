#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

export HFT_AFXDP_IFACE="${HFT_AFXDP_IFACE:-lo}"
export HFT_AFXDP_QUEUE="${HFT_AFXDP_QUEUE:-0}"
export HFT_AFXDP_FORCE_ZEROCOPY="${HFT_AFXDP_FORCE_ZEROCOPY:-0}"
export HFT_AFXDP_PREFER_SKB="${HFT_AFXDP_PREFER_SKB:-1}"

echo "HFT_AFXDP_IFACE=$HFT_AFXDP_IFACE"
echo "HFT_AFXDP_QUEUE=$HFT_AFXDP_QUEUE"
echo "HFT_AFXDP_FORCE_ZEROCOPY=$HFT_AFXDP_FORCE_ZEROCOPY"
echo "HFT_AFXDP_PREFER_SKB=$HFT_AFXDP_PREFER_SKB"

# Multicast / rewinder routing knobs. Unset = unicast loopback (legacy);
# set HFT_FEED_MCAST_GROUP=239.x.x.x to join a group via IGMP and have AF_XDP
# receive its frames. HFT_REWINDER_IP overrides the rewinder unicast target
# (default 127.0.0.1) — needed in any netns / cross-host setup.
[[ -n "${HFT_FEED_MCAST_GROUP:-}" ]]    && echo "HFT_FEED_MCAST_GROUP=$HFT_FEED_MCAST_GROUP"
[[ -n "${HFT_FEED_MCAST_IFACE_IP:-}" ]] && echo "HFT_FEED_MCAST_IFACE_IP=$HFT_FEED_MCAST_IFACE_IP"
[[ -n "${HFT_REWINDER_IP:-}" ]]         && echo "HFT_REWINDER_IP=$HFT_REWINDER_IP"

exec ./build/bin/trading
