/* SPDX-License-Identifier: MPL-2.0
 * net_bindings.cpp — Bridge between guest mod calls (wwhd_net_*) and the
 * native net:: transport layer.
 *
 * These are plain extern "C" functions; the mod .elf's linker resolves them
 * the same way it resolves wwhd_log, wwhd_actor_get, etc.
 *
 * Per-channel recv queues are simple ring buffers. The host side (net::poll)
 * fills them; the guest side (mod.c's hooks) drains them. Both run on the
 * same thread (the game's main thread), so no locking is needed.
 *
 * Add this file to the wwhd CMake target alongside net.cpp / session.cpp.
 */
#include "net.h"
#include "session.h"
#include "packets.h"
#include <cstring>
#include <cstdint>
#include <cstddef>

/* ── Per-channel recv ring buffer ── */

static constexpr size_t QUEUE_BYTES = 4096;

struct RecvQueue {
    uint8_t buf[QUEUE_BYTES];
    size_t  read_pos  = 0;
    size_t  write_pos = 0;

    bool push(const void* data, size_t len) {
        /* Each entry is prefixed with a 2-byte length field. */
        size_t total = len + 2;
        if (total > QUEUE_BYTES - ((write_pos - read_pos + QUEUE_BYTES) % QUEUE_BYTES))
            return false;  /* drop on overflow */
        uint16_t u16len = (uint16_t)len;
        write_byte((uint8_t)(u16len >> 8));
        write_byte((uint8_t)(u16len & 0xFF));
        for (size_t i = 0; i < len; i++)
            write_byte(((const uint8_t*)data)[i]);
        return true;
    }

    uint32_t pop(void* out, uint32_t maxlen) {
        if (read_pos == write_pos) return 0;
        uint8_t hi  = read_byte();
        uint8_t lo  = read_byte();
        uint16_t len = (uint16_t)((hi << 8) | lo);
        uint32_t copy = (len < maxlen) ? len : maxlen;
        for (uint32_t i = 0; i < copy; i++)
            ((uint8_t*)out)[i] = read_byte();
        /* Discard any bytes we couldn't fit. */
        for (uint32_t i = copy; i < len; i++)
            read_byte();
        return copy;
    }

private:
    void write_byte(uint8_t b) {
        buf[write_pos % QUEUE_BYTES] = b;
        write_pos = (write_pos + 1) % QUEUE_BYTES;
    }
    uint8_t read_byte() {
        uint8_t b = buf[read_pos % QUEUE_BYTES];
        read_pos = (read_pos + 1) % QUEUE_BYTES;
        return b;
    }
};

static RecvQueue s_queues[NET_CH_COUNT];

/* ── Packet callback — installed by net_bindings_init() ── */

static void on_packet(uint8_t peer_id, net::Channel ch,
                       const void* data, size_t len) {
    uint8_t ch_idx = (uint8_t)ch;
    if (ch_idx < NET_CH_COUNT)
        s_queues[ch_idx].push(data, len);

    /* Also route to session layer for handshake / state management. */
    /* The session callback is installed in session::init(), but we route
     * system/state packets there explicitly so session.cpp can handle
     * MSG_HELLO, MSG_ENEMY_DAMAGE, MSG_WORLD_FLAG, etc.
     * player packets are forwarded by session::handle_packet as well. */
    (void)peer_id;  /* session.cpp's callback already has this via net:: */
}

/* ── Initialization — call after net::host_start or net::client_connect ── */

extern "C" void net_bindings_init() {
    for (auto& q : s_queues) {
        q.read_pos  = 0;
        q.write_pos = 0;
    }
    /* session::init() installs its own callbacks; we layer on top here
     * to also fill the recv queues for guest code to drain. */
    net::set_on_packet(on_packet);
    session::init();
}

/* ── Guest-callable functions (extern "C", resolved by the mod loader) ── */

extern "C" int wwhd_net_is_connected() {
    return net::is_connected() ? 1 : 0;
}

extern "C" int wwhd_net_is_host() {
    return net::is_host() ? 1 : 0;
}

extern "C" int wwhd_net_is_relay() {
    return net::is_relay() ? 1 : 0;
}

extern "C" int wwhd_net_is_p2p() {
    return net::is_p2p() ? 1 : 0;
}

extern "C" uint8_t wwhd_net_player_id() {
    return net::local_player_id();
}

extern "C" uint8_t wwhd_net_peer_count() {
    return net::peer_count();
}

/* Returns a pointer to the 6-char NUL-terminated lobby code (host only).
 * The string lives in static storage on the host side — the guest reads it
 * in place; it must not be written to or freed. */
extern "C" const char* wwhd_net_lobby_code() {
    return net::lobby_code();
}

extern "C" void wwhd_net_send_unreliable(uint8_t channel, uint8_t peer_id,
                                          const void* data, uint32_t len) {
    if (channel < NET_CH_COUNT)
        net::send_unreliable(peer_id,
                             static_cast<net::Channel>(channel),
                             data, len);
}

extern "C" void wwhd_net_send_reliable(uint8_t channel, uint8_t peer_id,
                                        const void* data, uint32_t len) {
    if (channel < NET_CH_COUNT)
        net::send_reliable(peer_id,
                           static_cast<net::Channel>(channel),
                           data, len);
}

extern "C" uint32_t wwhd_net_recv(uint8_t channel, void* buf, uint32_t maxlen) {
    if (channel >= NET_CH_COUNT || !buf || !maxlen) return 0;
    return s_queues[channel].pop(buf, maxlen);
}
