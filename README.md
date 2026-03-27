# Low-Latency ITCH-to-Order Pipeline in C/C++

A replay-driven HFT signal pipeline built in C11/C++20 for low-latency experimentation.

It replays NASDAQ ITCH over MoldUDP64, ingests packets through AF_XDP, parses market data into shared-memory rings, runs SIMD batch transforms plus a simple imbalance strategy, applies inline risk checks, and sends generated orders back to a TCP receiver that logs end-to-end latency.

This repository is not a full exchange simulator or production trading stack. It is a focused latency lab for studying feed handling, batch dispatch, signal generation, and order-path timing under a tightly controlled local setup.

## What It Supports

- NASDAQ ITCH replay from `data/01302020.NASDAQ_ITCH50`
- MoldUDP64 UDP feed generation
- AF_XDP packet capture with loopback fallback
- zero-copy-style parser handoff through `packet_ref` descriptors
- shared-memory event and order rings
- AVX2-accelerated batch transforms for hot event paths
- imbalance-driven signal generation
- inline risk checks for rate, size, notional, and per-stock position
- TCP order egress back to the local server
- latency logging split into engine, queue, wire, and total components
- replay modes for normal, lossy, chaotic, and timestamp-driven playback

## Focus Areas

- feed-to-signal latency instead of full exchange completeness
- CPU pinning and predictable thread placement
- cache-friendly batch processing
- low-allocation IPC via shared memory ring buffers
- stage-by-stage latency attribution
- realistic replay stress with packet loss, duplication, and timestamp pacing

## Overview

The system is organized as three cooperating processes:

1. `server`
   Replays an ITCH file as MoldUDP64 over UDP and accepts generated orders over TCP.
2. `client`
   Captures the UDP feed with AF_XDP, parses ITCH messages, pushes normalized events into a shared-memory ring, and sends generated orders to the server.
3. `engine`
   Consumes parsed events, batches them by lane, runs SIMD transforms, applies the imbalance strategy and risk checks, and publishes orders into a second shared-memory ring.

The main loop is:

`ITCH file -> MoldUDP64 UDP -> AF_XDP capture -> parser -> EVENT_ENGINE ring -> dispatcher -> SIMD compute -> imbalance strategy -> risk -> ORDER_TO_EXC ring -> TCP sender -> order receiver -> results/*.txt`

## Motivation

This project isolates a practical subset of an HFT stack:

- market data ingress
- message normalization
- fast-path compute
- order generation
- latency measurement

The goal is to measure where time goes across the pipeline, not to model every exchange rule or venue behavior.

## Architecture

### Core Design

- `apps/server/feed.c` replays ITCH either sequentially or according to embedded message timestamps.
- `apps/client/udp_receiver.c` receives raw frames through AF_XDP, extracts UDP payloads for port `5000`, and publishes `packet_ref` pointers into the parser ring.
- `src/parser/parser_to_engine.c` converts ITCH messages into compact `event` records.
- `apps/engine/dispatcher.cpp` batches events into two compute lanes based on `order_id % NUMBER_OF_DISPATCHERS`.
- `src/engine/cpu/cpu_entry.cpp` uses AVX2 to derive per-event fields such as notional, side-specific quantities, and timestamp deltas.
- `src/engine/strategy/imbalance_strat.cpp` aggregates bid/ask pressure by `stock_locate` and emits buy/sell signals.
- `src/engine/risk/risk.cpp` enforces order-rate, quantity, notional, and per-symbol position limits.
- `src/engine/oms/oms.cpp` writes compact `order` structs into the outbound shared-memory ring.
- `apps/client/order_sender.c` drains the order ring and writes orders to the server over non-blocking TCP.
- `apps/server/order_receiver.c` timestamps received orders and writes sampled latency reports to `results/`.

### Main Shared-Memory Structures

- `PARSER_ENGINE`
  `packet_ref*` ring inside the client process
- `EVENT_ENGINE`
  shared-memory ring of normalized `event` objects
- `ORDER_TO_EXC`
  shared-memory ring of 64-byte `order` objects

Current ring sizes:

- `EVENT_TO_ENGINE_SIZE = 512k`
- `ORDER_TO_EXC_SIZE = 512k`
- `PARSER_TO_ENGINE_SIZE = 512k`

### Batch and Lane Model

