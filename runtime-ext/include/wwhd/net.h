/* SPDX-License-Identifier: MPL-2.0
 * wwhd/net.h — Guest-side networking API (included by mod.c on the PowerPC side).
 *
 * These functions are declared here and implemented in net_bindings.cpp as
 * host trampolines that the runtime resolves when the mod .elf is loaded.
 * They follow the same pattern as wwhd_log / wwhd_actor_get etc.
 *
 * Place this file in the base project's guest SDK include path alongside
 * wwhd_guest.h, then add net_bindings.cpp to the wwhd CMake target.
 *
 * Usage in mod.c:
 *   #include "wwhd/net.h"
 *   ...
 *   if (wwhd_net_is_connected()) {
 *       PlayerPacket pkt = { ... };
 *       wwhd_net_send_unreliable(NET_CH_PLAYER, &pkt, sizeof pkt);
 *   }
 */
#pragma once

/* Re-export the shared packet definitions so mod.c only needs one include. */
#include "../../../src/net/packets.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Session ── */

/**
 * Returns 1 if connected to at least one peer (client: host accepted;
 * host: at least one client joined), 0 otherwise.
 */
int     wwhd_net_is_connected(void);

/** Returns 1 if this instance is the session host, 0 if client. */
int     wwhd_net_is_host(void);

/** Returns 1 if connected through the relay server (no port forwarding). */
int     wwhd_net_is_relay(void);

/** Returns 1 if connected via P2P (STUN + hole punching, no relay server). */
int     wwhd_net_is_p2p(void);

/** Returns the local player ID: 0 for host, 1–N for clients. */
uint8_t wwhd_net_player_id(void);

/** Returns the number of remote peers. */
uint8_t wwhd_net_peer_count(void);

/**
 * Returns a pointer to the 6-character NUL-terminated lobby code for this
 * relay or P2P session (e.g. "AB3XYZ"). Valid after relay_create or
 * p2p_create succeeds. Returns an empty string if not host or not connected.
 * The pointed-to memory is static — do NOT modify or free it.
 */
const char* wwhd_net_lobby_code(void);

/* ── Send ── */

/**
 * Send data on the given channel with no delivery guarantee.
 * Use for high-frequency position/animation state (20 Hz).
 *   channel: NET_CH_PLAYER, NET_CH_ENEMY, etc.
 *   peer_id: 0xFF = broadcast to all, otherwise specific player ID.
 */
void wwhd_net_send_unreliable(uint8_t channel, uint8_t peer_id,
                               const void* data, uint32_t len);

/**
 * Send data with guaranteed, ordered delivery.
 * Use for HP changes, deaths, world flags, session events.
 */
void wwhd_net_send_reliable(uint8_t channel, uint8_t peer_id,
                             const void* data, uint32_t len);

/* ── Receive ── */

/**
 * Dequeue the next pending packet for this channel into buf (up to maxlen bytes).
 * Returns the number of bytes written, or 0 if the queue is empty.
 *
 * The packet dispatch happens on the host side in net::poll(), which is called
 * from game_hooks.cpp every logic step. By the time the guest hook runs,
 * all packets for this step are already queued.
 */
uint32_t wwhd_net_recv(uint8_t channel, void* buf, uint32_t maxlen);

#ifdef __cplusplus
}
#endif
