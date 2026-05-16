#!/usr/bin/env python3
"""Render the three README charts from a dashboard-log compact directory.

Inputs:
    <compact_dir>/order_acks.csv.gz                    (or order_acks_*.csv.gz from new format)
    <compact_dir>/health.csv.gz
    <compact_dir>/mm_stats.csv.gz

Outputs (PNG, single-panel each, ~800x320 @ 100 dpi -> ~30-60 KB):
    <out_dir>/latency_ccdf.png       complementary CDF of the 3 latency legs
    <out_dir>/throughput.png         orders/s, ACKs/s, ITCH msgs/s over time
    <out_dir>/pnl_curves.png         per-symbol realized PnL over time (top-N by abs PnL)

Usage:
    python3 scripts/utils/render_charts.py <compact_dir> [out_dir]

Defaults out_dir to docs/charts/.
"""

import csv
import glob
import gzip
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

DPI = 100
FIGSIZE_WIDE = (8.0, 3.4)


def read_csv_gz(path):
    with gzip.open(path, "rt") as f:
        for row in csv.DictReader(f):
            yield row


def render_latency_ccdf(compact_dir, out_dir):
    """Survival / complementary CDF (1 - F(x)) of the three latency legs.

    Two panels share the same data but answer different questions:

      left  ("operating range")  zoomed to <= p99.99 (1-200 us). This is where
            the system actually lives; the one-way and observer-round-trip legs
            coincide here, so the slower one is dashed to stay visible.
      right ("full tail")        the complete log-log distribution out to the
            13.19 s extreme. The seconds-scale shelf is an observer-scheduling
            artifact (clientSendTs -> ackObserveTs), documented in README Notes
            and annotated here so it is never mistaken for engine latency.

    Splitting avoids the old failure mode where one leg's multi-second artifact
    ate ~85% of a single log axis and squashed the real 1-150 us story.
    """
    from array import array
    cols = {"observerRoundTripNs": array('q'),
            "clientSendToServerRecvNs": array('q'),
            "clientSendToAckObserveNs": array('q')}
    sources = sorted(glob.glob(str(compact_dir / "order_acks_*.csv.gz"))) \
              or sorted(glob.glob(str(compact_dir / "order_acks.csv.gz")))
    if not sources:
        print(f"[charts] no order_acks file in {compact_dir}", file=sys.stderr)
        return
    for src in sources:
        for row in read_csv_gz(src):
            for k, a in cols.items():
                v = row.get(k)
                if v:
                    a.append(int(v))

    obs = np.sort(np.frombuffer(cols["observerRoundTripNs"], dtype=np.int64))
    c2s = np.sort(np.frombuffer(cols["clientSendToServerRecvNs"], dtype=np.int64))
    c2a = np.sort(np.frombuffer(cols["clientSendToAckObserveNs"], dtype=np.int64))
    if obs.size == 0:
        print("[charts] no latency rows", file=sys.stderr)
        return

    # (label, sorted_array, color, linestyle, linewidth)
    legs = [
        ("one-way: client send → server recv", c2s, "#1f77b4", "-",  1.5),
        ("round-trip: order → ACK on wire",    obs, "#2ca02c", "--", 1.8),
        ("full RTT: client send → ACK observed", c2a, "#d62728", "-", 1.5),
    ]

    # Coarse grid for the body; very fine near 1.0 so the extreme tail (n=44.7M
    # supports ~2e-8) actually reaches the y-floor instead of stopping at 1e-4.
    q_zoom = np.linspace(0.0, 0.9999, 400)
    q_full = np.concatenate([
        np.linspace(0.0,    0.99,     150),
        np.linspace(0.99,   0.9999,   150),
        np.linspace(0.9999, 1 - 1e-7, 250),
    ])

    fig, (axL, axR) = plt.subplots(1, 2, figsize=(9.4, 3.5), dpi=DPI)

    for label, a, color, ls, lw in legs:
        n = len(a)
        axL.plot(np.quantile(a, q_zoom) / 1e3, 1 - q_zoom,
                 color=color, ls=ls, lw=lw, label=f"{label}  (n={n:,})")
        axR.plot(np.quantile(a, q_full) / 1e3, 1 - q_full,
                 color=color, ls=ls, lw=lw)

    # Left: operating range.
    axL.set_xscale("log"); axL.set_yscale("log")
    axL.set_xlim(1, 200)
    axL.set_ylim(8e-5, 1)
    axL.set_xlabel("latency (µs, log)")
    axL.set_ylabel("P(X ≥ x)")
    axL.set_title("Operating range (≤ p99.99)")
    axL.grid(True, which="both", alpha=0.25)
    for v in (10, 100):
        axL.axvline(v, color="grey", lw=0.4, alpha=0.5)
    axL.legend(loc="lower left", fontsize=7.0, framealpha=0.92)

    # Right: full tail, including the observer-scheduling artifact.
    axR.set_xscale("log"); axR.set_yscale("log")
    axR.set_xlim(1, 2e7)
    axR.set_ylim(1e-7, 1)
    axR.set_xlabel("latency (µs, log)")
    axR.set_title("Full tail (→ 13.19 s, observer artifact)")
    axR.grid(True, which="both", alpha=0.25)
    axR.axvline(c2s.max() / 1e3, color="#1f77b4", lw=0.8, ls=":", alpha=0.8)
    axR.text(c2s.max() / 1e3, 1.5e-7, " one-way max\n 3.02 ms",
             fontsize=6.5, color="#1f77b4", ha="left", va="bottom")
    axR.axvline(obs.max() / 1e3, color="#555", lw=0.8, ls=":", alpha=0.8)
    axR.text(obs.max() / 1e3, 0.35, "13.19 s\nobserver\ndeschedule ",
             fontsize=6.5, color="#555", ha="right", va="center")

    fig.tight_layout()
    fig.savefig(out_dir / "latency_ccdf.png")
    plt.close(fig)
    print(f"[charts] wrote {out_dir / 'latency_ccdf.png'}")


