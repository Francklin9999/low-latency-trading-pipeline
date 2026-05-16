#!/usr/bin/env python3
"""Stream a (possibly huge) dashboard_*.txt JSONL log into a handful of small,
gzipped per-kind CSVs that preserve every metric we actually use.

What's kept:
    order_acks_YYYYMMDD_HH.csv.gz   per-hour split of correlated order_ack
                                    records (each file commit-sized)
    order_acks_per_minute.csv.gz    per-minute percentile aggregates of the
                                    three latency legs (tiny; commit-friendly)
    health.csv.gz                   every health snapshot (~1 Hz throughput/loss)
    mm_stats.csv.gz                 every per-symbol mm_stats (~0.5 Hz, flattened)
    lat_stats.csv.gz                every engine-side lat_stats (~0.5 Hz percentiles)
    feed_per_sec.csv.gz             feed packets/messages bucketed to 1 s

What's dropped:
    per-event 'order' / 'ack' telemetry (covered by 'health' deltas + 'order_ack')
    'command', 'service_start'          (kept as counts in summary.json)

Output:
    summary.json    counts of every record kind seen, in/out bytes, file index

Usage:
    python3 scripts/utils/compact_dashboard_log.py <input.txt[.gz]> [out_dir]

Defaults out_dir to '<input>.compact/'. Progress goes to stderr every 1 M lines.
"""

import csv
import gzip
import json
import math
import sys
import time
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path

NS_PER_HOUR = 3_600 * 1_000_000_000
NS_PER_MIN  =    60 * 1_000_000_000


def open_input(path):
    if path.endswith(".gz"):
        return gzip.open(path, "rt", errors="replace")
    return open(path, "r", errors="replace")


def open_csv_gz(path, header):
    fp = gzip.open(path, "wt", newline="", compresslevel=6)
    w = csv.writer(fp)
    w.writerow(header)
    return fp, w


def percentile(sorted_values, p):
    if not sorted_values:
        return None
    if p >= 100:
        return sorted_values[-1]
    idx = min(len(sorted_values) - 1, int(math.ceil(p / 100.0 * len(sorted_values))) - 1)
    return sorted_values[max(0, idx)]


