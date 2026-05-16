#include "hft/telemetry/telemetry.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

#include "hft/engine/strategy/mm_mean_reversion.hpp"
#include "hft/engine/universe.hpp"

extern "C" void order_sender_lat_snapshot_ns(std::uint32_t* p50,
                                             std::uint32_t* p90,
                                             std::uint32_t* p99,
                                             std::uint32_t* mx,
                                             std::uint32_t* n_out);

namespace hft::telemetry {
namespace {

const char* telemetry_socket_path() noexcept
{
    const char* env = std::getenv("HFT_TELEMETRY_SOCKET");
    return (env && env[0] != '\0') ? env : "run/hft-telemetry.sock";
}

int tele_connect() noexcept
{
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, telemetry_socket_path(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool tele_send_line(int& fd, const char* buf, std::size_t len) noexcept
{
    if (fd < 0) return false;
    std::size_t sent = 0;
    while (sent < len) {
        const ssize_t rc = ::send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (rc <= 0) { ::close(fd); fd = -1; return false; }
        sent += static_cast<std::size_t>(rc);
    }
    return true;
}

std::atomic<bool>       g_running{false};
std::thread             g_control_thread;
std::thread             g_stats_thread;
strategy::MarketMaker*  g_mm = nullptr;

const char* control_socket_path() noexcept
{
    const char* env = std::getenv("HFT_CONTROL_SOCKET");
    return (env && env[0] != '\0') ? env : "run/hft-control.sock";
}

void apply_command_line(std::string_view line) noexcept
{
    if (!g_mm) return;

    if (line == "kill_global") {
        g_mm->kill_global();
        return;
    }
    if (line == "arm_all") {
        g_mm->arm_all();
        return;
    }

    const auto space = line.find(' ');
    if (space == std::string_view::npos) return;

    const std::string_view op = line.substr(0, space);
    const std::string_view arg = line.substr(space + 1);

    char* end = nullptr;
    const unsigned long locate_ul = std::strtoul(arg.data(), &end, 10);
    if (end == arg.data() || *end != '\0' || locate_ul > 65535UL) return;

    const std::uint16_t locate = static_cast<std::uint16_t>(locate_ul);
    const std::uint16_t shard = universe::shard_for_locate(locate);
    if (shard == universe::kNotInUniverse) return;

    if (op == "kill_symbol") {
        g_mm->kill(shard);
    } else if (op == "arm_symbol") {
        g_mm->arm(shard);
    }
}

void control_loop() noexcept
{
    const char* path = control_socket_path();
    ::unlink(path);

    int server_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::perror("[control] socket");
        return;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (::bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::perror("[control] bind");
        ::close(server_fd);
        return;
    }
    if (::listen(server_fd, 8) != 0) {
        std::perror("[control] listen");
        ::close(server_fd);
        return;
    }

    while (g_running.load(std::memory_order_relaxed)) {
        int client_fd = ::accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            continue;
        }

        char buf[256];
        std::size_t used = 0;
        for (;;) {
            const ssize_t rc = ::read(client_fd, buf + used, sizeof(buf) - used - 1);
            if (rc <= 0) break;
            used += static_cast<std::size_t>(rc);
            buf[used] = '\0';

            char* cursor = buf;
            while (char* nl = std::strchr(cursor, '\n')) {
                *nl = '\0';
                apply_command_line(cursor);
                cursor = nl + 1;
            }

            used = std::strlen(cursor);
            std::memmove(buf, cursor, used);
        }
        ::close(client_fd);
    }

    ::close(server_fd);
    ::unlink(path);
}

void stats_loop() noexcept
{
    using namespace std::chrono;
    if (!g_mm) return;

    const char* dump_env = std::getenv("HFT_PRINT_MM_STATS");
    const bool dump = (dump_env && dump_env[0] == '1');

    constexpr auto kInterval = seconds(2);
    auto next_tick = steady_clock::now() + kInterval;

    int tele_fd = -1;
    auto retry_at = steady_clock::now();

    char buf[8192];

    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_until(next_tick);
        next_tick += kInterval;
        if (!g_running.load(std::memory_order_relaxed)) break;

        // Lazy (re)connect to the control telemetry socket; failure is non-fatal.
        if (tele_fd < 0 && steady_clock::now() >= retry_at) {
            tele_fd = tele_connect();
            if (tele_fd < 0) retry_at = steady_clock::now() + seconds(2);
        }

        const auto now_ns = duration_cast<nanoseconds>(
            system_clock::now().time_since_epoch()).count();

        // mm_stats: per-symbol PnL, inventory, quote counters
        int n = std::snprintf(buf, sizeof(buf),
            "{\"type\":\"mm_stats\",\"ts\":%lld,\"symbols\":[",
            (long long)now_ns);
        for (std::size_t shard = 0; shard < universe::kSize; ++shard) {
            const auto& s = g_mm->state(shard);
            const std::int64_t unreal = g_mm->unrealized_pnl_ticks(shard);
            n += std::snprintf(buf + n, sizeof(buf) - n,
                "%s{\"symbol\":\"%s\",\"locate\":%u,"
                "\"quotes\":%lu,\"fills\":%lu,\"inventory\":%d,"
                "\"realized_pnl_ticks\":%ld,\"unrealized_pnl_ticks\":%ld,"
                "\"quoted_both\":%lu,\"skip_no_tob\":%lu,\"skip_crossed\":%lu,"
                "\"skip_bad_px\":%lu,\"skip_hard_band\":%lu,\"skip_panic\":%lu,"
                "\"rate_limited\":%lu}",
                shard == 0 ? "" : ",",
                universe::kNames[shard],
                (unsigned)universe::kLocates[shard],
                (unsigned long)s.total_quotes,
                (unsigned long)s.total_fills,
                s.inventory,
                (long)s.realized_pnl_ticks,
                (long)unreal,
                (unsigned long)s.quoted_both,
                (unsigned long)s.skip_no_tob,
                (unsigned long)s.skip_crossed,
                (unsigned long)s.skip_bad_px,
                (unsigned long)s.skip_hard_band,
                (unsigned long)s.skip_panic,
                (unsigned long)s.rate_limited);
        }
        n += std::snprintf(buf + n, sizeof(buf) - n, "]}\n");
        if (tele_fd >= 0) (void)tele_send_line(tele_fd, buf, (std::size_t)n);

        // lat_stats: parse_to_send latency from the sender ring
        std::uint32_t p50 = 0, p90 = 0, p99 = 0, mx = 0, ln = 0;
        order_sender_lat_snapshot_ns(&p50, &p90, &p99, &mx, &ln);
        const int ln_n = std::snprintf(buf, sizeof(buf),
            "{\"type\":\"lat_stats\",\"ts\":%lld,"
            "\"p50_ns\":%u,\"p90_ns\":%u,\"p99_ns\":%u,\"max_ns\":%u,"
            "\"n\":%u,\"label\":\"parse_to_send\"}\n",
            (long long)now_ns, p50, p90, p99, mx, ln);
        if (tele_fd >= 0) (void)tele_send_line(tele_fd, buf, (std::size_t)ln_n);

        // terminal dump (opt-in)
        if (dump) {
            std::fprintf(stderr, "[mm] --- universe snapshot ---\n");
            for (std::size_t shard = 0; shard < universe::kSize; ++shard) {
                const auto& s = g_mm->state(shard);
                const std::int64_t unreal = g_mm->unrealized_pnl_ticks(shard);
                std::fprintf(stderr,
                    "[mm %5s] quotes=%lu fills=%lu inv=%+d "
                    "rPnL=%+ld uPnL=%+ld bid=%u ask=%u | "
                    "qBoth=%lu noTOB=%lu crossed=%lu badPx=%lu "
                    "hardBand=%lu panic=%lu rate_lim=%lu\n",
                    universe::kNames[shard],
                    (unsigned long)s.total_quotes,
                    (unsigned long)s.total_fills, s.inventory,
                    (long)s.realized_pnl_ticks, (long)unreal,
                    (unsigned)s.resting_bid, (unsigned)s.resting_ask,
                    (unsigned long)s.quoted_both,
                    (unsigned long)s.skip_no_tob, (unsigned long)s.skip_crossed,
                    (unsigned long)s.skip_bad_px, (unsigned long)s.skip_hard_band,
                    (unsigned long)s.skip_panic, (unsigned long)s.rate_limited);
            }
        }
    }
    if (tele_fd >= 0) ::close(tele_fd);
}

}  // namespace

#ifdef __linux__
namespace {
// Park telemetry/control threads off the hot-path cores; drop to SCHED_OTHER
// nice+10 since they wake every ~2 s.
void detach_to_housekeeping_core() noexcept
{
    const char* env = std::getenv("HFT_HOUSEKEEPING_CORE");
    const int core = (env && *env) ? std::atoi(env) : 0;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<size_t>(core), &set);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);

    sched_param sp{};
    sp.sched_priority = 0;
    (void)pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
    nice(10);
}
}  // namespace
#endif

void start(strategy::MarketMaker* mm) noexcept
{
    g_mm = mm;
    bool expected = false;
    if (!g_running.compare_exchange_strong(expected, true,
                                           std::memory_order_acq_rel)) {
        return;
    }
    g_control_thread = std::thread([]{
#ifdef __linux__
        detach_to_housekeeping_core();
#endif
        control_loop();
    });
    g_stats_thread = std::thread([]{
#ifdef __linux__
        detach_to_housekeeping_core();
#endif
        stats_loop();
    });
}

void stop() noexcept
{
    if (!g_running.exchange(false, std::memory_order_acq_rel)) return;
    if (g_control_thread.joinable()) g_control_thread.join();
    if (g_stats_thread.joinable())   g_stats_thread.join();
}

}  // namespace hft::telemetry
