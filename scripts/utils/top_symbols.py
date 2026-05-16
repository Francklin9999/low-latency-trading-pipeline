#!/usr/bin/env python3
"""Report the busiest symbols in an ITCH 5.0 file."""
import struct
import sys
from collections import defaultdict
from pathlib import Path

PATH = Path(sys.argv[1] if len(sys.argv) > 1 else "data/01302020.NASDAQ_ITCH50")
TOP_N = int(sys.argv[2] if len(sys.argv) > 2 else 15)

locate_to_ticker = {}
volume = defaultdict(int)
msg_count = defaultdict(int)

with open(PATH, "rb") as f:
    bytes_read = 0
    total_size = PATH.stat().st_size
    next_progress = 1 << 30
    while True:
        hdr = f.read(2)
        if len(hdr) < 2:
            break
        msg_len = struct.unpack(">H", hdr)[0]
        msg = f.read(msg_len)
        if len(msg) < msg_len:
            break
        bytes_read += 2 + msg_len
        if bytes_read >= next_progress:
            print(f"  ...{bytes_read/total_size*100:.1f}%", file=sys.stderr)
            next_progress += 1 << 30

        t = msg[0:1]
        if t == b"R":
            locate = struct.unpack(">H", msg[1:3])[0]
            ticker = msg[11:19].decode("ascii", errors="replace").strip()
            locate_to_ticker[locate] = ticker
        elif t == b"A":
            locate = struct.unpack(">H", msg[1:3])[0]
            msg_count[locate] += 1
        elif t == b"F":
            locate = struct.unpack(">H", msg[1:3])[0]
            msg_count[locate] += 1
        elif t == b"E":
            locate = struct.unpack(">H", msg[1:3])[0]
            shares = struct.unpack(">I", msg[19:23])[0]
            volume[locate] += shares
            msg_count[locate] += 1
        elif t == b"C":
            locate = struct.unpack(">H", msg[1:3])[0]
            shares = struct.unpack(">I", msg[19:23])[0]
            volume[locate] += shares
            msg_count[locate] += 1
        elif t == b"P":
            locate = struct.unpack(">H", msg[1:3])[0]
            shares = struct.unpack(">I", msg[20:24])[0]
            volume[locate] += shares
            msg_count[locate] += 1
        elif t in {b"X", b"D", b"U", b"Q"}:
            locate = struct.unpack(">H", msg[1:3])[0]
            msg_count[locate] += 1

print(f"\nTop {TOP_N} by executed share volume:")
top_vol = sorted(volume.items(), key=lambda kv: -kv[1])[:TOP_N]
for locate, shares in top_vol:
    ticker = locate_to_ticker.get(locate, "?")
    print(
        f"  {ticker:<8} locate={locate} "
        f"shares={shares:,} msgs={msg_count[locate]:,}"
    )

print(f"\nTop {TOP_N} by message count:")
top_msg = sorted(msg_count.items(), key=lambda kv: -kv[1])[:TOP_N]
for locate, n in top_msg:
    ticker = locate_to_ticker.get(locate, "?")
    print(
        f"  {ticker:<8} locate={locate} "
        f"msgs={n:,} shares={volume[locate]:,}"
    )
