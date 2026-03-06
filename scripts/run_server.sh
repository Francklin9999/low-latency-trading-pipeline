if [ "$(basename "$PWD")" == "scripts" ]; then
    cd ..
fi

if [ -n "${1:-}" ]; then HFT_SERVER_MODE="$1"; fi
if [ -n "${2:-}" ]; then HFT_SERVER_SPEED="$2"; fi

HFT_ISOLATED_PATH="${HFT_ISOLATED_PATH:-0}"
HFT_ISO_NS_TX="${HFT_ISO_NS_TX:-ns_tx}"
HFT_ISO_RX_ADDR="${HFT_ISO_RX_ADDR:-10.10.0.2}"

SERVER_MODE="${HFT_SERVER_MODE:-4}"
SERVER_FEED_PORT="${HFT_SERVER_FEED_PORT:-5000}"
if [ "$HFT_ISOLATED_PATH" != "0" ]; then
    SERVER_FEED_IP="${HFT_SERVER_FEED_IP:-$HFT_ISO_RX_ADDR}"
else
    SERVER_FEED_IP="${HFT_SERVER_FEED_IP:-127.0.0.1}"
fi
SERVER_ITCH_FILE="${HFT_SERVER_ITCH_FILE:-}"
SERVER_SPEED="${HFT_SERVER_SPEED:-1.0}"

echo "HFT_SERVER_MODE=$SERVER_MODE"
echo "HFT_SERVER_FEED_PORT=$SERVER_FEED_PORT"
echo "HFT_SERVER_FEED_IP=$SERVER_FEED_IP"
echo "HFT_ISOLATED_PATH=$HFT_ISOLATED_PATH"
if [ -n "$SERVER_ITCH_FILE" ]; then
    echo "HFT_SERVER_ITCH_FILE=$SERVER_ITCH_FILE"
fi
echo "HFT_SERVER_SPEED=$SERVER_SPEED"

_itch_arg="${SERVER_ITCH_FILE}"
_args=("$SERVER_MODE" "$SERVER_FEED_PORT" "$SERVER_FEED_IP" "$_itch_arg" "$SERVER_SPEED")

if [ "$HFT_ISOLATED_PATH" != "0" ]; then
    ./scripts/setup_isolated_path.sh
    ip netns exec "$HFT_ISO_NS_TX" ./build/bin/server "${_args[@]}"
else
    ./build/bin/server "${_args[@]}"
fi
