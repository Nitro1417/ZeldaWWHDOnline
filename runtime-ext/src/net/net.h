/* SPDX-License-Identifier: MPL-2.0
 * net.h — Host-side ENet transport API.
 *
 * Usage:
 *   Host:   net::host_start(7777);
 *   Client: net::client_connect("192.168.1.5", 7777);
 *
 *   Register callbacks, then call net::poll() once per game logic step
 *   (from the hook in game_hooks.cpp).
 *
 *   peer_id == NET_BROADCAST (0xFF) sends to every connected peer.
 */
#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>

namespace net {

static constexpr uint8_t NET_BROADCAST = 0xFF;
static constexpr uint8_t NET_HOST_ID   = 0;

enum class Channel : uint8_t {
    Player  = 0,
    Enemy   = 1,
    State   = 2,
    System  = 3,
    Count   = 4
};

/* Callback types */
using OnPacket     = std::function<void(uint8_t peer_id, Channel ch,
                                        const void* data, size_t len)>;
using OnConnect    = std::function<void(uint8_t peer_id)>;
using OnDisconnect = std::function<void(uint8_t peer_id)>;

/* ── Direct session setup (players open ports / are on same LAN) ── */
bool host_start   (uint16_t port, uint8_t max_peers = 4);
bool client_connect(const char* host_addr, uint16_t port);

/* ── Relay session setup (no port forwarding required) ──────────────
 *   relay_create: start a room on the relay server.  After RELAY_CREATED
 *                 arrives, lobby_code() returns the 6-char code to share.
 *   relay_join:   join an existing room by its 6-char code.
 *   relay_addr:   hostname or IP of the relay server.
 *   relay_port:   UDP port the relay is listening on (default 7778).
 */
bool relay_create(const char* relay_addr, uint16_t relay_port = 7778);
bool relay_join  (const char* relay_addr, uint16_t relay_port,
                  const char* code6);

/* ── P2P session setup (STUN + Cloudflare Worker signaling) ──────────
 *   p2p_create:    discover public IP via STUN, register room in
 *                  the signaling Worker, start ENet host.
 *                  lobby_code() returns the 6-char code when done.
 *   p2p_join:      look up host addr from signaling Worker,
 *                  punch NAT holes, then connect via ENet.
 *   signaling_url: Cloudflare Worker URL, e.g.
 *                  "https://wwhd-signal.YOUR-SUBDOMAIN.workers.dev"
 *   local_port:    local UDP port (default 7777).
 */
bool p2p_create(const char* signaling_url, uint16_t local_port = 7777);
bool p2p_join  (const char* signaling_url, const char* code6,
                uint16_t local_port = 7777);

/* ── State queries (P2P-specific) ── */
bool is_p2p();   /* true when connected via P2P (not relay, not direct) */

/* ── Security: session secret authentication ──────────────────────────────
 * Call this after p2p_create / p2p_join with the hex-encoded 32-byte secret
 * from P2PCreateResult / P2PJoinResult.  When set:
 *   - Clients send MSG_AUTH_REQ on connect; host disconnects unauthenticated peers.
 *   - Hosts only accept game packets from authenticated peers.
 *   - Direct and relay sessions leave this unset (empty) for no enforcement.
 */
void set_session_secret(const char* hex64 /* 64 hex chars + NUL, or nullptr */);

/* ── Teardown ── */
void shutdown();

/* ── Callbacks (register before calling poll) ── */
void set_on_packet    (OnPacket cb);
void set_on_connect   (OnConnect cb);
void set_on_disconnect(OnDisconnect cb);

/* ── Frame update — call once per logic step ── */
void poll();

/* ── Send ── */
void send_unreliable(uint8_t peer_id, Channel ch, const void* data, size_t len);
void send_reliable  (uint8_t peer_id, Channel ch, const void* data, size_t len);

/* ── State queries ── */
bool        is_host();
bool        is_relay();         /* true when connected through relay server */
bool        is_connected();
uint8_t     local_player_id(); /* NET_HOST_ID for host, 1–N for clients    */
uint8_t     peer_count();      /* number of remote peers                   */
const char* lobby_code();      /* 6-char room code (relay host only)       */
void        set_local_player_id(uint8_t id); /* called by session.cpp after MSG_HELLO */

} // namespace net