- `NUMBER_OF_DISPATCHERS = 2`
- default build uses the balanced batch preset: `BATCH_MAX = 1024`
- alternate latency and throughput presets exist in `include/hft/engine/batch_sizes.hpp`

The current CMake build does not expose a toggle for those presets, so the default build stays on the balanced configuration unless you add compile definitions manually.

### Event Types on the Hot Path

The dispatcher currently handles these ITCH-derived events:

- `A` add order
- `F` add order with MPID
- `E` executed / modify-like quantity update
- `C` executed at price
- `X` cancel
- `D` delete
- `U` replace

### One-Line Data Flow

`AF_XDP frame -> packet_ref -> ITCH handler -> event -> batch lane -> Signal -> risk gate -> order -> TCP log`

## Process Layout

```text
                           +----------------------+
                           |  ITCH replay file    |
                           | data/01302020...     |
                           +----------+-----------+
                                      |
                                      v
                          +-----------------------+
                          | apps/server/feed.c    |
                          | MoldUDP64 over UDP    |
                          +----------+------------+
                                     |
                                     v
                  +-------------------------------------------+
                  | apps/client/udp_receiver.c                |
                  | AF_XDP RX -> packet_ref ring -> parser    |
                  +-------------------+-----------------------+
                                      |
                                      v
                        +-------------------------------+
                        | EVENT_ENGINE shared memory    |
                        | normalized event records      |
                        +---------------+---------------+
                                        |
                                        v
                      +---------------------------------------+
                      | apps/engine/dispatcher.cpp            |
                      | batch -> SIMD compute -> strategy     |
                      | -> risk -> OMS                        |
                      +------------------+--------------------+
                                         |
                                         v
                        +-------------------------------+
                        | ORDER_TO_EXC shared memory    |
                        | compact outbound orders       |
                        +---------------+---------------+
                                        |
                                        v
                     +----------------------------------------+
                     | apps/client/order_sender.c             |
                     | non-blocking TCP -> server receiver    |
                     +------------------+---------------------+
                                        |
                                        v
                         +-------------------------------+
                         | apps/server/order_receiver.c  |
                         | latency report in results/    |
                         +-------------------------------+
```

## Latency Accounting

The server logs every 100th received order to limit measurement overhead.

Example output lives in:

- `results/orders_127.0.0.1_44968_20260306_140247.txt`
- `results/orders_127.0.0.1_37964_20260306_135940.txt`

From the `2026-03-06 14:02:47` run, steady-state sampled orders were commonly in these ranges:

| Stage | Measures | Typical sampled range |
| --- | --- | --- |
| `engine` | ITCH parse timestamp -> OMS enqueue | `0.9 us` to `6.7 us` |
| `queue` | OMS enqueue -> client TCP send | `0.24 us` to `0.8 us` |
| `wire` | client send -> server receive | `40 us` to `58 us` |
| `total` | parse -> final receive | `43 us` to `58 us` |

The same run also shows occasional multi-millisecond engine spikes, which is consistent with scheduler noise, replay burstiness, or transient backpressure.

## Build

### Prerequisites

- Linux
- CMake `>= 3.16`
- a C11/C++20-capable compiler
- `libxdp`
- `libbpf`
- `libelf`
- `zlib`
- `pthread`
- `clang` if you want the custom XDP filter object built automatically

The CMake file also supports a local fallback layout via:

- `XDP_TUTORIAL_ROOT=../xdp-tutorial`

If the required XDP or BPF headers are missing, configuration fails early.

### Configure and Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Built binaries are written to:

- `build/bin/server`
- `build/bin/client`
- `build/bin/engine`

### Pinning Configuration

Thread/core placement is configured through CMake cache variables:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DHFT_SERVER_FEED_CORE=2 \
  -DHFT_SERVER_ORDER_RECV_CORE=3 \
  -DHFT_CLIENT_UDP_RECV_CORE=4 \
  -DHFT_CLIENT_ORDER_SEND_CORE=5 \
  -DHFT_ENGINE_DISPATCH_CORE=6 \
  -DHFT_ENGINE_LANE1_CORE=7 \
  -DHFT_ENGINE_RESET_CORE=8 \
  -DHFT_ENGINE_RESET_LANE1_CORE=9
