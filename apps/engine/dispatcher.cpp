#define _POSIX_C_SOURCE 200809L
#include <array>
#include <cerrno>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#endif

#include "hft/engine/itch_book_adapter.hpp"
#include "hft/engine/sim/fill_sim.hpp"
#include "hft/engine/oms/oms.hpp"
#include "hft/engine/risk/risk.hpp"
#include "hft/engine/strategy/mm_mean_reversion.hpp"
#include "hft/engine/strategy/signal.hpp"
#include "hft/engine/universe.hpp"
#include "hft/telemetry/telemetry.hpp"

extern "C" {
    #include "hft/engine/events.h"
    #include "hft/itch/itch_handler.h"
    #include "hft/itch/packet.h"
    #include "hft/utils/utils.h"
}
#include "hft/ring_buffers/order/order_to_exc.h"
#include "hft/ring_buffers/parser/parser_to_engine.h"

#ifndef HFT_ENGINE_DISPATCH_CORE
#define HFT_ENGINE_DISPATCH_CORE 6
#endif

static constexpr std::int32_t kPoolPerBook = 1 << 18;
static std::array<hft::ItchBookAdapter*, hft::universe::kSize> g_books{};
static std::array<hft::sim::FillSim, hft::universe::kSize> g_sims{};
static hft::strategy::MarketMaker g_mm{};

static order_to_exc g_order_to_exc;
order_to_exc* ORDER_TO_EXC = &g_order_to_exc;

static inline oms::Side oms_side_from(Side s) noexcept
{
    return (s == Side::Buy) ? oms::Side::BUY : oms::Side::SELL;
}

static bool engine_submit_quote(std::uint16_t locate, Side side, PriceTicks px,
                                Quantity qty, std::uint64_t parse_ns) noexcept
{
    if (qty == 0) {
        return oms::submit(locate, oms_side_from(side),
                           static_cast<std::uint32_t>(px), 0,
                           parse_ns) != 0;
    }
    Signal sig{
        .stock_locate = locate,
        .price        = static_cast<std::uint32_t>(px),
        .qty          = static_cast<std::uint32_t>(qty),
        .side         = oms_side_from(side),
        .parse_ns     = parse_ns,
    };
    if (!risk::check_and_fill(sig)) return false;
    return oms::submit(locate, sig.side, sig.price, sig.qty, parse_ns) != 0;
}

#ifdef __linux__
static void pin_self_to_core(int core)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
        std::fprintf(stderr, "[engine] pin to core %d failed\n", core);
    }
}
#endif

static inline void apply_event(const event& e) noexcept
{
    const std::uint16_t shard = hft::universe::shard_for_locate(e.stock_locate);
    if (shard == hft::universe::kNotInUniverse) return;
    auto& adapter = *g_books[shard];
    auto& fsim    = g_sims[shard];

    hft::ItchBookAdapter::OrderInfo trade_info{};
    const bool have_trade_info =
        (e.type == 'E' || e.type == 'C') && adapter.peek(e.order_id, trade_info);

    switch (e.type) {
        case 'A':
        case 'F':
            adapter.onAdd(e.order_id, static_cast<char>(e.side), e.qty, e.price);
            break;
        case 'X':
            adapter.onCancelPartial(e.order_id, e.qty);
            break;
        case 'D':
            adapter.onDelete(e.order_id);
            break;
        case 'E':
            adapter.onExecute(e.order_id, e.qty);
            break;
        case 'C':
            adapter.onExecuteWithPrice(e.order_id, e.qty, e.price, 'Y');
            break;
        case 'U':
            adapter.onReplace(e.order_id, e.aux, e.qty, e.price);
            break;
        default:
            return;
    }

    if (have_trade_info) {
        const PriceTicks tp = (e.type == 'C')
            ? static_cast<PriceTicks>(e.price / 100u)
            : trade_info.px;
        if (auto fill = fsim.onTrade(trade_info.side, tp,
                                     static_cast<Quantity>(e.qty), e.ts)) {
            g_mm.on_fill(shard, *fill);
        }
    }

    g_mm.on_book_event(shard, adapter, fsim, e.ts);
}

static std::atomic<std::uint64_t> g_heartbeat{0};
static bool g_deadman_enabled = false;

