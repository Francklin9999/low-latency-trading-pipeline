#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/udp.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#ifndef FEED_PORT
#define FEED_PORT 5000
#endif

struct {
    __uint(type, BPF_MAP_TYPE_XSKMAP);
    __uint(key_size, sizeof(__u32));
    __uint(value_size, sizeof(__u32));
    __uint(max_entries, 64);
} xsks_map SEC(".maps");

static __always_inline int is_udp_feed(struct xdp_md *ctx)
{
    void *data     = (void *)(long)ctx->data;
    void *data_end = (void *)(long)ctx->data_end;

    struct ethhdr *eth = data;
    if ((void *)(eth + 1) > data_end)
        return 0;

    __u16 proto = bpf_ntohs(eth->h_proto);
    void *next = (void *)(eth + 1);

    // Skip up to two stacked VLAN tags (QinQ) with a bounded loop.
#pragma clang loop unroll(full)
    for (int i = 0; i < 2; i++) {
        if (proto != ETH_P_8021Q && proto != ETH_P_8021AD)
            break;
        if (next + 4 > data_end)
            return 0;
        proto = bpf_ntohs(*(__u16 *)(next + 2));
        next += 4;
    }
    if (proto == ETH_P_8021Q || proto == ETH_P_8021AD)
        return 0;

    if (proto == ETH_P_IP) {
        struct iphdr *ip = next;
        if ((void *)(ip + 1) > data_end)
            return 0;
        if (ip->protocol != IPPROTO_UDP)
            return 0;

        __u32 ihl = (__u32)ip->ihl * 4u;
        if (ihl < sizeof(*ip))
            return 0;
        if (next + ihl > data_end)
            return 0;

        struct udphdr *udp = next + ihl;
        if ((void *)(udp + 1) > data_end)
            return 0;

        return bpf_ntohs(udp->dest) == FEED_PORT;
    }

    if (proto == ETH_P_IPV6) {
        struct ipv6hdr *ip6 = next;
        if ((void *)(ip6 + 1) > data_end)
            return 0;
        if (ip6->nexthdr != IPPROTO_UDP)
            return 0;

        struct udphdr *udp = (void *)(ip6 + 1);
        if ((void *)(udp + 1) > data_end)
            return 0;

        return bpf_ntohs(udp->dest) == FEED_PORT;
    }

    return 0;
}

SEC("xdp")
int xdp_feed_filter(struct xdp_md *ctx)
{
    if (is_udp_feed(ctx))
        return bpf_redirect_map(&xsks_map, ctx->rx_queue_index, XDP_PASS);

    return XDP_PASS; // TCP and everything else -> normal kernel stack
}

char _license[] SEC("license") = "GPL";