def segment_indices(monotonic_series):
    """Return list of (start, end_exclusive) index pairs of monotonic-up runs.

    A drop in the cumulative counter marks a process restart -- we split there
    so deltas are only computed within a single live session.
    """
    a = np.asarray(monotonic_series, dtype=np.int64)
    breaks = np.where(np.diff(a) < 0)[0] + 1   # index where new segment starts
    boundaries = np.concatenate([[0], breaks, [len(a)]])
    return [(int(boundaries[i]), int(boundaries[i + 1])) for i in range(len(boundaries) - 1)]


def _rolling_mean(y, w):
    """Centered moving average over a 1-D array (window w samples, w odd-ish).

    Raw 1 s counter-deltas are a seismograph; smoothing over ~30 s shows the
    sustained rate instead of per-sample sampling noise.
    """
    if w <= 1 or y.size < w:
        return y
    k = np.ones(w) / w
    return np.convolve(y, k, mode="same")


def render_throughput(compact_dir, out_dir):
    """Sustained per-second rates of ITCH messages, orders, and ACKs over time.

    health snapshots are cumulative counters; convert to per-second deltas,
    segment on process restart (counter resets), keep only real trading
    sessions, smooth each, and plot on a wall-clock axis. orders/s and ACKs/s
    are 1:1 by construction, so ACKs is drawn dashed on top of orders rather
    than hidden beneath it. Linear y keeps the ~10x ITCH:order spread honest
    (log y turned every momentary dip into a misleading downspike).
    """
    src = compact_dir / "health.csv.gz"
    if not src.exists():
        print(f"[charts] no health file", file=sys.stderr)
        return
    rows = list(read_csv_gz(src))
    if len(rows) < 2:
        return
    ts = np.asarray([int(r["ts"]) for r in rows], dtype=np.int64)
    series_for = {
        "ITCH msgs/s": np.asarray([int(r["feedMessages"] or 0) for r in rows], dtype=np.int64),
        "orders/s":    np.asarray([int(r["orders"] or 0)        for r in rows], dtype=np.int64),
        "ACKs/s":      np.asarray([int(r["acks"] or 0)          for r in rows], dtype=np.int64),
    }
    msgs = series_for["ITCH msgs/s"]
    segs = segment_indices(msgs)

    # Keep only substantial trading sessions: >= 5 min of samples and a mean
    # ITCH rate > 1 k/s. This drops the short warm-up blips that previously
    # showed as stray ticks with an 11 h dead gap before the real session.
    SMOOTH_W = 25   # ~30 s at the ~1.2 s health cadence
    active = []
    for s, e in segs:
        if e - s < 250:
            continue
        d  = np.diff(msgs[s:e])
        dt = np.diff(ts[s:e]) / 1e9
        d  = np.where((d >= 0) & (dt > 0), d / np.maximum(dt, 1e-9), 0)
        if d.size and np.nanmean(d) > 1000:
            active.append((s, e))

    if not active:
        print("[charts] no active throughput segment found", file=sys.stderr)
        return

    t0 = int(ts[active[0][0]])

    fig, ax = plt.subplots(figsize=FIGSIZE_WIDE, dpi=DPI)
    styles = [("ITCH msgs/s", "#1f77b4", "-", 1.2),
              ("orders/s",    "#ff7f0e", "-", 1.2),
              ("ACKs/s",      "#2ca02c", "--", 1.0)]
    for label, color, ls, lw in styles:
        vals = series_for[label]
        xs, ys = [], []
        for (s, e) in active:
            t_seg = ts[s:e]
            dt = np.diff(t_seg) / 1e9
            d  = np.diff(vals[s:e])
            d  = np.where((d >= 0) & (dt > 0), d / np.maximum(dt, 1e-9), np.nan)
            xs.append((t_seg[1:] - t0) / 1e9 / 3600.0)
            ys.append(_rolling_mean(d, SMOOTH_W))
            xs.append(np.asarray([np.nan])); ys.append(np.asarray([np.nan]))
        ax.plot(np.concatenate(xs), np.concatenate(ys),
                label=label, color=color, ls=ls, lw=lw, alpha=0.9)

    ax.set_ylim(bottom=0)
    ax.set_xlabel("hours into trading window (wall clock)")
    ax.set_ylabel("rate (events / s, ~30 s mean)")
    n = len(active)
    ax.set_title(f"Throughput over time — {n} trading session{'s' if n != 1 else ''}")
    ax.grid(True, alpha=0.25)
    ax.legend(loc="upper right", fontsize=8, framealpha=0.9)
    fig.tight_layout()
    fig.savefig(out_dir / "throughput.png")
    plt.close(fig)
    print(f"[charts] wrote {out_dir / 'throughput.png'}  ({n} active / {len(segs)} total)")


