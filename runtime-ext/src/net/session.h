/* SPDX-License-Identifier: MPL-2.0
 * session.h — Player registry and session handshake.
 *
 * The session layer sits above the raw transport (net.h) and handles:
 *  - Sending MSG_HELLO / MSG_PLAYER_LIST to new clients
 *  - Tracking which player IDs are live
 *  - Accepting or rejecting MSG_AREA_ENTER (gate for area transitions)
 *  - Forwarding received MSG_ENEMY_DAMAGE to the game state
 */
#pragma once
#include <cstdint>
#include "packets.h"

namespace session {

static constexpr uint8_t MAX_PLAYERS = 5;  /* host + 4 clients */

struct Player {
    uint8_t  id;
    bool     active;
    float    x, y, z;    /* last known position (host-side cache) */
    uint16_t yaw;
    uint16_t stage_id;
    uint8_t  room_id;
    uint8_t  life;        /* current quarter-hearts (0 = unknown) */
    uint8_t  max_life;    /* max quarter-hearts (0 = unknown) */
    char     name[16];    /* display name, null-terminated */
};

struct ChatEntry {
    uint8_t player_id;
    char    name[16];     /* sender display name */
    char    text[64];     /* message text */
};

/* ── Lifecycle ── */
void init();     /* call after net::host_start or net::client_connect */
void shutdown();

/* ── Per-frame update ── */
void tick();     /* call after net::poll() each logic step             */

/* ── Accessors ── */
const Player* get_player(uint8_t id);    /* nullptr if not active */
uint8_t       player_count();

/* ── Chat ring buffer (up to 8 entries, most-recent first) ── */
int        chat_count();
ChatEntry  chat_get(int idx);            /* 0 = most recent */
void       add_local_chat(const char* sender_name, const char* text);

/* ── Guest-side state cache ── */
/* Called by net_bindings.cpp when a MSG_PLAYER_STATE packet arrives. */
void on_player_packet(const PlayerPacket* pkt);

/* Called by net_bindings.cpp when a MSG_ENEMY_DAMAGE packet arrives. */
void on_enemy_damage_packet(const EnemyDamagePacket* pkt);

/* Called by net_bindings.cpp when a MSG_WORLD_FLAG packet arrives. */
void on_world_flag_packet(const WorldFlagPacket* pkt);

/* Called by net_mod.cpp when a MSG_PLAYER_INFO / MSG_CHAT arrives. */
void on_player_info_packet(uint8_t peer_id, const PlayerInfoPacket* pkt);
void on_chat_packet        (uint8_t peer_id, const ChatPacket*       pkt);

} // namespace session
