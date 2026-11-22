/* SPDX-License-Identifier: MPL-2.0
 * packets.h — Wire format shared between the guest mod (PowerPC C) and the
 * host network layer (native C++). Fixed-width integers only; no pointers.
 *
 * Include on the guest side via  #include "wwhd/net.h"  (which includes this).
 * Include on the host side via   #include "packets.h"   (from runtime-ext/src/net/).
 */
#pragma once
#include <stdint.h>

/* ── Channel assignments (must match net::Channel enum in net.h) ── */
#define NET_CH_PLAYER   0   /* unreliable, 20 Hz: player position + anim  */
#define NET_CH_ENEMY    1   /* unreliable, 20 Hz: enemy position + anim    */
#define NET_CH_STATE    2   /* reliable: HP deltas, deaths, world flags     */
#define NET_CH_SYSTEM   3   /* reliable: session handshake, disconnect      */
#define NET_CH_COUNT    4

/* ── Message type tags (first byte of every packet) ── */
#define MSG_PLAYER_STATE    0x01
#define MSG_ENEMY_STATE     0x02
#define MSG_ENEMY_DAMAGE    0x03  /* client → host: I hit this enemy        */
#define MSG_WORLD_FLAG      0x04  /* host → all: chest/switch/door changed  */
#define MSG_HELLO           0x10  /* host → new client: welcome + player id */
#define MSG_PLAYER_LIST     0x11  /* host → new client: existing player ids */
#define MSG_AREA_ENTER      0x12  /* any → host: I entered this stage/room  */
#define MSG_AUTH_REQ        0x13  /* client → host: authenticate with session secret */
#define MSG_AUTH_OK         0x14  /* host → client: secret accepted          */
#define MSG_AUTH_FAIL       0x15  /* host → client: secret rejected; will disconnect */
#define MSG_DISCONNECT      0x1F

#pragma pack(push, 1)

/* 10 bytes — sent unreliable on CH_PLAYER at 20 Hz */
typedef struct {
    uint8_t  type;        /* MSG_PLAYER_STATE */
    uint8_t  player_id;
    int16_t  x, y, z;    /* world position * 4 (range ±8192 m)  */
    uint16_t yaw;         /* y-rotation, 0–65535 wraps 360°      */
    uint8_t  anim_id;     /* animation index (0 = idle)           */
    uint8_t  flags;       /* bit 0: on_ground, bit 1: in_water    */
} PlayerPacket;           /* sizeof == 10 */

/* 20 bytes — sent unreliable on CH_ENEMY at 20 Hz (host only) */
typedef struct {
    uint8_t  type;        /* MSG_ENEMY_STATE */
    uint8_t  pad;
    uint16_t actor_index; /* slot in host's tracked-enemy array   */
    uint32_t actor_id;    /* fopAc_ac_c process handle (host key) */
    int16_t  x, y, z;    /* position * 4                          */
    uint16_t yaw;
    uint8_t  hp;
    uint8_t  phase;       /* 0=alive 1=flinch 2=stunned 3=dead    */
    uint8_t  anim_id;
    uint8_t  attack_flag; /* nonzero while attack hitbox is live  */
} EnemyPacket;            /* sizeof == 20 */

/* 7 bytes — sent reliable on CH_STATE, client → host */
typedef struct {
    uint8_t  type;        /* MSG_ENEMY_DAMAGE */
    uint8_t  player_id;   /* which player struck                  */
    uint32_t actor_id;    /* which enemy                          */
    uint8_t  damage;      /* amount (host validates and applies)  */
    uint8_t  damage_type; /* 0=sword 1=arrow 2=bomb 3=other       */
} EnemyDamagePacket;      /* sizeof == 7 */

/* 8 bytes — sent reliable on CH_STATE, host → all */
typedef struct {
    uint8_t  type;        /* MSG_WORLD_FLAG */
    uint8_t  flag_type;   /* 0=chest 1=switch 2=door 3=enemy_kill */
    uint16_t flag_index;  /* index into the stage's flag array    */
    uint8_t  value;       /* new flag value                       */
    uint8_t  pad[3];
} WorldFlagPacket;        /* sizeof == 8 */

