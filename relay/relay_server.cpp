/* SPDX-License-Identifier: MPL-2.0
 * relay_server.cpp — WWHD Online relay server.
 *
 * All game traffic is proxied here, so players need no port forwarding or VPN.
 * Both players connect to this single public UDP endpoint. The relay forwards
 * every game packet between clients in the same room.
 *
 * Usage:  ./wwhd_relay [port]        (default port: 7778)
 *
 * Deploy:
 *   Any Linux VPS with a public IP works. Needs inbound UDP <port> open.
 *   Fly.io free tier is enough for dozens of concurrent sessions:
 *     fly launch --name wwhd-relay --dockerfile relay/Dockerfile
 *
 * Lobby protocol (see packets.h relay section):
 *   Client → relay  RELAY_CREATE             → creates room, gets 6-char code
 *   Client → relay  RELAY_JOIN + 6-char code → joins existing room
 *   Relay  → client RELAY_CREATED + id + code → host welcome
 *   Relay  → all    RELAY_PLAYER_JOINED + id  → someone arrived
 *   Relay  → all    RELAY_PLAYER_LEFT   + id  → someone left
 *   Game packets (any other msg type) are forwarded transparently, prepended
 *   with [RELAY_GAME_DATA, sender_player_id].
 *
 * Room lifetime: 30 minutes of inactivity before the room is closed.
 * Max players per room: 5 (host + 4 clients).
 */

#include <enet/enet.h>
#include <unordered_map>
#include <string>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <cctype>
#include <csignal>

/* ── Constants ── */
static constexpr int     MAX_TOTAL_PEERS = 512;
static constexpr int     MAX_PLAYERS     = 5;
static constexpr int     CODE_LEN        = 6;
static constexpr uint32_t ROOM_TIMEOUT_S = 1800; /* 30 minutes */
static constexpr size_t  MAX_PKT         = 2048;

/* ── Relay message types (must match packets.h) ── */
static constexpr uint8_t RELAY_GAME_DATA     = 0x00;
static constexpr uint8_t RELAY_CREATE        = 0x20;
static constexpr uint8_t RELAY_JOIN          = 0x21;
static constexpr uint8_t RELAY_CREATED       = 0x22;
static constexpr uint8_t RELAY_PLAYER_JOINED = 0x23;
static constexpr uint8_t RELAY_PLAYER_LEFT   = 0x24;
static constexpr uint8_t RELAY_ERROR         = 0x2F;

/* Error codes for RELAY_ERROR */
static constexpr uint8_t ERR_ALREADY_IN_ROOM = 0x01;
static constexpr uint8_t ERR_CODE_NOT_FOUND  = 0x02;
static constexpr uint8_t ERR_ROOM_FULL       = 0x03;

/* ── Data structures ── */

struct RoomPlayer {
    ENetPeer* peer     = nullptr;
    uint8_t   id       = 0xFF;
};

struct Room {
    char       code[CODE_LEN + 1] = {};
    RoomPlayer players[MAX_PLAYERS];
    uint8_t    count              = 0;
    uint32_t   last_activity      = 0;
};

/* peer → room code */
static std::unordered_map<ENetPeer*, std::string> s_peer_room;
/* code → room */
static std::unordered_map<std::string, Room>      s_rooms;

static volatile bool s_running = true;

/* ── Helpers ── */