```

At the moment, `server` and `client` use their configured thread affinities directly, while the `engine` code actively pins only the dispatcher thread. The extra engine core cache entries are present for the lane/reset layout the code is clearly evolving toward, but they are not all consumed by the current implementation yet.

## Run

### Recommended Launch Order

Start the engine first so the shared-memory regions exist before the client tries to open them.

Terminal 1:

```bash
./build/bin/engine
```

Terminal 2:

```bash
sudo env HFT_AFXDP_IFACE=lo HFT_AFXDP_PREFER_SKB=1 ./build/bin/client
```

Terminal 3:

```bash
./build/bin/server 4 5000 127.0.0.1 data/01302020.NASDAQ_ITCH50 1.0
```

That command starts:

- server mode `4` = timestamp replay
- UDP destination `127.0.0.1:5000`
- replay file `data/01302020.NASDAQ_ITCH50`
- speed `1.0x`

### Server Modes

- `1` normal sequential replay
- `2` lossy mode, drops every 100th packet
- `3` chaotic mode, duplicates every 10th packet
- `4` timestamp replay mode with configurable speed

Modes `1` to `3` intentionally wait `30` seconds before sending so you have time to start the rest of the pipeline. Mode `4` starts immediately.

### Client Runtime Knobs

- `HFT_AFXDP_IFACE`
  interface to bind, defaults to `lo`
- `HFT_AFXDP_QUEUE`
  AF_XDP queue id, defaults to `0`
- `HFT_AFXDP_FORCE_ZEROCOPY`
  request zero-copy mode first
- `HFT_AFXDP_PREFER_SKB`
  prefer SKB mode over native XDP

If AF_XDP setup fails on the requested interface, the client already contains retry logic that falls back to loopback copy-mode paths where possible.

### Notes on Privileges

AF_XDP and XDP attach operations commonly require elevated privileges or the equivalent Linux capabilities. The sample launch above uses `sudo` for the client for that reason.

## Design Tradeoffs

This project favors:

- explicit, inspectable dataflow
- fast local replay iteration
- low-overhead ring-based communication
- stage-level latency measurement
- CPU-specific tuning such as `-march=native`, AVX2, and LTO

It does not currently try to solve:

- full exchange matching
- portfolio-level strategy logic
- persistence or recovery
- multi-symbol sharding across processes
- gateway/session management
- production-grade retransmit handling
- kernel-bypass order egress

## Current Strategy and Risk Model

### Imbalance Strategy

The current strategy is deliberately simple:

- aggregate buy and sell quantity by `stock_locate`
- require minimum total quantity `>= 1`
- emit a buy when bid ratio `>= 0.20`
- emit a sell when ask ratio `>= 0.20`
- send a fixed order size of `1`

### Risk Controls

Inline risk checks currently enforce:

- max position per stock: `10000`
- max order quantity: `500`
- max orders per second: `100`
- burst tokens: `50`

Notional caps are present but effectively unbounded in the current configuration.

## Repository Layout

- `CMakeLists.txt`
  build, flags, library detection, and CPU-core pinning config
- `apps/server/`
  UDP ITCH replay and TCP order receiver
- `apps/client/`
  AF_XDP ingress, parser thread, and TCP order sender
- `apps/engine/`
  engine dispatcher entry point
- `src/engine/`
  SIMD compute path, strategy, risk, and OMS
- `src/parser/`
  ITCH packet-to-event conversion
- `src/itch/`
  ITCH message structs and handler dispatch
- `src/afxdp/`
  AF_XDP socket setup and XDP BPF filter
- `src/pools/`
  event and message object pools
- `include/hft/`
  all project headers, namespaced under `hft/`
- `include/hft/ring_buffers/`
  parser, event, and order ring definitions
- `scripts/`
  build and run helpers
- `data/`
  replay input
- `results/`
  generated latency logs

## Limitations

- Linux-only in practice because of AF_XDP and XDP dependencies
- the current client captures a single UDP feed path
- the strategy is intentionally minimal and not alpha-focused
- GPU and FPGA entry files exist but are not wired into the active build
- timestamp accuracy is useful for comparison, not a substitute for production telemetry
- order receiver sampling logs every 100th order rather than every order

## Future Work

- expose batch-mode presets through CMake options
- add richer retransmit and sequence-gap handling
- widen the strategy beyond a single imbalance heuristic
- add deterministic replay validation and correctness tests
- benchmark with isolated CPUs and broader perf counter capture
- wire up the GPU and FPGA experiment paths or remove the placeholders
