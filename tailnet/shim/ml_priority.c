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