static void gen_code(char* out) {
    /* Omit 0, 1, I, O to avoid ambiguous characters. */
    static const char CHARS[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    for (int i = 0; i < CODE_LEN; i++)
        out[i] = CHARS[rand() % (int)(sizeof CHARS - 1)];
    out[CODE_LEN] = '\0';
}

static std::string unique_code() {
    char code[CODE_LEN + 1];
    do { gen_code(code); } while (s_rooms.count(code));
    return std::string(code);
}

static void send_reliable(ENetPeer* peer, const void* data, size_t len) {
    ENetPacket* pkt = enet_packet_create(data, len, ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, 0, pkt);
}

static void send_error(ENetPeer* peer, uint8_t code) {
    uint8_t buf[2] = {RELAY_ERROR, code};
    send_reliable(peer, buf, sizeof buf);
}

static RoomPlayer* find_player_in_room(Room& room, ENetPeer* peer) {
    for (int i = 0; i < room.count; i++)
        if (room.players[i].peer == peer) return &room.players[i];
    return nullptr;
}

/* ── Relay a game packet from sender to all other room members ── */
static void relay_game_packet(Room& room, uint8_t sender_id,
                               const void* data, size_t len, bool reliable) {
    if (len < 1 || len + 2 > MAX_PKT) return;

    /* Prepend [RELAY_GAME_DATA, sender_id] so the receiver knows who sent it. */
    uint8_t buf[MAX_PKT];
    buf[0] = RELAY_GAME_DATA;
    buf[1] = sender_id;
    memcpy(buf + 2, data, len);
    size_t total = len + 2;

    uint32_t flags = reliable ? ENET_PACKET_FLAG_RELIABLE
                              : ENET_PACKET_FLAG_UNSEQUENCED;
    for (int i = 0; i < room.count; i++) {
        if (room.players[i].id == sender_id) continue;
        ENetPacket* pkt = enet_packet_create(buf, total, flags);
        enet_peer_send(room.players[i].peer, 0, pkt);
    }

    room.last_activity = (uint32_t)time(nullptr);
}

/* ── Event handlers ── */

static void on_create(ENetPeer* peer) {
    if (s_peer_room.count(peer)) {
        send_error(peer, ERR_ALREADY_IN_ROOM);
        return;
    }

    std::string code = unique_code();
    Room& room       = s_rooms[code];
    strncpy(room.code, code.c_str(), CODE_LEN + 1);
    room.players[0] = {peer, 0};   /* host = player 0 */
    room.count      = 1;
    room.last_activity = (uint32_t)time(nullptr);
    s_peer_room[peer]  = code;

    /* RELAY_CREATED: [type, player_id=0, code[6]] */
    uint8_t resp[2 + CODE_LEN];
    resp[0] = RELAY_CREATED;
    resp[1] = 0;
    memcpy(resp + 2, code.c_str(), CODE_LEN);
    send_reliable(peer, resp, sizeof resp);

    printf("[relay] room %-6s created\n", room.code);
}

static void on_join(ENetPeer* peer, const uint8_t* code_bytes) {
    if (s_peer_room.count(peer)) {
        send_error(peer, ERR_ALREADY_IN_ROOM);
        return;
    }

    char code[CODE_LEN + 1];
    for (int i = 0; i < CODE_LEN; i++)
        code[i] = (char)toupper((unsigned char)code_bytes[i]);
    code[CODE_LEN] = '\0';

    auto it = s_rooms.find(code);
    if (it == s_rooms.end()) {
        send_error(peer, ERR_CODE_NOT_FOUND);
        return;
    }

    Room& room = it->second;
    if (room.count >= MAX_PLAYERS) {
        send_error(peer, ERR_ROOM_FULL);
        return;
    }

    uint8_t new_id = room.count;
    room.players[room.count++] = {peer, new_id};
    room.last_activity = (uint32_t)time(nullptr);
    s_peer_room[peer]  = code;

    /* Tell the new player their ID: [RELAY_PLAYER_JOINED, new_id, total_count, code[6]] */
    uint8_t welcome[3 + CODE_LEN];
    welcome[0] = RELAY_PLAYER_JOINED;
    welcome[1] = new_id;
    welcome[2] = room.count;
    memcpy(welcome + 3, code, CODE_LEN);
    send_reliable(peer, welcome, sizeof welcome);

    /* Notify all existing players: [RELAY_PLAYER_JOINED, new_id, total_count] */
    uint8_t notify[3] = {RELAY_PLAYER_JOINED, new_id, room.count};
    for (int i = 0; i < room.count - 1; i++)
        send_reliable(room.players[i].peer, notify, sizeof notify);

    printf("[relay] room %-6s player %u joined (%u/%u)\n",
           code, new_id, room.count, MAX_PLAYERS);
}

static void on_disconnect(ENetPeer* peer) {
    auto pit = s_peer_room.find(peer);
    if (pit == s_peer_room.end()) return;

    std::string code = pit->second;
    s_peer_room.erase(pit);

    auto rit = s_rooms.find(code);
    if (rit == s_rooms.end()) return;

    Room& room   = rit->second;
    uint8_t left = 0xFF;

    /* Find and remove, compacting the array. */
    for (int i = 0; i < room.count; i++) {
        if (room.players[i].peer == peer) {
            left = room.players[i].id;
            for (int j = i; j < room.count - 1; j++)
                room.players[j] = room.players[j + 1];
            room.players[--room.count] = {};
            break;
        }
    }

    if (left != 0xFF) {
        uint8_t notify[2] = {RELAY_PLAYER_LEFT, left};
        for (int i = 0; i < room.count; i++)
            send_reliable(room.players[i].peer, notify, sizeof notify);
    }

    printf("[relay] room %-6s player %u left (%u remain)\n",
           code.c_str(), left, room.count);

    if (room.count == 0) {
        s_rooms.erase(rit);
        printf("[relay] room %-6s closed\n", code.c_str());
    }
}

static void cleanup_stale_rooms() {
    uint32_t now = (uint32_t)time(nullptr);
    for (auto it = s_rooms.begin(); it != s_rooms.end(); ) {
        if (now - it->second.last_activity > ROOM_TIMEOUT_S) {
            printf("[relay] room %-6s timed out\n", it->second.code);
            for (int i = 0; i < it->second.count; i++)
                s_peer_room.erase(it->second.players[i].peer);
            it = s_rooms.erase(it);
        } else {
            ++it;
        }
    }
}

/* ── Signal handler ── */
static void on_signal(int) { s_running = false; }

/* ── Entry point ── */
int main(int argc, char** argv) {
    uint16_t port = 7778;
    if (argc > 1) port = (uint16_t)atoi(argv[1]);

    srand((unsigned)time(nullptr));
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    if (enet_initialize() != 0) {
        fprintf(stderr, "[relay] ENet init failed\n");
        return 1;
    }

    ENetAddress addr;
    addr.host = ENET_HOST_ANY;
    addr.port = port;

    ENetHost* server = enet_host_create(&addr, MAX_TOTAL_PEERS,
                                        /*channels=*/1, 0, 0);
    if (!server) {
        fprintf(stderr, "[relay] could not bind UDP port %u\n", port);
        enet_deinitialize();
        return 1;
    }

    printf("[relay] WWHD Online relay server listening on UDP port %u\n", port);

    static uint32_t last_cleanup = 0;

    while (s_running) {
        ENetEvent ev;
        while (enet_host_service(server, &ev, /*timeout_ms=*/100) > 0) {
            switch (ev.type) {

            case ENET_EVENT_TYPE_CONNECT:
                /* Nothing to do — wait for RELAY_CREATE or RELAY_JOIN. */
                break;

            case ENET_EVENT_TYPE_RECEIVE: {
                const uint8_t* data = ev.packet->data;
                size_t len = ev.packet->dataLength;

                if (len >= 1) {
                    uint8_t msg = data[0];

                    if (msg == RELAY_CREATE) {
                        on_create(ev.peer);
                    } else if (msg == RELAY_JOIN && len >= 1 + CODE_LEN) {
                        on_join(ev.peer, data + 1);
                    } else {
                        /* Game packet — forward to room. */
                        auto pit = s_peer_room.find(ev.peer);
                        if (pit != s_peer_room.end()) {
                            auto rit = s_rooms.find(pit->second);
                            if (rit != s_rooms.end()) {
                                RoomPlayer* rp = find_player_in_room(
                                    rit->second, ev.peer);
                                if (rp) {
                                    bool rel = (ev.packet->flags &
                                                ENET_PACKET_FLAG_RELIABLE) != 0;
                                    relay_game_packet(rit->second, rp->id,
                                                      data, len, rel);
                                }
                            }
                        }
                    }
                }
                enet_packet_destroy(ev.packet);
                break;
            }

            case ENET_EVENT_TYPE_DISCONNECT:
                on_disconnect(ev.peer);
                break;

            default:
                break;
            }
        }

        /* Stale room cleanup once per minute. */
        uint32_t now = (uint32_t)time(nullptr);
        if (now - last_cleanup >= 60) {
            last_cleanup = now;
            cleanup_stale_rooms();
        }
    }

    printf("[relay] shutting down\n");
    enet_host_destroy(server);
    enet_deinitialize();
    return 0;
}
