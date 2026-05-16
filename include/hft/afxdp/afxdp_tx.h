#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct afxdp_tx_ctx;

struct afxdp_tx_config {
    const char *ifname;        // interface for the XSK TX bind
    uint32_t    queue_id;      // NIC queue for the bind
    uint32_t    src_ip_be;     // network byte order
    uint32_t    dst_ip_be;
    uint16_t    src_port;      // host order
    uint16_t    dst_port;
    uint8_t     src_mac[6];
    uint8_t     dst_mac[6];
    int         force_zerocopy;
    int         use_hugepages; // try MAP_HUGETLB first, fall back
};

int  afxdp_tx_init(struct afxdp_tx_ctx **out,
                   const struct afxdp_tx_config *cfg);

// Send `payload_len` bytes (typically a wire_order) through the AF_XDP TX path.
// Returns 0 on success, -1 with errno set on failure (caller should retry).
int  afxdp_tx_send(struct afxdp_tx_ctx *ctx,
                   const void *payload, uint32_t payload_len);

// Drain the XSK completion ring, returning consumed frames to the free list.
// Cheap; safe to call from the TX hot path between sends.
int  afxdp_tx_drain_completions(struct afxdp_tx_ctx *ctx);

void afxdp_tx_close(struct afxdp_tx_ctx *ctx);

#ifdef __cplusplus
}
#endif
