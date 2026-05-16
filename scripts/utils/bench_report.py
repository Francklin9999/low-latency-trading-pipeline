#!/usr/bin/env python3
"""Generate report.md + report.json from a bench_latency.sh run directory.

Reads:
    dashboard_window.jsonl   sliced audit log
    perf_trading.txt         perf stat human output
    proc_trading_{before,after}.txt
    window.json
    preflight.txt

Writes:
    report.md
    report.json
"""

import json
import math
import re
import sys
from pathlib import Path
from statistics import median


def percentiles(values, ps=(50, 90, 99, 99.9, 99.99)):
    if not values:
        return {f"p{p}": None for p in ps}
    s = sorted(values)
    n = len(s)
    out = {}
    for p in ps:
        if p >= 100:
            idx = n - 1
        else:
            idx = min(n - 1, int(math.ceil(p / 100.0 * n)) - 1)
            if idx < 0:
                idx = 0
        out[f"p{p}"] = s[idx]
    return out


def stats(values):
    if not values:
        return {"n": 0}
    n = len(values)
    mean = sum(values) / n
    var = sum((x - mean) ** 2 for x in values) / n
    return {
        "n": n,
        "min": min(values),
        "max": max(values),
        "mean": mean,
        "stddev": math.sqrt(var),
        **percentiles(values),
    }


def fmt_ns(v):
    if v is None:
        return "--"
    if v < 1_000:
        return f"{v:.0f} ns"
    if v < 1_000_000:
        return f"{v/1_000:.2f} us"
    if v < 1_000_000_000:
        return f"{v/1_000_000:.2f} ms"
    return f"{v/1_000_000_000:.2f} s"


def fmt_int(v):
    if v is None:
        return "--"
    return f"{int(v):,}"


