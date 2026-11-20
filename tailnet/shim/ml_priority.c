/*
 * MicroLink copies its config at microlink_init() and has no setter for the
 * priority peer. This reaches into the struct (microlink_internal.h, from the
 * pinned submodule, so a layout change fails the build) so the shim can pin a
 * peer on an existing handle between microlink_stop() and microlink_start(),
 * keeping the node's keys and session.
 */
#include "microlink_internal.h"

void tailnet_ml_set_priority_peer(microlink_t *ml, uint32_t vpn_ip) {
    if (ml) ml->config.priority_peer_ip = vpn_ip;
}

/* Home DERP region of a tracked peer, 0 if unknown. */
uint16_t tailnet_ml_peer_derp_region(const microlink_t *ml, uint32_t vpn_ip) {
    if (!ml) return 0;
    for (int i = 0; i < ml->peer_count && i < ML_MAX_PEERS; i++) {
        if (ml->peers[i].active && ml->peers[i].vpn_ip == vpn_ip) return ml->peers[i].derp_region;
    }
    return 0;
}
