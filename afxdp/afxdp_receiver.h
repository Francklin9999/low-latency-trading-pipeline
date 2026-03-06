#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct afxdp_ctx;

struct afxdp_pkt {
    uint8_t  *data;
    uint32_t  len;
    uint64_t  addr;
};

int afxdp_init(struct afxdp_ctx **out,
               const char *ifname,
               uint32_t queue_id,
               int force_zerocopy);

int afxdp_recv_burst(struct afxdp_ctx *ctx,
                     struct afxdp_pkt *pkts,
                     int max_pkts);

int afxdp_recycle(struct afxdp_ctx *ctx,
                  const uint64_t *addrs,
                  int n);

void afxdp_close(struct afxdp_ctx *ctx);

#ifdef __cplusplus
}
#endif
