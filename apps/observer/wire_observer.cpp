#include <array>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>
#include <endian.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "hft/protocol/wire.h"
#include "hft/server/server.h"
#include "hft/utils/moldudp64.h"

namespace {

constexpr std::size_t kFrameMax = 65536;
constexpr std::uint64_t kRetryEveryNs = 1'000'000'000ULL;
constexpr std::uint64_t kHealthEveryNs = 1'000'000'000ULL;

struct StreamBuffer {
    std::array<std::uint8_t, 8192> bytes{};
    std::size_t used = 0;
};

struct Counters {
    std::uint64_t feed_packets = 0;
    std::uint64_t feed_messages = 0;
    std::uint64_t orders = 0;
    std::uint64_t acks = 0;
    std::uint64_t ack_ok = 0;
    std::uint64_t ack_bad_csum = 0;
    std::uint64_t ack_seq_gap = 0;
    std::uint64_t transport_drops = 0;
    std::uint64_t last_ack_seq = 0;
};

std::uint64_t mono_ns() noexcept
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

const char* telemetry_socket_path() noexcept
{
    const char* env = std::getenv("HFT_TELEMETRY_SOCKET");
    return (env && env[0] != '\0') ? env : "run/hft-telemetry.sock";
}

bool send_all(int fd, const char* buf, std::size_t len) noexcept
{
    std::size_t sent = 0;
    while (sent < len) {
        const ssize_t rc = ::send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (rc <= 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<std::size_t>(rc);
    }
    return true;
}

int connect_unix_socket(const char* path) noexcept
{
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool send_json(int& fd, const char* buf, Counters& counters) noexcept
{
    if (fd < 0) {
        counters.transport_drops++;
        return false;
    }
    if (!send_all(fd, buf, std::strlen(buf))) {
        ::close(fd);
        fd = -1;
        counters.transport_drops++;
        return false;
    }
    return true;
}

void maybe_reconnect(int& fd, std::uint64_t& last_retry_ns) noexcept
{
    const std::uint64_t now = mono_ns();
    if (fd >= 0 || now - last_retry_ns < kRetryEveryNs) return;
    fd = connect_unix_socket(telemetry_socket_path());
    last_retry_ns = now;
}

void clear_stream(StreamBuffer& stream) noexcept
{
    stream.used = 0;
}

void append_bytes(StreamBuffer& stream, const std::uint8_t* data,
                  std::size_t len) noexcept
{
    if (len == 0) return;
    if (stream.used + len > stream.bytes.size()) {
        stream.used = 0;
        if (len > stream.bytes.size()) {
            data += len - stream.bytes.size();
            len = stream.bytes.size();
        }
    }
    std::memcpy(stream.bytes.data() + stream.used, data, len);
    stream.used += len;
}

void emit_feed_event(int& fd, Counters& counters, std::uint64_t seq,
                     std::uint16_t msg_count, std::uint32_t payload_bytes) noexcept
{
    char buf[256];
    std::snprintf(
        buf, sizeof(buf),
        "{\"type\":\"feed\",\"ts\":%" PRIu64 ",\"seq\":%" PRIu64
        ",\"msgCount\":%u,\"payloadBytes\":%u}\n",
        mono_ns(), seq, static_cast<unsigned>(msg_count),
        static_cast<unsigned>(payload_bytes));
    (void)send_json(fd, buf, counters);
}

void emit_order_event(int& fd, Counters& counters, const wire_order& order) noexcept
{
    char buf[320];
    std::snprintf(
        buf, sizeof(buf),
        "{\"type\":\"order\",\"ts\":%" PRIu64 ",\"seq\":%" PRIu64
        ",\"symbolId\":%u,\"side\":\"%c\",\"qty\":%u,\"price\":%u,"
        "\"msgType\":\"%c\",\"clientSendTs\":%" PRIu64 "}\n",
        mono_ns(), order.seq, static_cast<unsigned>(order.symbol_id), order.side,
        order.qty, order.price, order.msg_type, order.ts);
    (void)send_json(fd, buf, counters);
}

void emit_ack_event(int& fd, Counters& counters, const wire_ack& ack) noexcept
{
    char buf[256];
    std::snprintf(
        buf, sizeof(buf),
        "{\"type\":\"ack\",\"ts\":%" PRIu64 ",\"seq\":%" PRIu64
        ",\"flag\":%u,\"recvTs\":%" PRIu64 "}\n",
        mono_ns(), ack.seq, static_cast<unsigned>(ack.flag), ack.recv_ts);
    (void)send_json(fd, buf, counters);
}

void emit_health_event(int& fd, Counters& counters) noexcept
{
    char buf[384];
    std::snprintf(
        buf, sizeof(buf),
        "{\"type\":\"health\",\"ts\":%" PRIu64 ",\"feedPackets\":%" PRIu64
        ",\"feedMessages\":%" PRIu64 ",\"orders\":%" PRIu64
        ",\"acks\":%" PRIu64 ",\"ackOk\":%" PRIu64
        ",\"ackBadCsum\":%" PRIu64 ",\"ackSeqGap\":%" PRIu64
        ",\"lastAckSeq\":%" PRIu64 ",\"transportDrops\":%" PRIu64 "}\n",
        mono_ns(), counters.feed_packets, counters.feed_messages,
        counters.orders, counters.acks, counters.ack_ok,
        counters.ack_bad_csum, counters.ack_seq_gap,
        counters.last_ack_seq, counters.transport_drops);
    (void)send_json(fd, buf, counters);
}

void drain_orders(StreamBuffer& stream, int& fd, Counters& counters) noexcept
{
    std::size_t off = 0;
    while (stream.used - off >= sizeof(wire_order)) {
        wire_order order;
        std::memcpy(&order, stream.bytes.data() + off, sizeof(order));
        if (order.msg_type != WIRE_MSG_NEW
            && order.msg_type != WIRE_MSG_CANCEL
            && order.msg_type != WIRE_MSG_FLUSH) {
            off += 1;
            continue;
        }
        counters.orders++;
        emit_order_event(fd, counters, order);
        off += sizeof(wire_order);
    }
    if (off > 0) {
        std::memmove(stream.bytes.data(), stream.bytes.data() + off, stream.used - off);
        stream.used -= off;
    }
}

void drain_acks(StreamBuffer& stream, int& fd, Counters& counters) noexcept
{
    std::size_t off = 0;
    while (stream.used - off >= sizeof(wire_ack)) {
        wire_ack ack;
        std::memcpy(&ack, stream.bytes.data() + off, sizeof(ack));
        if (ack.msg_type != WIRE_MSG_ACK) {
            off += 1;
            continue;
        }
        counters.acks++;
        counters.last_ack_seq = ack.seq;
        if (ack.flag == WIRE_ACK_OK) counters.ack_ok++;
        else if (ack.flag == WIRE_ACK_BAD_CSUM) counters.ack_bad_csum++;
        else if (ack.flag == WIRE_ACK_SEQ_GAP) counters.ack_seq_gap++;
        emit_ack_event(fd, counters, ack);
        off += sizeof(wire_ack);
    }
    if (off > 0) {
        std::memmove(stream.bytes.data(), stream.bytes.data() + off, stream.used - off);
        stream.used -= off;
    }
}

int open_packet_socket() noexcept
{
    return ::socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_ALL));
}

bool parse_ipv4_packet(const std::uint8_t* frame, std::size_t len,
                       const iphdr*& ip4, const std::uint8_t*& l4,
                       std::size_t& l4_len) noexcept
{
    if (len < sizeof(iphdr)) return false;
    ip4 = reinterpret_cast<const iphdr*>(frame);
    if (ip4->version != 4) return false;
    const std::size_t ihl = static_cast<std::size_t>(ip4->ihl) * 4u;
    if (ihl < sizeof(iphdr) || len < ihl) return false;
    const std::size_t total_len = std::min<std::size_t>(len, ntohs(ip4->tot_len));
    if (total_len < ihl) return false;
    l4 = frame + ihl;
    l4_len = total_len - ihl;
    return true;
}

}  // namespace

