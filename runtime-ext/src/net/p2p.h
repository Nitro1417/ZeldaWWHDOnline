/* SPDX-License-Identifier: MPL-2.0
 * p2p.h — Peer-to-peer session setup using STUN + Cloudflare Worker signaling.
 *
 * Flow (host):
 *   1. p2p_create() → STUN discovers public IP:port → POST to signaling Worker
 *      → returns 6-char room code
 *   2. ENet host_start() on the same local port
 *   3. Wait for p2p_join_wait() to signal peer arrival, then p2p_punch() + ENet accept
 *
 * Flow (guest):
 *   1. p2p_join() → GET host addr from Worker → returns host public IP:port
 *   2. p2p_punch() to open NAT hole
 *   3. ENet client_connect() to host
 */
#pragma once
#include <cstdint>

/* Default local UDP port shared by ENet and the STUN probe. */
static constexpr uint16_t P2P_DEFAULT_PORT = 7777;

struct P2PCreateResult {
    char     code[7];        /* 6-char room code, NUL-terminated */
    uint16_t local_port;     /* the local port to pass to host_start() */
    char     secret[65];     /* 32-byte session secret as 64 hex chars + NUL */
    bool     ok;
};

struct P2PJoinResult {
    char     host_ip[64];    /* host's public dotted-decimal IPv4 */
    uint16_t host_port;      /* host's public port */
    uint16_t local_port;     /* local port to pass to client_connect() / p2p_punch() */
    char     secret[65];     /* session secret received from signaling server */
    bool     ok;
};

/*
 * Host: discover public address, register in signaling server.
 *   signaling_url  — base URL of the Cloudflare Worker
 *                    e.g. "https://wwhd-signal.YOUR-SUBDOMAIN.workers.dev"
 *   local_port     — local UDP port (default P2P_DEFAULT_PORT).
 *                    Pass 0 to let the OS pick; returned in result.local_port.
 * Returns .ok = false on STUN failure or HTTP error.
 */
P2PCreateResult p2p_create(const char* signaling_url,
                           uint16_t    local_port = P2P_DEFAULT_PORT);

/*
 * Guest: look up host address in signaling server.
 *   signaling_url — same base URL used by the host
 *   code          — 6-char room code from the host
 *   local_port    — local UDP port for hole-punch / ENet (default P2P_DEFAULT_PORT)
 * Returns .ok = false when code not found or network error.
 */
P2PJoinResult p2p_join(const char* signaling_url,
                       const char* code,
                       uint16_t    local_port = P2P_DEFAULT_PORT);

/*
 * Send count raw UDP packets to ip:port on local_port.
 * Both host and guest call this before ENet connect to open NAT holes.
 * local_port must match what was used for STUN / what ENet will bind.
 */
void p2p_punch(const char* ip, uint16_t port,
               uint16_t local_port = P2P_DEFAULT_PORT,
               int count = 6);

/*
 * Remove the room from the signaling server (host calls after session ends).
 * Silent no-op on network error.
 */
void p2p_delete_room(const char* signaling_url, const char* code);