static void consume()
{
    std::atomic_ref<std::uint32_t> nr{PARSER_ENGINE.next_read};
    std::atomic_ref<std::uint32_t> nw{PARSER_ENGINE.next_write};
    std::uint32_t idle_loops = 0;
    for (;;) {
        const std::uint32_t r = nr.load(std::memory_order_relaxed);
        const std::uint32_t w = nw.load(std::memory_order_acquire);
        if (r == w) {
            if (g_deadman_enabled && ((++idle_loops & 0x3FFu) == 0u)) {
                g_heartbeat.fetch_add(1, std::memory_order_relaxed);
            }
            cpu_relax();
            continue;
        }
        idle_loops = 0;

        packet_ref* ref = PARSER_ENGINE.data[r & PARSER_ENGINE_MASK];
        std::uint32_t offset = 0;
        while (offset + 2u <= ref->len) {
            const std::uint16_t msg_size =
                be16toh(*(const std::uint16_t*)(ref->data + offset));
            const std::uint32_t next_offset = offset + 2u + (std::uint32_t)msg_size;
            if (next_offset > ref->len) break;

            const std::uint8_t msg_type = ref->data[offset + 2u];
            itch_handler_fn fn = dispatch_table[msg_type];
            if (fn) {
                event ev;
                if (fn((uint8_t*)(ref->data + offset + 2u), &ev) == 0) {
                    ev.ts = ref->ts;
                    apply_event(ev);
                }
            }
            offset = next_offset;
        }

        nr.store(r + 1, std::memory_order_release);
        if (g_deadman_enabled) {
            g_heartbeat.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

static void start_deadman_if_enabled()
{
    const char* env = std::getenv("HFT_DEADMAN_MS");
    if (!env || env[0] == '\0') return;
    char* end = nullptr;
    const unsigned long ms = std::strtoul(env, &end, 10);
    if (*end != '\0' || ms == 0) return;
    g_deadman_enabled = true;

    std::thread([ms]{
        using namespace std::chrono;
        const auto period = milliseconds(ms);
        std::uint64_t prev = g_heartbeat.load(std::memory_order_relaxed);
        for (;;) {
            std::this_thread::sleep_for(period);
            const std::uint64_t cur = g_heartbeat.load(std::memory_order_relaxed);
            if (cur == prev) {
                std::fprintf(stderr,
                    "[engine] DEAD-MAN: no heartbeat for %lu ms -- global kill\n",
                    ms);
                hft::strategy::MarketMaker::global_kill_switch().store(
                    true, std::memory_order_release);
                return;
            }
            prev = cur;
        }
    }).detach();
    std::fprintf(stderr, "[engine] dead-man enabled (%lu ms)\n", ms);
}

extern "C" {
    void *udp_receiver_thread(void *arg);
    void *order_sender_thread(void *arg);
}

#ifndef HFT_CLIENT_UDP_RECV_CORE
#define HFT_CLIENT_UDP_RECV_CORE 4
#endif
#ifndef HFT_CLIENT_ORDER_SEND_CORE
#define HFT_CLIENT_ORDER_SEND_CORE 5
#endif

#ifdef __linux__
static int pin_attr_to_core(pthread_attr_t* attr, int core)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<size_t>(core), &set);
    int rc = pthread_attr_setaffinity_np(attr, sizeof(set), &set);
    if (rc != 0) {
        std::fprintf(stderr, "pthread_attr_setaffinity_np(core=%d) failed\n", core);
        return -1;
    }
    return 0;
}
#endif

int main()
{
#ifdef __linux__
    // Pin all pages (current + future) so no major fault hits the hot path.
    rlimit rlim{RLIM_INFINITY, RLIM_INFINITY};
    setrlimit(RLIMIT_MEMLOCK, &rlim);
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        std::fprintf(stderr, "[trading] mlockall failed (continuing): %s\n",
                     std::strerror(errno));
    } else {
        std::printf("[trading] mlockall(CURRENT|FUTURE) ok -- pages pinned\n");
    }
#endif

    std::atomic_ref<std::uint32_t>(ORDER_TO_EXC->next_write).store(0, std::memory_order_relaxed);
    std::atomic_ref<std::uint32_t>(ORDER_TO_EXC->next_read).store(0, std::memory_order_relaxed);

    for (std::size_t i = 0; i < hft::universe::kSize; ++i) {
        g_books[i] = new hft::ItchBookAdapter(kPoolPerBook);
    }

    // Env-tunable MM knobs (HFT_MM_*); see Params defaults in mm_mean_reversion.hpp.
    auto env_d = [](const char* k, double dflt) {
        const char* v = std::getenv(k);
        return (v && *v) ? std::strtod(v, nullptr) : dflt;
    };
    auto env_i = [](const char* k, long dflt) {
        const char* v = std::getenv(k);
        return (v && *v) ? std::strtol(v, nullptr, 10) : dflt;
    };
    hft::strategy::Params p{};
    p.half_spread_ticks       = (PriceTicks)env_i("HFT_MM_HALF_SPREAD_TICKS", p.half_spread_ticks);
    p.requote_threshold_ticks = (PriceTicks)env_i("HFT_MM_REQUOTE_TICKS",     p.requote_threshold_ticks);
    p.quote_qty               = (Quantity)  env_i("HFT_MM_QUOTE_QTY",         p.quote_qty);
    p.ema_alpha               =             env_d("HFT_MM_EMA_ALPHA",         p.ema_alpha);
    p.per_symbol_msgs_per_sec = (int32_t)   env_i("HFT_MM_PER_SYMBOL_RPS",    p.per_symbol_msgs_per_sec);
    p.aggregate_msgs_per_sec  = (int32_t)   env_i("HFT_MM_AGG_RPS",           p.aggregate_msgs_per_sec);
    p.burst_msgs              = (int32_t)   env_i("HFT_MM_BURST",             p.burst_msgs);
    p.inventory_aversion      =             env_d("HFT_MM_INV_AVERSION",      p.inventory_aversion);
    p.max_inventory           = (int32_t)   env_i("HFT_MM_MAX_INVENTORY",     p.max_inventory);
    g_mm.set_params(p);
    std::printf("[mm] half_spread=%u requote_thr=%u qty=%u ema=%.3f "
                "rps_per_sym=%d rps_agg=%d burst=%d inv_aversion=%.4f max_inv=%d\n",
                (unsigned)p.half_spread_ticks, (unsigned)p.requote_threshold_ticks,
                (unsigned)p.quote_qty, p.ema_alpha,
                p.per_symbol_msgs_per_sec, p.aggregate_msgs_per_sec, p.burst_msgs,
                p.inventory_aversion, p.max_inventory);

    g_mm.set_submit(&engine_submit_quote);

    // Risk rate-limit knobs (env-tunable; defaults in risk.hpp).
    {
        const char* rps_env   = std::getenv("HFT_RISK_RPS");
        const char* burst_env = std::getenv("HFT_RISK_BURST");
        const uint32_t rps   = (rps_env   && *rps_env)   ? (uint32_t)std::strtoul(rps_env,   nullptr, 10) : 0;
        const uint32_t burst = (burst_env && *burst_env) ? (uint32_t)std::strtoul(burst_env, nullptr, 10) : 0;
        if (rps || burst) {
            risk::set_limits(rps, burst);
            std::printf("[risk] rps=%u burst=%u (env override)\n", rps, burst);
        }
    }
    hft::telemetry::start(&g_mm);
    start_deadman_if_enabled();

#ifdef __linux__
    pthread_attr_t recv_attr, send_attr;
    pthread_attr_init(&recv_attr);
    pthread_attr_init(&send_attr);
    if (pin_attr_to_core(&recv_attr, HFT_CLIENT_UDP_RECV_CORE) != 0) return 1;
    if (pin_attr_to_core(&send_attr, HFT_CLIENT_ORDER_SEND_CORE) != 0) return 1;

    pthread_t recv_tid, send_tid;
    if (pthread_create(&recv_tid, &recv_attr, udp_receiver_thread, nullptr) != 0) {
        std::perror("pthread_create udp_receiver"); return 1;
    }
    if (pthread_create(&send_tid, &send_attr, order_sender_thread, nullptr) != 0) {
        std::perror("pthread_create order_sender"); return 1;
    }
    pthread_attr_destroy(&recv_attr);
    pthread_attr_destroy(&send_attr);

    pin_self_to_core(HFT_ENGINE_DISPATCH_CORE);
    std::printf("[trading] dispatcher core=%d udp=%d order=%d  universe N=%zu\n",
                HFT_ENGINE_DISPATCH_CORE,
                HFT_CLIENT_UDP_RECV_CORE,
                HFT_CLIENT_ORDER_SEND_CORE,
                hft::universe::kSize);
#endif

    consume();
    return 0;
}