int main()
{
    int pkt_fd = open_packet_socket();
    if (pkt_fd < 0) {
        std::perror("[wire_observer] packet socket");
        return 1;
    }

    int telemetry_fd = -1;
    std::uint64_t last_retry_ns = 0;
    std::uint64_t last_health_ns = 0;
    Counters counters{};
    StreamBuffer order_stream{};
    StreamBuffer ack_stream{};

    std::array<std::uint8_t, kFrameMax> frame{};

    std::fprintf(stderr,
        "[wire_observer] watching PACKET_OUTGOING on feed=%d order=%d -> %s\n",
        FEED_PORT, ORDER_PORT, telemetry_socket_path());

    for (;;) {
        maybe_reconnect(telemetry_fd, last_retry_ns);

        sockaddr_ll addr{};
        socklen_t addr_len = sizeof(addr);
        const ssize_t n = ::recvfrom(pkt_fd, frame.data(), frame.size(), 0,
                                     reinterpret_cast<sockaddr*>(&addr), &addr_len);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::perror("[wire_observer] recvfrom");
            break;
        }

        if (addr.sll_pkttype != PACKET_OUTGOING) continue;

        const iphdr* ip4 = nullptr;
        const std::uint8_t* l4 = nullptr;
        std::size_t l4_len = 0;
        if (!parse_ipv4_packet(frame.data(), static_cast<std::size_t>(n), ip4, l4, l4_len)) {
            continue;
        }

        if (ip4->protocol == IPPROTO_UDP) {
            if (l4_len < sizeof(udphdr)) continue;
            const auto* udp = reinterpret_cast<const udphdr*>(l4);
            const std::uint16_t dport = ntohs(udp->dest);
            if (dport != FEED_PORT) continue;
            if (l4_len < sizeof(udphdr) + sizeof(mold_udp64_header)) continue;

            const auto* hdr = reinterpret_cast<const mold_udp64_header*>(l4 + sizeof(udphdr));
            const std::uint16_t msg_count = be16toh(hdr->message_count);
            if (msg_count == 0xFFFFu) continue;
            counters.feed_packets++;
            counters.feed_messages += msg_count;
            emit_feed_event(telemetry_fd, counters,
                            be64toh(hdr->sequence_number),
                            msg_count,
                            static_cast<std::uint32_t>(l4_len - sizeof(udphdr)));
        } else if (ip4->protocol == IPPROTO_TCP) {
            if (l4_len < sizeof(tcphdr)) continue;
            const auto* tcp = reinterpret_cast<const tcphdr*>(l4);
            const std::size_t tcp_hlen = static_cast<std::size_t>(tcp->doff) * 4u;
            if (tcp_hlen < sizeof(tcphdr) || l4_len < tcp_hlen) continue;
            const std::uint8_t* payload = l4 + tcp_hlen;
            const std::size_t payload_len = l4_len - tcp_hlen;
            const std::uint16_t sport = ntohs(tcp->source);
            const std::uint16_t dport = ntohs(tcp->dest);

            if (tcp->syn || tcp->fin || tcp->rst) {
                if (sport == ORDER_PORT || dport == ORDER_PORT) {
                    clear_stream(order_stream);
                    clear_stream(ack_stream);
                }
            }
            if (payload_len == 0) continue;

            if (dport == ORDER_PORT) {
                append_bytes(order_stream, payload, payload_len);
                drain_orders(order_stream, telemetry_fd, counters);
            } else if (sport == ORDER_PORT) {
                append_bytes(ack_stream, payload, payload_len);
                drain_acks(ack_stream, telemetry_fd, counters);
            }
        }

        const std::uint64_t now = mono_ns();
        if (now - last_health_ns >= kHealthEveryNs) {
            emit_health_event(telemetry_fd, counters);
            last_health_ns = now;
        }
    }

    if (telemetry_fd >= 0) ::close(telemetry_fd);
    ::close(pkt_fd);
    return 1;
}