def main(in_path, out_dir):
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)

    # Per-kind writers (single-file)
    he_fp, he_w = open_csv_gz(out / "health.csv.gz", [
        "ts", "feedPackets", "feedMessages", "orders", "acks",
        "ackOk", "ackBadCsum", "ackSeqGap", "lastAckSeq", "transportDrops",
    ])
    mm_fp, mm_w = open_csv_gz(out / "mm_stats.csv.gz", [
        "ts", "symbol", "locate", "quotes", "fills", "inventory",
        "realized_pnl_ticks", "unrealized_pnl_ticks", "quoted_both",
        "skip_no_tob", "skip_crossed", "skip_bad_px", "skip_hard_band",
        "skip_panic", "rate_limited",
    ])
    ls_fp, ls_w = open_csv_gz(out / "lat_stats.csv.gz", [
        "ts", "label", "n", "p50_ns", "p90_ns", "p99_ns", "max_ns",
    ])

    # Order acks: per-hour file rotation, keyed by clientSendTs hour.
    OA_HEADER = [
        "seq", "symbolId", "symbol", "side", "qty", "price", "msgType",
        "orderObserveTs", "clientSendTs", "ackObserveTs", "serverRecvTs", "ackFlag",
        "observerRoundTripNs", "clientSendToAckObserveNs", "clientSendToServerRecvNs",
    ]
    oa_writers = {}  # hour_key (str "YYYYMMDD_HH") -> (fp, writer)
    def oa_writer_for(ts_ns):
        if ts_ns is None:
            key = "unknown"
        else:
            dt = datetime.fromtimestamp(int(ts_ns) // 1_000_000_000, tz=timezone.utc)
            key = dt.strftime("%Y%m%d_%H")
        entry = oa_writers.get(key)
        if entry is None:
            fp, w = open_csv_gz(out / f"order_acks_{key}.csv.gz", OA_HEADER)
            entry = (fp, w)
            oa_writers[key] = entry
        return entry[1]

    # Per-minute latency aggregation. We hold raw arrays per (minute, leg) and
    # compute percentiles at the end. Memory ~ O(records); typical 10h run with
    # 25M order_acks at 3 floats each ~ 600 MB peak. If that's tight, swap in
    # a reservoir sampler -- but on a dev box this is fine.
    per_min = defaultdict(lambda: ([], [], []))  # min_key (int) -> (obs_rtt, c2s, c2ack)

    counts = defaultdict(int)
    feed_bucket = defaultdict(lambda: [0, 0])  # ts_s -> [packets, msgs]
    bytes_in = 0
    t0 = time.time()

    def g(d, *keys):
        for k in keys:
            if k in d and d[k] is not None:
                return d[k]
        return None

    with open_input(in_path) as f:
        for i, line in enumerate(f, 1):
            bytes_in += len(line)
            try:
                rec = json.loads(line)
            except Exception:
                counts["bad_json"] += 1
                continue

            kind = rec.get("kind")

            if kind == "order_ack":
                counts["order_ack"] += 1
                send_ts = rec.get("clientSendTs")
                w = oa_writer_for(send_ts)
                w.writerow([
                    rec.get("seq"), rec.get("symbolId"), rec.get("symbol"),
                    rec.get("side"), rec.get("qty"), rec.get("price"), rec.get("msgType"),
                    rec.get("orderObserveTs"), send_ts,
                    rec.get("ackObserveTs"), rec.get("serverRecvTs"), rec.get("ackFlag"),
                    rec.get("observerRoundTripNs"),
                    rec.get("clientSendToAckObserveNs"),
                    rec.get("clientSendToServerRecvNs"),
                ])
                if isinstance(send_ts, (int, float)):
                    mkey = int(send_ts) // NS_PER_MIN
                    obs, c2s, c2a = per_min[mkey]
                    v = rec.get("observerRoundTripNs")
                    if isinstance(v, (int, float)): obs.append(int(v))
                    v = rec.get("clientSendToServerRecvNs")
                    if isinstance(v, (int, float)): c2s.append(int(v))
                    v = rec.get("clientSendToAckObserveNs")
                    if isinstance(v, (int, float)): c2a.append(int(v))

            elif kind == "telemetry":
                ev = rec.get("event") or {}
                t = ev.get("type")
                counts[f"telemetry.{t}"] += 1

                if t == "health":
                    he_w.writerow([
                        ev.get("ts"),
                        g(ev, "feedPackets", "feed_packets"),
                        g(ev, "feedMessages", "feed_messages"),
                        ev.get("orders"), ev.get("acks"),
                        g(ev, "ackOk", "ack_ok"),
                        g(ev, "ackBadCsum", "ack_bad_csum"),
                        g(ev, "ackSeqGap", "ack_seq_gap"),
                        g(ev, "lastAckSeq", "last_ack_seq"),
                        g(ev, "transportDrops", "transport_drops"),
                    ])

                elif t == "mm_stats":
                    ts = ev.get("ts")
                    for s in ev.get("symbols", []) or []:
                        mm_w.writerow([
                            ts, s.get("symbol"), s.get("locate"),
                            s.get("quotes"), s.get("fills"), s.get("inventory"),
                            s.get("realized_pnl_ticks"), s.get("unrealized_pnl_ticks"),
                            s.get("quoted_both"),
                            s.get("skip_no_tob"), s.get("skip_crossed"),
                            s.get("skip_bad_px"), s.get("skip_hard_band"),
                            s.get("skip_panic"), s.get("rate_limited"),
                        ])

                elif t == "lat_stats":
                    ls_w.writerow([
                        ev.get("ts"), ev.get("label"), ev.get("n"),
                        ev.get("p50_ns"), ev.get("p90_ns"),
                        ev.get("p99_ns"), ev.get("max_ns"),
                    ])

                elif t == "feed":
                    ts_ns = ev.get("ts")
                    if isinstance(ts_ns, (int, float)):
                        ts_s = int(ts_ns) // 1_000_000_000
                        b = feed_bucket[ts_s]
                        b[0] += 1
                        b[1] += int(ev.get("msgCount") or 0)
                # 'order' / 'ack' per-event telemetry -> drop (covered)

            else:
                counts[f"kind.{kind or 'unknown'}"] += 1

            if i % 1_000_000 == 0:
                rate_mb_s = (bytes_in / max(1e-6, time.time() - t0)) / 1e6
                print(f"  {i:>13,} lines  {bytes_in/1e9:6.2f} GB read  "
                      f"({rate_mb_s:5.1f} MB/s)", file=sys.stderr)

    he_fp.close(); mm_fp.close(); ls_fp.close()
    for fp, _ in oa_writers.values():
        fp.close()

    # Flush per-second feed buckets.
    with gzip.open(out / "feed_per_sec.csv.gz", "wt", newline="", compresslevel=6) as fp:
        w = csv.writer(fp)
        w.writerow(["ts_s", "feed_packets", "feed_messages"])
        for ts_s in sorted(feed_bucket):
            b = feed_bucket[ts_s]
            w.writerow([ts_s, b[0], b[1]])

    # Per-minute latency percentile aggregates (sort once per bucket).
    with gzip.open(out / "order_acks_per_minute.csv.gz", "wt", newline="", compresslevel=6) as fp:
        w = csv.writer(fp)
        w.writerow([
            "minute_ts_s", "n",
            "obs_rtt_p50", "obs_rtt_p90", "obs_rtt_p99", "obs_rtt_p999", "obs_rtt_max",
            "c2s_p50",     "c2s_p90",     "c2s_p99",     "c2s_p999",     "c2s_max",
            "c2ack_p50",   "c2ack_p90",   "c2ack_p99",   "c2ack_p999",   "c2ack_max",
        ])
        for mkey in sorted(per_min):
            obs, c2s, c2a = per_min[mkey]
            obs.sort(); c2s.sort(); c2a.sort()
            n = max(len(obs), len(c2s), len(c2a))
            row = [mkey * 60, n]
            for arr in (obs, c2s, c2a):
                row += [
                    percentile(arr, 50), percentile(arr, 90),
                    percentile(arr, 99), percentile(arr, 99.9),
                    percentile(arr, 100),
                ]
            w.writerow(row)

    output_files = {p.name: p.stat().st_size for p in out.iterdir() if p.is_file()}
    out_total = sum(output_files.values())

    summary = {
        "input": in_path,
        "input_bytes": bytes_in,
        "output_dir": str(out),
        "output_bytes_total": out_total,
        "compression_ratio": round(bytes_in / max(1, out_total), 2),
        "counts": dict(sorted(counts.items())),
        "outputs": dict(sorted(output_files.items())),
        "elapsed_s": round(time.time() - t0, 2),
    }
    (out / "summary.json").write_text(json.dumps(summary, indent=2, default=str))

    print(file=sys.stderr)
    print(f"[compact] input        {bytes_in:>15,} bytes", file=sys.stderr)
    print(f"[compact] output       {out_total:>15,} bytes  "
          f"({100 * out_total / max(1, bytes_in):.2f}% of input)", file=sys.stderr)
    print(f"[compact] ratio        {bytes_in / max(1, out_total):>15.1f}x", file=sys.stderr)
    print(f"[compact] order_acks hourly files: {sum(1 for k in output_files if k.startswith('order_acks_2'))}", file=sys.stderr)
    print(f"[compact] output dir   {out}", file=sys.stderr)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit("usage: compact_dashboard_log.py <input.txt[.gz]> [out_dir]")
    in_path = sys.argv[1]
    out_dir = sys.argv[2] if len(sys.argv) > 2 else in_path + ".compact"
    main(in_path, out_dir)