/* 4 bytes — host → new client on connect */
typedef struct {
    uint8_t  type;        /* MSG_HELLO */
    uint8_t  player_id;   /* ID assigned to this client (1–N)    */
    uint8_t  player_count;/* total connected including host (0)   */
    uint8_t  flags;       /* reserved                             */
} HelloPacket;            /* sizeof == 4 */

/* 8 bytes — any direction, reliable on CH_SYSTEM */
typedef struct {
    uint8_t  type;        /* MSG_AREA_ENTER */
    uint8_t  player_id;
    uint16_t stage_id;    /* Wind Waker stage id                  */
    uint8_t  room_id;
    uint8_t  pad[3];
} AreaPacket;             /* sizeof == 8 */

/* 33 bytes — client → host on connect, reliable on CH_SYSTEM.
 * The secret is the 32-byte session secret received from the signaling server
 * (stored in P2PJoinResult.secret decoded from the hex string).
 * Hosts without P2P (direct/relay) accept any peer; this is enforced only
 * when the host has a non-empty session secret from p2p_create. */
typedef struct {
    uint8_t  type;        /* MSG_AUTH_REQ */
    uint8_t  secret[32];  /* raw 32-byte session secret */
} AuthReqPacket;          /* sizeof == 33 */

/* 1 byte — host → client: auth result */
typedef struct {
    uint8_t  type;        /* MSG_AUTH_OK or MSG_AUTH_FAIL */
} AuthResultPacket;       /* sizeof == 1 */

#pragma pack(pop)

/* ── Packet size validation ───────────────────────────────────────────────
 * Returns true if a packet of the given type and size is structurally valid.
 * Call before processing any received packet. */
#ifdef __cplusplus
inline bool packet_size_valid(uint8_t msg_type, uint32_t len) {
    switch (msg_type) {
    case MSG_PLAYER_STATE: return len == sizeof(PlayerPacket);
    case MSG_ENEMY_STATE:  return len == sizeof(EnemyPacket);
    case MSG_ENEMY_DAMAGE: return len == sizeof(EnemyDamagePacket);
    case MSG_WORLD_FLAG:   return len == sizeof(WorldFlagPacket);
    case MSG_HELLO:        return len == sizeof(HelloPacket);
    case MSG_AREA_ENTER:   return len == sizeof(AreaPacket);
    case MSG_AUTH_REQ:     return len == sizeof(AuthReqPacket);
    case MSG_AUTH_OK:      return len == sizeof(AuthResultPacket);
    case MSG_AUTH_FAIL:    return len == sizeof(AuthResultPacket);
    case MSG_DISCONNECT:   return len >= 1 && len <= 4;
    default:               return len >= 1 && len <= 512; /* unknown future types */
    }
}
#endif

/* ── Relay protocol (relay_server ↔ game client) ─────────────────────────
 *
 * These messages are exchanged on the relay server's UDP port (default 7778).
 * They are NEVER sent over the peer-to-peer ENet channels above; the relay
 * uses its own single-channel ENet host that all clients connect to.
 *
 * Session flow:
 *   Host:   RELAY_CREATE             → relay sends RELAY_CREATED + 6-char code
 *   Guest:  RELAY_JOIN + code[6]     → relay sends RELAY_PLAYER_JOINED + id
 *   Both:   game packets forwarded transparently with RELAY_GAME_DATA prefix
 */

/* Relay message type tags (first byte) */
#define RELAY_GAME_DATA      0x00  /* forwarded game packet (prepended by relay) */
#define RELAY_CREATE         0x20  /* client → relay: create a new room          */
#define RELAY_JOIN           0x21  /* client → relay: join room, body = code[6]  */
#define RELAY_CREATED        0x22  /* relay → host:   [type, id=0, code[6]]      */
#define RELAY_PLAYER_JOINED  0x23  /* relay → all:    [type, new_id, total, code[6] (to new only)] */
#define RELAY_PLAYER_LEFT    0x24  /* relay → all:    [type, left_id]            */
#define RELAY_ERROR          0x2F  /* relay → client: [type, error_code]         */

/* Error codes for RELAY_ERROR */
#define RELAY_ERR_ALREADY_IN_ROOM  0x01
#define RELAY_ERR_CODE_NOT_FOUND   0x02
#define RELAY_ERR_ROOM_FULL        0x03

#define RELAY_CODE_LEN 6  /* 6 uppercase alphanumeric chars, no 0/1/I/O */
