import mmap
import struct
import time
import os
import sys
import glob
from collections import deque

# ─── Layout constants — must match C headers ─────────────────────────────────

# Both ring buffers use alignas(64) for cursors:
#   next_write at byte 0, next_read at byte 64, data[] at byte 128
CURSOR_WRITE_OFF = 0
CURSOR_READ_OFF  = 64
DATA_OFF         = 128

# event_to_engine  (event_to_engine.h)
EVENT_RING_SIZE = 512 * 1024           # EVENT_TO_ENGINE_SIZE
EVENT_RING_MASK = EVENT_RING_SIZE - 1
EVENT_SIZE      = 32                   # sizeof(event) aligned(32)

# order_to_exc  (order_to_exc.h)
ORDER_RING_SIZE = 512 * 1024           # ORDER_TO_EXC_SIZE
ORDER_RING_MASK = ORDER_RING_SIZE - 1
ORDER_SIZE      = 64                   # cache-line aligned

# order struct layout (little-endian):
#   +0   uint64  order_id
#   +8   uint64  ts          ← steady_clock::now() at OMS submit
#   +16  uint32  price
#   +20  uint32  qty
#   +24  uint16  stock_locate
#   +26  uint8   side
ORDER_TS_FIELD_OFF = 8

SHM_EVENT = "/dev/shm/hft_event_to_strat"
SHM_ORDER = "/dev/shm/order_to_exc"

INTERVAL  = float(sys.argv[1]) if len(sys.argv) > 1 else 0.5
HIST_SIZE = 2000   # rolling window of inter-order intervals

# ─── ANSI ─────────────────────────────────────────────────────────────────────
CLEAR  = '\033[2J\033[H'
BOLD   = '\033[1m'
DIM    = '\033[2m'
CYAN   = '\033[96m'
GREEN  = '\033[92m'
YELLOW = '\033[93m'
RED    = '\033[91m'
RESET  = '\033[0m'

# ─── Shared memory helpers ────────────────────────────────────────────────────

def open_shm(path):
    try:
        fd = open(path, 'rb')
        mm = mmap.mmap(fd.fileno(), 0, access=mmap.ACCESS_READ)
        return fd, mm
    except Exception:
        return None, None

def read_u32(mm, off):
    mm.seek(off)
    return struct.unpack('<I', mm.read(4))[0]

def read_u64(mm, off):
    mm.seek(off)
    return struct.unpack('<Q', mm.read(8))[0]

def ring_backlog(write, read):
    return (write - read) & 0xFFFFFFFF

# ─── Process helpers ──────────────────────────────────────────────────────────

def find_pid(name):
    try:
        out = os.popen(f'pgrep -x {name} 2>/dev/null').read().strip()
        pids = [int(x) for x in out.split() if x]
        return pids[0] if pids else None
    except Exception:
        return None

def proc_stat(pid):
    """Return (utime+stime jiffies, rss_kb)."""
    try:
        with open(f'/proc/{pid}/stat') as f:
            fields = f.read().split()
        jiffies = int(fields[13]) + int(fields[14])
        rss_kb  = int(fields[23]) * 4          # pages → KB
        return jiffies, rss_kb
    except Exception:
        return 0, 0

def proc_schedstat(pid):
    """Return (cpu_ns, wait_ns) summed over all threads."""
    cpu_ns = wait_ns = 0
    try:
        for path in glob.glob(f'/proc/{pid}/task/*/schedstat'):
            with open(path) as f:
                p = f.read().split()
            cpu_ns  += int(p[0])
            wait_ns += int(p[1])
    except Exception:
        pass
    return cpu_ns, wait_ns

# ─── Statistics helpers ───────────────────────────────────────────────────────

def percentile(data, p):
    if not data:
        return 0
    s = sorted(data)
    idx = max(0, min(int(len(s) * p / 100), len(s) - 1))
    return s[idx]

def fmt_ns(ns):
    if ns < 1_000:
        return f"{ns}ns"
    if ns < 1_000_000:
        return f"{ns / 1e3:.1f}µs"
    return f"{ns / 1e6:.2f}ms"

def fmt_rate(delta, dt):
    r = delta / max(dt, 1e-9)
    if r >= 1_000_000:
        return f"{r / 1e6:.2f}M/s"
    if r >= 1_000:
        return f"{r / 1e3:.1f}K/s"
    return f"{r:.0f}/s"

def pct_bar(pct, width=10):
    filled = int(pct / 100 * width)
    bar    = '█' * filled + '░' * (width - filled)
    color  = RED if pct > 90 else YELLOW if pct > 60 else GREEN
    return f"{color}{bar}{RESET}"