def render_pnl_curves(compact_dir, out_dir):
    """Final realized PnL per symbol (ticks). Horizontal bar chart -- much more
    legible than a time series since the trading process restarts during the
    window invalidate cumulative time series anyway.
    """
    src = compact_dir / "mm_stats.csv.gz"
    if not src.exists():
        print(f"[charts] no mm_stats file", file=sys.stderr)
        return
    # Take the LAST snapshot per symbol -- final cumulative PnL of the live session.
    last = {}
    for row in read_csv_gz(src):
        try:
            sym = row["symbol"]
            pnl = int(row["realized_pnl_ticks"] or 0)
            fills = int(row["fills"] or 0)
        except Exception:
            continue
        last[sym] = (pnl, fills)

    if not last:
        return

    # Sort by signed PnL ascending so losses sit at the bottom (visual gravity).
    syms = sorted(last, key=lambda s: last[s][0])
    pnls   = np.asarray([last[s][0] for s in syms], dtype=np.int64) / 1e6  # M ticks
    fills_ = np.asarray([last[s][1] for s in syms], dtype=np.int64)
    colors = ["#d62728" if p < 0 else "#2ca02c" for p in pnls]

    fig, ax = plt.subplots(figsize=(FIGSIZE_WIDE[0], max(3.4, 0.28 * len(syms))), dpi=DPI)
    y = np.arange(len(syms))
    ax.barh(y, pnls, color=colors, edgecolor="none", alpha=0.85)
    ax.axvline(0, color="grey", lw=0.6)
    ax.set_yticks(y)
    ax.set_yticklabels(syms, fontsize=8)
    ax.set_xlabel("realized PnL (millions of ticks)")
    ax.set_title("Per-symbol realized PnL (last live snapshot, all 19 symbols)")
    ax.grid(True, axis="x", alpha=0.25)

    # Reserve right margin for fill-count annotations so they never collide
    # with the y-tick labels on the left.
    xmin = min(0, pnls.min())
    xmax = max(0, pnls.max())
    span = xmax - xmin if (xmax - xmin) > 0 else 1.0
    ax.set_xlim(xmin - 0.02 * span, xmax + 0.18 * span)
    for i, f in enumerate(fills_):
        ax.text(xmax + 0.02 * span, i,
                f"{f:>6,} fills",
                ha="left", va="center",
                fontsize=7, color="#444", family="monospace")

    fig.tight_layout()
    fig.savefig(out_dir / "pnl_curves.png")
    plt.close(fig)
    print(f"[charts] wrote {out_dir / 'pnl_curves.png'}")


def main(compact_dir, out_dir):
    compact_dir = Path(compact_dir)
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    render_latency_ccdf(compact_dir, out_dir)
    render_throughput(compact_dir, out_dir)
    render_pnl_curves(compact_dir, out_dir)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit("usage: render_charts.py <compact_dir> [out_dir]")
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "docs/charts")
