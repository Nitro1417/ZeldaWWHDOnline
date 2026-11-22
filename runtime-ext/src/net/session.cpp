/* SPDX-License-Identifier: MPL-2.0
 * session.cpp — Player registry and handshake logic.
 */
#include "session.h"
#include "net.h"
#include "packets.h"
#include <cstring>
#include <cstdio>

namespace session {

static Player s_players[MAX_PLAYERS] = {};

/* ── Internal helpers ── */

static void clear_player(uint8_t id) {
    if (id < MAX_PLAYERS) {
        s_players[id] = {};
    }
}

static Player* alloc_player(uint8_t id) {
    if (id >= MAX_PLAYERS) return nullptr;
    s_players[id].id     = id;
    s_players[id].active = true;
    return &s_players[id];
}

/* ── On connect / disconnect callbacks ── */

static void handle_connect(uint8_t peer_id) {
    if (net::is_relay()) {
        /* In relay mode the relay server has already assigned peer_id.
         * No MSG_HELLO needed — just register the player locally. */
        alloc_player(peer_id);
        fprintf(stderr, "[online] relay: player %u in room\n", peer_id);
        return;
    }

    if (net::is_host()) {
        /* Direct mode: host assigns ID and tells the new client. */
        HelloPacket hello;
        hello.type         = MSG_HELLO;
        hello.player_id    = peer_id;
        hello.player_count = (uint8_t)(net::peer_count() + 1); /* +1 for host */
        hello.flags        = 0;
        net::send_reliable(peer_id, net::Channel::System, &hello, sizeof hello);

        alloc_player(peer_id);
        fprintf(stderr, "[online] player %u connected\n", peer_id);
    }
}

static void handle_disconnect(uint8_t peer_id) {
    clear_player(peer_id);
    fprintf(stderr, "[online] player %u disconnected\n", peer_id);
}

/* ── Packet dispatch ── */

static void handle_packet(uint8_t peer_id, net::Channel ch,
                           const void* data, size_t len) {
    if (!data || !len) return;
    const uint8_t type = *(const uint8_t*)data;

    switch (type) {

    case MSG_HELLO: {
        /* Client receives this from host. */
        if (len < sizeof(HelloPacket)) break;
        const HelloPacket* h = (const HelloPacket*)data;
        net::set_local_player_id(h->player_id);
        alloc_player(h->player_id);
        fprintf(stderr, "[online] assigned player id %u\n", h->player_id);
        break;
    }

    case MSG_PLAYER_STATE: {
        if (len < sizeof(PlayerPacket)) break;
        on_player_packet((const PlayerPacket*)data);

        /* Host forwards to all other clients. */
        if (net::is_host()) {
            /* Rewrite player_id to the sender before forwarding. */
            PlayerPacket fwd = *(const PlayerPacket*)data;
            fwd.player_id = peer_id;
            net::send_unreliable(net::NET_BROADCAST,
                                 net::Channel::Player, &fwd, sizeof fwd);
        }
        break;
    }

    case MSG_ENEMY_STATE:
        /* Clients apply incoming enemy state; handled by net_bindings.cpp. */
        break;

    case MSG_ENEMY_DAMAGE:
        if (len < sizeof(EnemyDamagePacket)) break;
        on_enemy_damage_packet((const EnemyDamagePacket*)data);
        break;

    case MSG_WORLD_FLAG:
        if (len < sizeof(WorldFlagPacket)) break;
        on_world_flag_packet((const WorldFlagPacket*)data);
        /* Host broadcasts to everyone including sender. */
        if (net::is_host())
            net::send_reliable(net::NET_BROADCAST,
                               net::Channel::State, data, len);
        break;

    case MSG_AREA_ENTER:
        /* TODO: gate area transitions so all players load together. */
        break;

    default:
        break;
    }
}

/* ── Public API ── */

void init() {
    memset(s_players, 0, sizeof s_players);

    /* Host is always player 0.
     * Relay joiners get their ID when RELAY_PLAYER_JOINED fires for them,
     * which calls handle_connect and alloc_player at that point. */
    if (net::is_host()) {
        s_players[0].id     = 0;
        s_players[0].active = true;
    }

    net::set_on_connect   (handle_connect);
    net::set_on_disconnect(handle_disconnect);
    net::set_on_packet    (handle_packet);
}

void shutdown() {
    memset(s_players, 0, sizeof s_players);
}

void tick() {
    /* Reserved for future heartbeat / timeout logic. */
}

const Player* get_player(uint8_t id) {
    if (id >= MAX_PLAYERS || !s_players[id].active) return nullptr;
    return &s_players[id];
}

uint8_t player_count() {
    uint8_t n = 0;
    for (const auto& p : s_players)
        if (p.active) ++n;
    return n;
}

/* ── Guest packet handlers ── */

void on_player_packet(const PlayerPacket* pkt) {
    if (!pkt || pkt->player_id >= MAX_PLAYERS) return;
    Player* p = &s_players[pkt->player_id];
    if (!p->active) return;
    p->x   = pkt->x * 0.25f;
    p->y   = pkt->y * 0.25f;
    p->z   = pkt->z * 0.25f;
    p->yaw = pkt->yaw;
}

void on_enemy_damage_packet(const EnemyDamagePacket* pkt) {
    if (!pkt) return;
    /*
     * Phase 4: apply validated damage to the host's enemy actor.
     * net_bindings.cpp will call wwhd_actor_combat_damage() once that
     * binding exists in the SDK.
     */
    fprintf(stderr, "[online] enemy 0x%08X hit by player %u for %u damage\n",
            pkt->actor_id, pkt->player_id, pkt->damage);
}

void on_world_flag_packet(const WorldFlagPacket* pkt) {
    if (!pkt) return;
    /*
     * Phase 5: write pkt->value to the stage's flag array at pkt->flag_index.
     * Exact flag offsets come from the decompilation.
     */
    fprintf(stderr, "[online] world flag type=%u index=%u value=%u\n",
            pkt->flag_type, pkt->flag_index, pkt->value);
}

} // namespace session