def backlog_color(bl, ring_size):
    pct = bl / ring_size
    if pct > 0.5:
        return RED
    if pct > 0.1:
        return YELLOW
    return GREEN

# ─── Main loop ────────────────────────────────────────────────────────────────

def main():
    evt_fd, evt_mm = open_shm(SHM_EVENT)
    ord_fd, ord_mm = open_shm(SHM_ORDER)

    hz = os.sysconf(os.sysconf_names['SC_CLK_TCK'])

    proc_names = ('server', 'client', 'engine')
    pids       = {n: find_pid(n) for n in proc_names}
    prev_jif   = {n: proc_stat(pids[n])[0] if pids[n] else 0 for n in proc_names}
    prev_sched = {n: proc_schedstat(pids[n]) if pids[n] else (0, 0) for n in proc_names}

    prev_time  = time.monotonic()
    prev_evt_w = read_u32(evt_mm, CURSOR_WRITE_OFF) if evt_mm else 0
    prev_evt_r = read_u32(evt_mm, CURSOR_READ_OFF)  if evt_mm else 0
    prev_ord_w = read_u32(ord_mm, CURSOR_WRITE_OFF) if ord_mm else 0
    prev_ord_r = read_u32(ord_mm, CURSOR_READ_OFF)  if ord_mm else 0

    inter_order_ns = deque(maxlen=HIST_SIZE)
    last_order_ts  = None

    print(f"Watching HFT ring buffers — interval={INTERVAL}s — Ctrl-C to quit")
    time.sleep(INTERVAL)

    while True:
        now = time.monotonic()
        dt  = now - prev_time
        prev_time = now

        # Re-open shm if processes restarted
        if evt_mm is None:
            evt_fd, evt_mm = open_shm(SHM_EVENT)
        if ord_mm is None:
            ord_fd, ord_mm = open_shm(SHM_ORDER)

        # Re-detect pids
        for n in proc_names:
            pids[n] = find_pid(n)

        # ── Ring buffer cursors ───────────────────────────────────────────────
        if evt_mm:
            evt_w = read_u32(evt_mm, CURSOR_WRITE_OFF)
            evt_r = read_u32(evt_mm, CURSOR_READ_OFF)
        else:
            evt_w = evt_r = prev_evt_w

        if ord_mm:
            ord_w = read_u32(ord_mm, CURSOR_WRITE_OFF)
            ord_r = read_u32(ord_mm, CURSOR_READ_OFF)
        else:
            ord_w = ord_r = prev_ord_w

        d_evt_w = (evt_w - prev_evt_w) & 0xFFFFFFFF
        d_evt_r = (evt_r - prev_evt_r) & 0xFFFFFFFF
        d_ord_w = (ord_w - prev_ord_w) & 0xFFFFFFFF
        d_ord_r = (ord_r - prev_ord_r) & 0xFFFFFFFF

        evt_bl = ring_backlog(evt_w, evt_r)
        ord_bl = ring_backlog(ord_w, ord_r)

        # ── Collect order timestamps ──────────────────────────────────────────
        if ord_mm and d_ord_w > 0:
            count = min(d_ord_w, ORDER_RING_SIZE)
            for i in range(count):
                slot     = (prev_ord_w + i) & ORDER_RING_MASK
                byte_off = DATA_OFF + slot * ORDER_SIZE + ORDER_TS_FIELD_OFF
                ts_ns    = read_u64(ord_mm, byte_off)
                if ts_ns > 0:
                    if last_order_ts is not None and ts_ns > last_order_ts:
                        inter_order_ns.append(ts_ns - last_order_ts)
                    last_order_ts = ts_ns

        # ── CPU and schedstat ─────────────────────────────────────────────────
        cpu_pct  = {}
        rss_mb   = {}
        wait_ms  = {}   # total thread scheduler wait in this interval
        for n in proc_names:
            pid = pids[n]
            if pid:
                j, rss  = proc_stat(pid)
                dj      = (j - prev_jif.get(n, 0))
                cpu_pct[n] = min((dj / hz / dt) * 100.0, 999.0)
                rss_mb[n]  = rss / 1024
                prev_jif[n] = j

                cpu_ns, w_ns = proc_schedstat(pid)
                _, prev_w_ns = prev_sched.get(n, (0, 0))
                wait_ms[n]   = (w_ns - prev_w_ns) / 1e6
                prev_sched[n] = (cpu_ns, w_ns)
            else:
                cpu_pct[n] = rss_mb[n] = wait_ms[n] = 0

        # ── Render ────────────────────────────────────────────────────────────
        W   = 64
        sep = f"{BOLD}{CYAN}{'─' * W}{RESET}"
        out = [CLEAR]

        out.append(sep)
        out.append(f"{BOLD}  HFT LATENCY MONITOR"
                   f"  {DIM}{time.strftime('%H:%M:%S')}  "
                   f"interval={INTERVAL}s{RESET}")
        out.append(sep)

        # Event ring
        eb_col = backlog_color(evt_bl, EVENT_RING_SIZE)
        out.append(f"\n{BOLD}EVENT RING{RESET}  /dev/shm/{SHM_EVENT.split('/')[-1]}")
        if evt_mm:
            out.append(f"  cursors  write={evt_w:<12} read={evt_r:<12} "
                       f"backlog={eb_col}{evt_bl}{RESET}")
            out.append(f"  rate     produced={fmt_rate(d_evt_w, dt):>10}   "
                       f"consumed={fmt_rate(d_evt_r, dt):>10}")
            lag = d_evt_w - d_evt_r
            if lag > 0:
                out.append(f"  {YELLOW}engine falling behind by {lag} events this interval{RESET}")
        else:
            out.append(f"  {RED}not found — start client + engine first{RESET}")

        # Order ring
        ob_col = backlog_color(ord_bl, ORDER_RING_SIZE)
        out.append(f"\n{BOLD}ORDER RING{RESET}  /dev/shm/{SHM_ORDER.split('/')[-1]}")
        if ord_mm:
            out.append(f"  cursors  write={ord_w:<12} read={ord_r:<12} "
                       f"backlog={ob_col}{ord_bl}{RESET}")
            out.append(f"  rate     submitted={fmt_rate(d_ord_w, dt):>9}   "
                       f"sent={fmt_rate(d_ord_r, dt):>13}")
            lag = d_ord_w - d_ord_r
            if lag > 0:
                out.append(f"  {YELLOW}order_sender falling behind by {lag} orders this interval{RESET}")
        else:
            out.append(f"  {RED}not found — start engine first{RESET}")

        # Inter-order latency
        out.append(f"\n{BOLD}INTER-ORDER LATENCY{RESET}  "
                   f"{DIM}time between consecutive OMS submissions "
                   f"(rolling {HIST_SIZE} samples){RESET}")
        if inter_order_ns:
            d   = list(inter_order_ns)
            avg = int(sum(d) / len(d))
            out.append(f"  samples={len(d)}")
            out.append(f"  {'min':>6} = {fmt_ns(min(d)):>10}   "
                       f"{'avg':>6} = {fmt_ns(avg):>10}   "
                       f"{'max':>6} = {fmt_ns(max(d)):>10}")
            out.append(f"  {'p50':>6} = {fmt_ns(percentile(d, 50)):>10}   "
                       f"{'p95':>6} = {fmt_ns(percentile(d, 95)):>10}   "
                       f"{'p99':>6} = {fmt_ns(percentile(d, 99)):>10}")
        else:
            out.append(f"  {DIM}waiting for orders...{RESET}")

        # Process stats
        out.append(f"\n{BOLD}PROCESSES{RESET}")
        out.append(f"  {'name':<8}  {'pid':<7}  {'cpu':>6}  {'bar':^12}  "
                   f"{'rss':>7}  {'sched-wait':>12}")
        for n in proc_names:
            pid = pids[n]
            if pid:
                cpu   = cpu_pct[n]
                bar   = pct_bar(cpu)
                jc    = RED if wait_ms[n] > 100 else YELLOW if wait_ms[n] > 10 else GREEN
                out.append(
                    f"  {n:<8}  {pid:<7}  "
                    f"{(RED if cpu>90 else YELLOW if cpu>60 else GREEN)}"
                    f"{cpu:5.1f}%{RESET}  "
                    f"{bar}  "
                    f"{rss_mb[n]:5.1f}MB  "
                    f"{jc}{wait_ms[n]:8.1f}ms{RESET}"
                )
            else:
                out.append(f"  {n:<8}  {DIM}not running{RESET}")

        out.append(f"\n{DIM}inter-order latency ≠ pipeline latency — "
                   f"add recv_ns to event struct for true end-to-end measurement{RESET}")
        out.append(sep)

        print('\n'.join(out), end='', flush=True)

        prev_evt_w, prev_evt_r = evt_w, evt_r
        prev_ord_w, prev_ord_r = ord_w, ord_r

        time.sleep(INTERVAL)


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print(f"\n{RESET}Stopped.")
