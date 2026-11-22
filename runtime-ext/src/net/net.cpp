/* SPDX-License-Identifier: MPL-2.0
 * net.cpp — ENet transport implementation.
 *
 * ENet version: 1.3.17 (via CMake FetchContent in the base project's CMakeLists).
 *
 * Two connection modes:
 *   Direct:  host_start / client_connect — classic P2P, host opens a port.
 *   Relay:   relay_create / relay_join  — both sides connect to a public relay
 *            server, no port forwarding required. The relay uses a single ENet
 *            channel; we derive the game channel from the message type tag.
 *
 * Peer slots: index 0–7 in s_peers[]. Slot index is stored in ENetPeer::data
 * as a uintptr_t so we can recover it on DISCONNECT without searching.
 * player_id for the host is always NET_HOST_ID (0). Clients are assigned
 * IDs 1–N by the host (direct) or the relay (relay mode).
 */
#include "net.h"
#include "packets.h"
#include "p2p.h"
#include <enet/enet.h>
#include <cstring>
#include <cstdio>
#include <cassert>

namespace net {

static constexpr int MAX_PEERS = 8;

/* NOTE: "s_host" is a macro in winsock.h; use s_enet_host to avoid conflict. */
static ENetHost*  s_enet_host   = nullptr;
static bool       s_is_host     = false;
static bool       s_is_relay    = false;
static bool       s_is_p2p      = false;
static bool       s_connected   = false;
static uint8_t    s_local_id    = NET_HOST_ID;
static uint8_t    s_peer_count  = 0;
static char       s_lobby_code[RELAY_CODE_LEN + 1] = {};
static char       s_signaling_url[512] = {};  /* stored to delete room on shutdown */

static ENetPeer*  s_peers[MAX_PEERS]    = {};
static uint8_t    s_peer_ids[MAX_PEERS] = {};  /* player_id per slot */
static bool       s_peer_authed[MAX_PEERS] = {}; /* peer passed MSG_AUTH_REQ */

/* Session secret: 32 raw bytes decoded from the 64-char hex received from
 * the signaling server.  Zero-filled → auth disabled (direct/relay sessions). */
static uint8_t s_session_secret[32]  = {};
static bool    s_has_secret          = false;
/* Auth timeout: disconnect unauthenticated peers after this many poll() calls. */
static uint32_t s_peer_auth_timer[MAX_PEERS] = {};
static constexpr uint32_t AUTH_TIMEOUT_POLLS = 300; /* ~5 seconds at 60Hz */

static OnPacket     s_on_packet;
static OnConnect    s_on_connect;
static OnDisconnect s_on_disconnect;

/* Map a game message type to its logical channel for the recv-queue router. */
static Channel channel_from_msg(uint8_t msg) {
    if (msg == MSG_PLAYER_STATE)                    return Channel::Player;
    if (msg == MSG_ENEMY_STATE)                     return Channel::Enemy;
    if (msg == MSG_ENEMY_DAMAGE || msg == MSG_WORLD_FLAG) return Channel::State;
    return Channel::System;
}

/* ── Internal helpers ── */

static int find_free_slot() {
    for (int i = 0; i < MAX_PEERS; i++)
        if (!s_peers[i]) return i;
    return -1;
}

static int slot_for_peer(ENetPeer* peer) {
    return (int)(uintptr_t)peer->data;
}

/* ── Public API ── */

bool host_start(uint16_t port, uint8_t max_peers) {
    if (s_enet_host) return false;
    if (enet_initialize() != 0) return false;

    ENetAddress addr;
    addr.host = ENET_HOST_ANY;
    addr.port = port;

    s_enet_host = enet_host_create(&addr,
                              (max_peers < MAX_PEERS ? max_peers : MAX_PEERS),
                              (size_t)Channel::Count,
                              /*inBandwidth=*/0, /*outBandwidth=*/0);
    if (!s_enet_host) {
        enet_deinitialize();
        return false;
    }

    s_is_host    = true;
    s_connected  = true;
    s_local_id   = NET_HOST_ID;
    s_peer_count = 0;
    return true;
}

bool client_connect(const char* host_addr, uint16_t port) {
    if (s_enet_host) return false;
    if (enet_initialize() != 0) return false;

    s_enet_host = enet_host_create(/*address=*/nullptr,
                              /*maxPeers=*/1,
                              (size_t)Channel::Count,
                              0, 0);
    if (!s_enet_host) {
        enet_deinitialize();
        return false;
    }

    ENetAddress addr;
    if (enet_address_set_host(&addr, host_addr) < 0) {
        enet_host_destroy(s_enet_host);
        s_enet_host = nullptr;
        enet_deinitialize();
        return false;
    }
    addr.port = port;

    ENetPeer* peer = enet_host_connect(s_enet_host, &addr,
                                       (size_t)Channel::Count, /*data=*/0);
    if (!peer) {
        enet_host_destroy(s_enet_host);
        s_enet_host = nullptr;
        enet_deinitialize();
        return false;
    }

    /* Slot 0 is reserved for the host peer on the client side. */
    s_peers[0]    = peer;
    s_peer_ids[0] = NET_HOST_ID;
    peer->data    = (void*)(uintptr_t)0;

    s_is_host   = false;
    s_is_relay  = false;
    s_connected = false;  /* set true on ENET_EVENT_TYPE_CONNECT */
    s_local_id  = 0xFF;   /* assigned by host in MSG_HELLO        */
    return true;
}

/* ── Relay helpers ── */

static bool relay_connect_common(const char* relay_addr, uint16_t relay_port) {
    if (s_enet_host) return false;
    if (enet_initialize() != 0) return false;

    /* Relay uses a single ENet channel; we route by message type tag. */
    s_enet_host = enet_host_create(nullptr, /*maxPeers=*/1, /*channels=*/1, 0, 0);
    if (!s_enet_host) { enet_deinitialize(); return false; }

    ENetAddress addr;
    if (enet_address_set_host(&addr, relay_addr) < 0) {
        enet_host_destroy(s_enet_host); s_enet_host = nullptr;
        enet_deinitialize(); return false;
    }
    addr.port = relay_port;

    ENetPeer* peer = enet_host_connect(s_enet_host, &addr, /*channels=*/1, 0);
    if (!peer) {
        enet_host_destroy(s_enet_host); s_enet_host = nullptr;
        enet_deinitialize(); return false;
    }

    s_peers[0]    = peer;
    s_peer_ids[0] = 0;
    peer->data    = (void*)(uintptr_t)0;
    s_is_relay    = true;
    s_connected   = false;
    return true;
}

bool relay_create(const char* relay_addr, uint16_t relay_port) {
    if (!relay_connect_common(relay_addr, relay_port)) return false;

    /* Flush the CONNECT event and send RELAY_CREATE immediately. */
    ENetEvent ev;
    if (enet_host_service(s_enet_host, &ev, /*timeout_ms=*/2000) > 0 &&
        ev.type == ENET_EVENT_TYPE_CONNECT)
    {
        uint8_t msg = RELAY_CREATE;
        ENetPacket* pkt = enet_packet_create(&msg, 1, ENET_PACKET_FLAG_RELIABLE);
        enet_peer_send(s_peers[0], 0, pkt);
        enet_host_flush(s_enet_host);
    } else {
        /* Relay unreachable. */
        shutdown();
        return false;
    }

    s_is_host  = true;
    s_local_id = NET_HOST_ID;
    return true;
}

/* ── P2P helpers ─────────────────────────────────────────────────────────── */

bool p2p_create(const char* signaling_url, uint16_t local_port) {
    if (s_enet_host) return false;

    P2PCreateResult r = ::p2p_create(signaling_url, local_port);
    if (!r.ok) return false;

    /* p2p_create already discovered our public address and registered the room.
     * Now start ENet on the same local port so NAT maps them together. */
    if (!host_start(r.local_port, /*max_peers=*/4)) {
        p2p_delete_room(signaling_url, r.code);
        return false;
    }

    s_is_p2p  = true;
    s_is_relay = false;
    memcpy(s_lobby_code, r.code, RELAY_CODE_LEN);
    s_lobby_code[RELAY_CODE_LEN] = '\0';
    strncpy(s_signaling_url, signaling_url, sizeof s_signaling_url - 1);

    /* Store the session secret so we can verify MSG_AUTH_REQ from joiners. */
    set_session_secret(r.secret);

    fprintf(stderr, "[net] P2P host ready, code=%s port=%u\n",
            s_lobby_code, r.local_port);
    return true;
}

bool p2p_join(const char* signaling_url, const char* code6, uint16_t local_port) {
    if (s_enet_host) return false;

    P2PJoinResult r = ::p2p_join(signaling_url, code6, local_port);
    if (!r.ok) return false;

    /* Punch NAT holes before ENet tries to connect. */
    p2p_punch(r.host_ip, r.host_port, r.local_port, /*count=*/6);

    /* Connect ENet on the same local port. */
    if (!client_connect(r.host_ip, r.host_port)) return false;

    s_is_p2p   = true;
    s_is_relay = false;

    /* Store the session secret so we send MSG_AUTH_REQ on connect. */
    set_session_secret(r.secret);
    return true;
}

bool relay_join(const char* relay_addr, uint16_t relay_port, const char* code6) {
    if (!relay_connect_common(relay_addr, relay_port)) return false;

    ENetEvent ev;
    if (enet_host_service(s_enet_host, &ev, /*timeout_ms=*/2000) > 0 &&
        ev.type == ENET_EVENT_TYPE_CONNECT)
    {
        uint8_t buf[1 + RELAY_CODE_LEN];
        buf[0] = RELAY_JOIN;
        memcpy(buf + 1, code6, RELAY_CODE_LEN);
        ENetPacket* pkt = enet_packet_create(buf, sizeof buf,
                                             ENET_PACKET_FLAG_RELIABLE);
        enet_peer_send(s_peers[0], 0, pkt);
        enet_host_flush(s_enet_host);
    } else {
        shutdown();
        return false;
    }

    s_is_host  = false;
    s_local_id = 0xFF;  /* assigned in RELAY_PLAYER_JOINED */
    return true;
}

void shutdown() {
    if (!s_enet_host) return;

    /* Clean up P2P room from signaling server if we were the host. */
    if (s_is_p2p && s_is_host && s_lobby_code[0])
        p2p_delete_room(s_signaling_url, s_lobby_code);

    for (int i = 0; i < MAX_PEERS; i++) {
        if (s_peers[i]) {
            enet_peer_disconnect_now(s_peers[i], 0);
            s_peers[i]    = nullptr;
            s_peer_ids[i] = 0;
        }
    }

    enet_host_destroy(s_enet_host);
    s_enet_host = nullptr;
    enet_deinitialize();

    s_connected        = false;
    s_peer_count       = 0;
    s_local_id         = NET_HOST_ID;
    s_is_p2p           = false;
    s_signaling_url[0] = '\0';
}

void set_on_packet    (OnPacket cb)     { s_on_packet     = cb; }
void set_on_connect   (OnConnect cb)    { s_on_connect    = cb; }
void set_on_disconnect(OnDisconnect cb) { s_on_disconnect = cb; }

/* Decode 64-char lowercase hex string into 32 raw bytes. */
static bool hex_decode(const char* hex64, uint8_t out[32]) {
    for (int i = 0; i < 32; i++) {
        auto h = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hi = h(hex64[i*2]), lo = h(hex64[i*2+1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

void set_session_secret(const char* hex64) {
    if (!hex64 || !hex64[0] || strlen(hex64) != 64) {
        memset(s_session_secret, 0, sizeof s_session_secret);
        s_has_secret = false;
        return;
    }
    s_has_secret = hex_decode(hex64, s_session_secret);
    if (!s_has_secret)
        fprintf(stderr, "[net] WARNING: set_session_secret: invalid hex\n");
}

void poll() {
    if (!s_enet_host) return;

    ENetEvent ev;
    while (enet_host_service(s_enet_host, &ev, /*timeout_ms=*/0) > 0) {
        switch (ev.type) {

        case ENET_EVENT_TYPE_CONNECT: {
            int slot = find_free_slot();
            if (slot < 0) {
                /* No room — reject. */
                enet_peer_disconnect(ev.peer, 0);
                break;
            }
            ev.peer->data = (void*)(uintptr_t)slot;
            s_peers[slot] = ev.peer;

            if (s_is_host) {
                /* Assign next available player ID (session.cpp fills MSG_HELLO). */
                uint8_t new_id = ++s_peer_count;
                s_peer_ids[slot] = new_id;
                /* Mark peer unauthenticated; start auth timeout. */
                s_peer_authed[slot]    = !s_has_secret; /* no secret → auto-auth */
                s_peer_auth_timer[slot] = 0;
                if (!s_has_secret && s_on_connect) s_on_connect(new_id);
            } else {
                /* Client side: host is always slot 0, ID assignment via MSG_HELLO.
                 * If we have a secret, send MSG_AUTH_REQ immediately. */
                s_peer_authed[slot] = true; /* client marks itself authed */
                if (s_has_secret) {
                    AuthReqPacket auth = {};
                    auth.type = MSG_AUTH_REQ;
                    memcpy(auth.secret, s_session_secret, 32);
                    ENetPacket* pkt = enet_packet_create(&auth, sizeof auth,
                                                         ENET_PACKET_FLAG_RELIABLE);
                    enet_peer_send(ev.peer, (uint8_t)Channel::System, pkt);
                    enet_host_flush(s_enet_host);
                }
                s_connected = true;
                if (s_on_connect) s_on_connect(NET_HOST_ID);
            }
            break;
        }

        case ENET_EVENT_TYPE_RECEIVE: {
            const uint8_t* data = ev.packet->data;
            size_t         len  = ev.packet->dataLength;

            if (s_is_relay && len >= 1) {
                /* Relay control packets come in on the same channel as game data. */
                uint8_t rtype = data[0];

                if (rtype == RELAY_CREATED && len >= 2 + RELAY_CODE_LEN) {
                    /* [RELAY_CREATED, player_id=0, code[6]] */
                    s_local_id = data[1];
                    memcpy(s_lobby_code, data + 2, RELAY_CODE_LEN);
                    s_lobby_code[RELAY_CODE_LEN] = '\0';
                    s_connected = true;
                    fprintf(stderr, "[net] relay: room created, code=%s\n",
                            s_lobby_code);
                    if (s_on_connect) s_on_connect(NET_HOST_ID);

                } else if (rtype == RELAY_PLAYER_JOINED && len >= 2) {
                    uint8_t new_id    = data[1];
                    uint8_t new_total = (len >= 3) ? data[2] : 0;
                    if (new_id == s_local_id) {
                        /* This is us — RELAY_JOIN was accepted. */
                        /* code is in data[3..8] for the joiner only. */
                        if (len >= 3 + RELAY_CODE_LEN)
                            memcpy(s_lobby_code, data + 3, RELAY_CODE_LEN);
                        s_lobby_code[RELAY_CODE_LEN] = '\0';
                        s_connected = true;
                        s_local_id  = new_id;
                        s_peer_count = new_total - 1; /* exclude self */
                        fprintf(stderr,
                                "[net] relay: joined room, id=%u players=%u\n",
                                new_id, new_total);
                    } else {
                        ++s_peer_count;
                        fprintf(stderr,
                                "[net] relay: player %u joined (%u total)\n",
                                new_id, new_total);
                    }
                    if (s_on_connect) s_on_connect(new_id);

                } else if (rtype == RELAY_PLAYER_LEFT && len >= 2) {
                    uint8_t left_id = data[1];
                    if (s_peer_count > 0) --s_peer_count;
                    fprintf(stderr, "[net] relay: player %u left\n", left_id);
                    if (s_on_disconnect) s_on_disconnect(left_id);

                } else if (rtype == RELAY_ERROR && len >= 2) {
                    fprintf(stderr, "[net] relay error 0x%02x\n", data[1]);

                } else if (rtype == RELAY_GAME_DATA && len >= 3 && s_on_packet) {
                    /* [RELAY_GAME_DATA, sender_id, ...game_payload...] */
                    uint8_t sender_id  = data[1];
                    const uint8_t* payload = data + 2;
                    size_t plen        = len - 2;
                    Channel ch         = channel_from_msg(payload[0]);
                    s_on_packet(sender_id, ch, payload, plen);
                }

            } else if (!s_is_relay && len > 0) {
                int     slot    = slot_for_peer(ev.peer);
                uint8_t peer_id = s_peer_ids[slot];
                uint8_t msg_type = data[0];

                /* Validate packet size before any processing. */
                if (!packet_size_valid(msg_type, (uint32_t)len)) {
                    fprintf(stderr,
                            "[net] dropped oversized/malformed pkt type=0x%02x len=%zu\n",
                            msg_type, len);
                    enet_packet_destroy(ev.packet);
                    continue;
                }

                /* Handle auth handshake before forwarding game packets. */
                if (s_is_host && msg_type == MSG_AUTH_REQ && s_has_secret
                    && len == sizeof(AuthReqPacket))
                {
                    bool match = (memcmp(data + 1, s_session_secret, 32) == 0);
                    uint8_t resp = match ? MSG_AUTH_OK : MSG_AUTH_FAIL;
                    ENetPacket* rp = enet_packet_create(&resp, 1,
                                                        ENET_PACKET_FLAG_RELIABLE);
                    enet_peer_send(ev.peer, (uint8_t)Channel::System, rp);
                    if (match) {
                        s_peer_authed[slot] = true;
                        fprintf(stderr, "[net] peer %u authenticated\n", peer_id);
                        if (s_on_connect) s_on_connect(peer_id);
                    } else {
                        fprintf(stderr, "[net] peer %u auth FAILED — disconnecting\n",
                                peer_id);
                        enet_host_flush(s_enet_host);
                        enet_peer_disconnect(ev.peer, 0);
                    }
                    enet_packet_destroy(ev.packet);
                    continue;
                }

                /* Drop game packets from unauthenticated peers. */
                if (s_is_host && s_has_secret && !s_peer_authed[slot]) {
                    enet_packet_destroy(ev.packet);
                    continue;
                }

                if (s_on_packet)
                    s_on_packet(peer_id, static_cast<Channel>(ev.channelID),
                                data, len);
            }
            enet_packet_destroy(ev.packet);
            break;
        }

        case ENET_EVENT_TYPE_DISCONNECT: {
            int     slot    = slot_for_peer(ev.peer);
            uint8_t peer_id = s_peer_ids[slot];

            s_peers[slot]       = nullptr;
            s_peer_ids[slot]    = 0;
            s_peer_authed[slot] = false;
            s_peer_auth_timer[slot] = 0;
            ev.peer->data       = nullptr;

            if (s_peer_count > 0) --s_peer_count;
            if (!s_is_host) s_connected = false;

            if (s_on_disconnect) s_on_disconnect(peer_id);
            break;
        }

        default:
            break;
        }
    }

    /* Auth timeout: disconnect peers that haven't sent MSG_AUTH_REQ in time. */
    if (s_is_host && s_has_secret) {
        for (int i = 0; i < MAX_PEERS; i++) {
            if (s_peers[i] && !s_peer_authed[i]) {
                s_peer_auth_timer[i]++;
                if (s_peer_auth_timer[i] >= AUTH_TIMEOUT_POLLS) {
                    fprintf(stderr,
                            "[net] peer %u auth timeout — disconnecting\n",
                            s_peer_ids[i]);
                    enet_peer_disconnect(s_peers[i], 0);
                }
            }
        }
    }
}

void send_unreliable(uint8_t peer_id, Channel ch, const void* data, size_t len) {
    if (!s_enet_host || !data || !len) return;

    if (s_is_relay) {
        /* In relay mode all traffic goes to the relay peer; it fans out. */
        if (!s_peers[0]) return;
        ENetPacket* pkt = enet_packet_create(data, len,
                                             ENET_PACKET_FLAG_UNSEQUENCED);
        if (pkt) enet_peer_send(s_peers[0], 0, pkt);
        return;
    }

    ENetPacket* pkt = enet_packet_create(data, len, ENET_PACKET_FLAG_UNSEQUENCED);
    if (!pkt) return;

    if (peer_id == NET_BROADCAST) {
        enet_host_broadcast(s_enet_host, (enet_uint8)ch, pkt);
    } else {
        for (int i = 0; i < MAX_PEERS; i++) {
            if (s_peers[i] && s_peer_ids[i] == peer_id) {
                enet_peer_send(s_peers[i], (enet_uint8)ch, pkt);
                return;
            }
        }
        enet_packet_destroy(pkt);
    }
}

void send_reliable(uint8_t peer_id, Channel ch, const void* data, size_t len) {
    if (!s_enet_host || !data || !len) return;

    if (s_is_relay) {
        if (!s_peers[0]) return;
        ENetPacket* pkt = enet_packet_create(data, len,
                                             ENET_PACKET_FLAG_RELIABLE);
        if (pkt) enet_peer_send(s_peers[0], 0, pkt);
        return;
    }

    ENetPacket* pkt = enet_packet_create(data, len, ENET_PACKET_FLAG_RELIABLE);
    if (!pkt) return;

    if (peer_id == NET_BROADCAST) {
        enet_host_broadcast(s_enet_host, (enet_uint8)ch, pkt);
    } else {
        for (int i = 0; i < MAX_PEERS; i++) {
            if (s_peers[i] && s_peer_ids[i] == peer_id) {
                enet_peer_send(s_peers[i], (enet_uint8)ch, pkt);
                return;
            }
        }
        enet_packet_destroy(pkt);
    }
}

bool        is_host()          { return s_is_host;    }
bool        is_relay()         { return s_is_relay;   }
bool        is_p2p()           { return s_is_p2p;     }
bool        is_connected()     { return s_connected;  }
uint8_t     local_player_id()  { return s_local_id;   }
uint8_t     peer_count()       { return s_peer_count; }
const char* lobby_code()       { return s_lobby_code; }

/* Called by session.cpp after MSG_HELLO is processed on the client. */
void set_local_player_id(uint8_t id) { s_local_id = id; }

} // namespace net