def load_jsonl(path):
    rows = []
    if not path.exists():
        return rows
    with path.open("r", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except Exception:
                continue
    return rows


def parse_perf(path):
    """Parse the human-format `perf stat` output. Returns dict event -> count."""
    out = {}
    if not path.exists():
        return out
    # Lines look like:
    #          12,345,678      cycles
    line_re = re.compile(r"^\s*([0-9.,]+)\s+([\w\-./:]+)")
    with path.open("r", errors="replace") as f:
        for line in f:
            line = line.split("#", 1)[0]  # drop trailing comments
            m = line_re.match(line)
            if not m:
                continue
            raw, evt = m.groups()
            if "<not" in raw:
                continue
            try:
                val = int(raw.replace(",", ""))
            except ValueError:
                try:
                    val = float(raw.replace(",", ""))
                except ValueError:
                    continue
            out[evt] = val
    return out


def parse_proc_status(path):
    """Pull a few interesting fields out of /proc/<pid>/{status,io,sched}."""
    out = {}
    if not path.exists():
        return out
    want = {
        "voluntary_ctxt_switches",
        "nonvoluntary_ctxt_switches",
        "VmRSS",
        "VmHWM",
        "VmPeak",
        "HugetlbPages",
        "rchar",
        "wchar",
        "read_bytes",
        "write_bytes",
        "nr_voluntary_switches",
        "nr_involuntary_switches",
        "nr_migrations",
    }
    with path.open("r", errors="replace") as f:
        for line in f:
            for w in want:
                if line.startswith(w):
                    # status fmt: "Key: value kB"; sched fmt: "key  :  value"
                    parts = re.split(r"[:\s]+", line.strip(), maxsplit=1)
                    if len(parts) == 2:
                        v = parts[1].split()[0]
                        try:
                            out[w] = int(v)
                        except ValueError:
                            out[w] = v
    return out


def main(bench_dir):
    d = Path(bench_dir)
    window = json.loads((d / "window.json").read_text())
    duration_s = window["duration_s"]

    rows = load_jsonl(d / "dashboard_window.jsonl")

    # Bucket records by kind/type.
    order_acks = []
    feed_ts = []        # ns timestamps of observer-seen feed packets
    order_ts = []       # ns timestamps of observer-seen orders (any)
    ack_ts = []
    health = []
    lat_stats = []
    mm_stats_last = {}

    for r in rows:
        if r.get("kind") == "order_ack":
            order_acks.append(r)
            continue
        ev = r.get("event") or {}
        t = ev.get("type")
        if t == "feed" and isinstance(ev.get("ts"), (int, float)):
            feed_ts.append(int(ev["ts"]))
        elif t == "order" and isinstance(ev.get("ts"), (int, float)):
            order_ts.append(int(ev["ts"]))
        elif t == "ack" and isinstance(ev.get("ts"), (int, float)):
            ack_ts.append(int(ev["ts"]))
        elif t == "health":
            health.append(ev)
        elif t == "lat_stats":
            lat_stats.append(ev)
        elif t == "mm_stats":
            mm_stats_last = ev

    # ----- latency legs --------------------------------------------------
    obs_rtt   = [r["observerRoundTripNs"]      for r in order_acks if r.get("observerRoundTripNs") is not None]
    c2s       = [r["clientSendToServerRecvNs"] for r in order_acks if r.get("clientSendToServerRecvNs") is not None]
    c2ack     = [r["clientSendToAckObserveNs"] for r in order_acks if r.get("clientSendToAckObserveNs") is not None]

    # Approximate tick-to-trade: for each order_ack, find the latest feed ts
    # observed at or before its clientSendTs (proxy for "engine reaction time
    # from packet-on-wire to order-on-wire"). Coarse, but actionable.
    feed_ts.sort()
    import bisect
    t2t = []
    if feed_ts:
        for r in order_acks:
            send = r.get("clientSendTs")
            if send is None:
                continue
            i = bisect.bisect_right(feed_ts, send) - 1
            if i >= 0:
                t2t.append(send - feed_ts[i])

    lat = {
        "observer_round_trip_ns":          stats(obs_rtt),
        "client_send_to_server_recv_ns":   stats(c2s),
        "client_send_to_ack_observe_ns":   stats(c2ack),
        "tick_to_trade_approx_ns":         stats(t2t),
    }

    # ----- throughput / drops / gaps ------------------------------------
    health_sorted = sorted(health, key=lambda e: e.get("ts", 0))
    throughput = {}
    drops = {}
    if len(health_sorted) >= 2:
        first, last = health_sorted[0], health_sorted[-1]
        span_ns = max(1, last["ts"] - first["ts"])
        span_s  = span_ns / 1e9
        def delta(k):
            return max(0, int(last.get(k, 0)) - int(first.get(k, 0)))
        throughput = {
            "duration_s_health":   round(span_s, 3),
            "feed_packets_total":  delta("feedPackets") if "feedPackets" in last else delta("feed_packets"),
            "feed_messages_total": delta("feedMessages") if "feedMessages" in last else delta("feed_messages"),
            "orders_total":        delta("orders"),
            "acks_total":          delta("acks"),
        }
        for k, v in list(throughput.items()):
            if k.endswith("_total"):
                throughput[k.replace("_total", "_per_s")] = round(v / span_s, 2)
        drops = {
            "transport_drops": delta("transportDrops") if "transportDrops" in last else delta("transport_drops"),
            "ack_seq_gap":     delta("ackSeqGap") if "ackSeqGap" in last else delta("ack_seq_gap"),
            "ack_bad_csum":    delta("ackBadCsum") if "ackBadCsum" in last else delta("ack_bad_csum"),
        }

    # Peak 1s order rate within the window.
    peak_order_per_s = 0
    if order_ts:
        bucket = {}
        for t in order_ts:
            s = t // 1_000_000_000
            bucket[s] = bucket.get(s, 0) + 1
        peak_order_per_s = max(bucket.values())

    # ----- perf counters -------------------------------------------------
    perf = parse_perf(d / "perf_trading.txt")
    cyc = perf.get("cycles")
    ins = perf.get("instructions")
    cm  = perf.get("cache-misses")
    cr  = perf.get("cache-references")
    bm  = perf.get("branch-misses")
    br  = perf.get("branch-instructions")
    cs  = perf.get("context-switches")

    perf_derived = {
        "ipc":                      (ins / cyc) if (cyc and ins) else None,
        "cache_miss_rate":          (cm / cr) if (cr and cm) else None,
        "branch_miss_rate":         (bm / br) if (br and bm) else None,
        "ctxt_switches_per_s":      (cs / duration_s) if cs else None,
    }
    # Per-order amortization (only meaningful if any orders went out).
    n_orders = throughput.get("orders_total") or 0
    if n_orders and cyc:
        perf_derived["cycles_per_order"]       = cyc / n_orders
    if n_orders and ins:
        perf_derived["instructions_per_order"] = ins / n_orders

    proc_before = parse_proc_status(d / "proc_trading_before.txt")
    proc_after  = parse_proc_status(d / "proc_trading_after.txt")
    proc_delta = {}
    for k in set(proc_before) | set(proc_after):
        bv = proc_before.get(k); av = proc_after.get(k)
        if isinstance(av, int) and isinstance(bv, int):
            proc_delta[k] = av - bv

    summary = {
        "window": window,
        "throughput": throughput,
        "drops": drops,
        "peak_order_per_s": peak_order_per_s,
        "latency": lat,
        "perf": perf,
        "perf_derived": perf_derived,
        "proc_delta": proc_delta,
    }

    (d / "report.json").write_text(json.dumps(summary, indent=2, default=str))

    # ----- markdown report ---------------------------------------------
    md = []
    md.append(f"# HFT bench -- {d.name}\n")
    md.append(f"- window: **{duration_s}s** (warmup {window['warmup_s']}s discarded)")
    md.append(f"- trading pid: {window['trading_pid']}\n")

    md.append("## Throughput\n")
    if throughput:
        md.append(f"| metric | total | per-second |\n|---|---:|---:|")
        md.append(f"| feed packets   | {fmt_int(throughput.get('feed_packets_total'))} | {throughput.get('feed_packets_per_s','--')} |")
        md.append(f"| feed messages  | {fmt_int(throughput.get('feed_messages_total'))} | {throughput.get('feed_messages_per_s','--')} |")
        md.append(f"| orders sent    | {fmt_int(throughput.get('orders_total'))} | {throughput.get('orders_per_s','--')} |")
        md.append(f"| acks received  | {fmt_int(throughput.get('acks_total'))} | {throughput.get('acks_per_s','--')} |")
        md.append(f"\n- peak 1s order rate: **{peak_order_per_s:,}** orders/s")
    else:
        md.append("_no health events in window_")

    md.append("\n## Loss\n")
    if drops:
        md.append(f"- transport drops:  {fmt_int(drops.get('transport_drops'))}")
        md.append(f"- ACK seq gaps:     {fmt_int(drops.get('ack_seq_gap'))}")
        md.append(f"- ACK bad checksum: {fmt_int(drops.get('ack_bad_csum'))}")
    else:
        md.append("_n/a_")

    md.append("\n## Latency (percentiles)\n")
    md.append("| leg | n | p50 | p90 | p99 | p99.9 | p99.99 | max | stddev |")
    md.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for label, key in [
        ("observer round-trip",            "observer_round_trip_ns"),
        ("client send -> server recv", "client_send_to_server_recv_ns"),
        ("client send -> ACK observed","client_send_to_ack_observe_ns"),
        ("tick -> trade (approx)",     "tick_to_trade_approx_ns"),
    ]:
        s = lat[key]
        if not s.get("n"):
            md.append(f"| {label} | 0 | -- | -- | -- | -- | -- | -- | -- |")
            continue
        md.append("| {l} | {n} | {p50} | {p90} | {p99} | {p999} | {p9999} | {mx} | {sd} |".format(
            l=label, n=fmt_int(s["n"]),
            p50=fmt_ns(s["p50"]), p90=fmt_ns(s["p90"]), p99=fmt_ns(s["p99"]),
            p999=fmt_ns(s["p99.9"]), p9999=fmt_ns(s["p99.99"]),
            mx=fmt_ns(s["max"]), sd=fmt_ns(s["stddev"]),
        ))
    if not feed_ts:
        md.append("\n> _tick-to-trade column empty because no observer `feed` events were seen in this window._")

    md.append("\n## CPU (trading process)\n")
    if perf:
        md.append(f"| counter | value |\n|---|---:|")
        for k in ("cycles","instructions","cache-references","cache-misses",
                  "branch-instructions","branch-misses","context-switches",
                  "cpu-migrations","page-faults"):
            if k in perf:
                md.append(f"| {k} | {fmt_int(perf[k])} |")
        md.append("")
        if perf_derived:
            md.append("### Derived\n")
            md.append(f"| metric | value |\n|---|---:|")
            for k, v in perf_derived.items():
                if v is None: continue
                if "rate" in k or k == "ipc":
                    md.append(f"| {k} | {v:.4f} |")
                else:
                    md.append(f"| {k} | {v:,.2f} |")
    else:
        md.append("_no perf data (perf stat failed or unavailable)_")

    md.append("\n## /proc/<pid> delta\n")
    if proc_delta:
        md.append("| field | delta |\n|---|---:|")
        for k in sorted(proc_delta):
            md.append(f"| {k} | {fmt_int(proc_delta[k])} |")
    else:
        md.append("_n/a_")

    pre = (d / "preflight.txt")
    md.append("\n## Preflight (host state)\n")
    md.append("```\n" + (pre.read_text() if pre.exists() else "(missing)") + "\n```")

    (d / "report.md").write_text("\n".join(md) + "\n")
    print(f"[report] wrote {d / 'report.md'} and {d / 'report.json'}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: bench_report.py <bench_dir>")
    main(sys.argv[1])
