#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

mode="${1:-${HFT_SERVER_MODE:-4}}"
speed="${2:-${HFT_SERVER_SPEED:-1.0}}"
feed_port="${HFT_SERVER_FEED_PORT:-5000}"
# Default destination is the admin-scoped multicast group; the feed detects
# 224/4 and switches to multicast transmit automatically. Override with a
# unicast address (e.g. 127.0.0.1) for single-process loopback testing.
feed_ip="${HFT_SERVER_FEED_IP:-239.1.1.1}"
itch_file="${HFT_SERVER_ITCH_FILE:-data/01302020.NASDAQ_ITCH50}"

echo "HFT_SERVER_MODE=$mode"
echo "HFT_SERVER_FEED_PORT=$feed_port"
echo "HFT_SERVER_FEED_IP=$feed_ip"
echo "HFT_SERVER_SPEED=$speed"
echo "HFT_SERVER_ITCH_FILE=$itch_file"
if [[ -n "${HFT_FEED_MCAST_IFACE_IP:-}" ]]; then
    echo "HFT_FEED_MCAST_IFACE_IP=$HFT_FEED_MCAST_IFACE_IP"
fi

exec ./build/bin/server "$mode" "$feed_port" "$feed_ip" "$itch_file" "$speed"
