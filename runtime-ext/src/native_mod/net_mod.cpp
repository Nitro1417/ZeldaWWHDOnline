/* SPDX-License-Identifier: MPL-2.0
 * net_mod.cpp — WWHD Online native mod entry point.
 *
 * Build this file as a Windows DLL and place it in the game's mods/ folder
 * alongside any .wwhd guest mod packs. The game loads it via wwhd_mod_init_v1.
 *
 * Build (llvm-mingw, bundled with the WWHD SDK):
 *   x86_64-w64-mingw32-g++ -std=c++17 -O2 -shared -fPIC
 *       net_mod.cpp ../net/net.cpp ../net/session.cpp ../net/stun.cpp ../net/p2p.cpp
 *       -I../../include -I../net
 *       -Lpath/to/enet/lib -lenet
 *       -lws2_32 -lwinmm -lwinhttp
 *       -o wwhd_online.dll
 *
 * Command-line options (passed to the WWHD launcher):
 *   --online-host <port>                   Host a direct session on this port.
 *   --online-join <addr> <port>            Join a direct session.
 *   --online-p2p-create <signal_url>       Host P2P via Cloudflare Worker signaling.
 *   --online-p2p-join   <signal_url> <code> Join P2P lobby by 6-char code.
 *   --online-relay-create <host> <port>    Host via relay server.
 *   --online-relay-join   <host> <port> <code>  Join relay lobby by code.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "wwhd_mod.h"
#include "net.h"
#include "session.h"
#include "packets.h"

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

/* ── PPC layout constants (WWHD USA v0) ───────────────────────────────────── */

/* Offset of the player actor pointer within g_dComIfG_gameInfo. */
static constexpr uint32_t kGameInfoPlayerPtrOff = 0x5B2C;
/* Byte offset of fopAc_ac_c::current.pos.x within the actor struct. */
static constexpr uint32_t kActorCurrentPosOff   = 0x314;  /* cXyz pos  = 3×f32 BE */
static constexpr uint32_t kActorCurrentYawOff   = 0x320;  /* u16 angle.y          */

/* PPC func address of wwhd_dComIfG_getGameInfo (confirmed in bindings.h). */
static constexpr uint32_t kGetGameInfoAddr = 0x025200D4;

/* ── Module state ─────────────────────────────────────────────────────────── */

struct Mod {
    const WWHDModHostV1* host;
    uint32_t gameinfo_base;      /* decoded from kGetGameInfoAddr on init */
    uint32_t last_link_ppc;      /* last-known PPC address of Link's actor */
    uint64_t last_send_step;
    char     status_buf[128];
};

static Mod g_mod;

/* ── Big-endian helpers ───────────────────────────────────────────────────── */

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}

static float be_f32(const uint8_t* p) {
    uint32_t u = be32(p);
    float f; memcpy(&f, &u, 4);
    return f;
}

static uint16_t be16(const uint8_t* p) {
    return ((uint16_t)p[0]<<8)|p[1];
}

/* ── Decode the PPC getter to find g_dComIfG_gameInfo base ────────────────── */
/*
 * wwhd_dComIfG_getGameInfo is a trivial getter:
 *   lis r3, HI(g_dComIfG_gameInfo)
 *   ori r3, r3, LO(g_dComIfG_gameInfo)   OR   addi r3, r3, LO
 *   blr
 * We decode it to get the PPC base address without hardcoding per region.
 */
static uint32_t decode_gameinfo_base(const WWHDModHostV1* h) {
    uint8_t insns[12] = {};
    if (!h->read_guest(h->context, kGetGameInfoAddr, insns, 12))
        return 0;

    uint32_t i0 = be32(insns);
    /* lis r3, HI  →  addis r3, r0, HI  →  0x3C60HHHH */
    if ((i0 & 0xFFFF0000u) != 0x3C600000u) return 0;
    uint32_t hi = i0 & 0xFFFFu;

    uint32_t i1 = be32(insns + 4);
    if ((i1 & 0xFFFF0000u) == 0x60630000u) {
        /* ori r3, r3, LO */
        return (hi << 16) | (i1 & 0xFFFFu);
    }
    if ((i1 & 0xFFFF0000u) == 0x38630000u) {
        /* addi r3, r3, LO (sign-extended) */
        return (uint32_t)((int32_t)(hi << 16) + (int16_t)(i1 & 0xFFFFu));
    }
    if (i1 == 0x4E800020u) {
        /* blr immediately → no low half */
        return hi << 16;
    }
    return 0;
}

/* ── Read Link's current world position ──────────────────────────────────── */

static bool read_link_pos(float* x, float* y, float* z, uint16_t* yaw) {
    if (!g_mod.gameinfo_base) return false;
    const WWHDModHostV1* h = g_mod.host;

    /* Step 1: read the player-actor PPC pointer from gameInfo. */
    uint8_t ptr_buf[4] = {};
    if (!h->read_guest(h->context,
                       g_mod.gameinfo_base + kGameInfoPlayerPtrOff,
                       ptr_buf, 4))
        return false;
    uint32_t link_ppc = be32(ptr_buf);
    if (!link_ppc) return false;
    g_mod.last_link_ppc = link_ppc;

    /* Step 2: read position fields (3×f32 big-endian) from the actor. */
    uint8_t pos_buf[14] = {};  /* pos.x/y/z (12 bytes) + 2 bytes pad + yaw (2 bytes) */
    if (!h->read_guest(h->context,
                       link_ppc + kActorCurrentPosOff,
                       pos_buf, 14))
        return false;

    *x   = be_f32(pos_buf + 0);
    *y   = be_f32(pos_buf + 4);
    *z   = be_f32(pos_buf + 8);
    *yaw = be16(pos_buf + 12); /* current.angle.y is at 0x320 = 0x314 + 0xC */
    return true;
}

/* ── Position sanity check ───────────────────────────────────────────────── */

static bool pos_valid(float x, float y, float z) {
    return (x == x) && (y == y) && (z == z)
        && (x >= -16000.f) && (x <= 16000.f)
        && (y >= -4000.f)  && (y <= 8000.f)
        && (z >= -16000.f) && (z <= 16000.f);
}

/* ── Command-line parser ─────────────────────────────────────────────────── */

struct OnlineArgs {
    enum class Mode { None, Host, Join, P2PCreate, P2PJoin, RelayCreate, RelayJoin };
    Mode        mode        = Mode::None;
    char        addr[256]   = {};  /* host addr or relay/signal url */
    uint16_t    port        = 0;
    char        code[8]     = {};  /* 6-char lobby code */
    char        signal_url[512] = {};
};

static std::vector<std::string> split_cmdline(const wchar_t* wstr) {
    std::vector<std::string> args;
    bool in_q = false;
    std::string cur;
    for (const wchar_t* p = wstr; *p; ++p) {
        if (*p == L'"') { in_q = !in_q; continue; }
        if (*p == L' ' && !in_q) {
            if (!cur.empty()) { args.push_back(cur); cur.clear(); }
        } else {
            cur += (char)*p;
        }
    }
    if (!cur.empty()) args.push_back(cur);
    return args;
}

static OnlineArgs parse_online_args() {
    OnlineArgs a;
    auto args = split_cmdline(GetCommandLineW());
    for (size_t i = 0; i < args.size(); ++i) {
        const auto& t = args[i];
        auto next = [&](size_t off = 1) -> const char* {
            return (i + off < args.size()) ? args[i + off].c_str() : "";
        };
        if (t == "--online-host" && i+1 < args.size()) {
            a.mode = OnlineArgs::Mode::Host;
            a.port = (uint16_t)atoi(next()); i++;
        } else if (t == "--online-join" && i+2 < args.size()) {
            a.mode = OnlineArgs::Mode::Join;
            strncpy(a.addr, next(1), 255); a.addr[255] = 0;
            a.port = (uint16_t)atoi(next(2)); i += 2;
        } else if (t == "--online-p2p-create" && i+1 < args.size()) {
            a.mode = OnlineArgs::Mode::P2PCreate;
            strncpy(a.signal_url, next(), 511); a.signal_url[511] = 0; i++;
        } else if (t == "--online-p2p-join" && i+2 < args.size()) {
            a.mode = OnlineArgs::Mode::P2PJoin;
            strncpy(a.signal_url, next(1), 511); a.signal_url[511] = 0;
            strncpy(a.code, next(2), 7); a.code[7] = 0; i += 2;
        } else if (t == "--online-relay-create" && i+2 < args.size()) {
            a.mode = OnlineArgs::Mode::RelayCreate;
            strncpy(a.addr, next(1), 255); a.addr[255] = 0;
            a.port = (uint16_t)atoi(next(2)); i += 2;
        } else if (t == "--online-relay-join" && i+3 < args.size()) {
            a.mode = OnlineArgs::Mode::RelayJoin;
            strncpy(a.addr, next(1), 255); a.addr[255] = 0;
            a.port = (uint16_t)atoi(next(2));
            strncpy(a.code, next(3), 7); a.code[7] = 0; i += 3;
        }
    }
    return a;
}

/* ── Frame callbacks ─────────────────────────────────────────────────────── */

static void on_frame(void* /*instance*/, uint64_t logic_step) {
    /* Drain ENet and handle session events. */
    net::poll();
    session::tick();

    const WWHDModHostV1* h = g_mod.host;

    /* Display lobby code once per second (every 60 logic steps). */
    if ((net::is_relay() || net::is_p2p()) && net::is_host() && net::is_connected()) {
        if (logic_step % 60 == 0) {
            const char* code = net::lobby_code();
            if (code && code[0]) {
                snprintf(g_mod.status_buf, sizeof g_mod.status_buf,
                         "WWHD Online — room: %s", code);
                h->status(h->context, g_mod.status_buf);
            }
        }
    }

    if (!net::is_connected()) return;

    /* Send position at ~20 Hz (every 3 logic steps at 60 fps). */
    if (logic_step % 3 != 0) return;

    float x, y, z;  uint16_t yaw;
    if (!read_link_pos(&x, &y, &z, &yaw)) return;
    if (!pos_valid(x, y, z)) return;

    PlayerPacket pkt = {};
    pkt.type      = MSG_PLAYER_STATE;
    pkt.player_id = net::local_player_id();
    pkt.x         = (int16_t)(x * 4.f);
    pkt.y         = (int16_t)(y * 4.f);
    pkt.z         = (int16_t)(z * 4.f);
    pkt.yaw       = yaw;

    if (net::is_host())
        net::send_unreliable(net::NET_BROADCAST, net::Channel::Player, &pkt, sizeof pkt);
    else
        net::send_unreliable(net::NET_HOST_ID,   net::Channel::Player, &pkt, sizeof pkt);
}

static void on_config_changed(void* /*instance*/) {}

static void on_unload(void* /*instance*/) {
    net::shutdown();
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

WWHD_MOD_EXPORT int wwhd_mod_init_v1(const WWHDModHostV1* host, WWHDModV1* mod) {
    if (!host || !mod) return 0;
    if (host->abi_version < WWHD_MOD_ABI_VERSION) return 0;

    g_mod.host          = host;
    g_mod.gameinfo_base = decode_gameinfo_base(host);

    if (g_mod.gameinfo_base)
        host->log(host->context, "WWHD Online: gameInfo decoded OK");
    else
        host->log(host->context,
                  "WWHD Online: WARNING — could not decode gameInfo base; "
                  "position sync disabled until addressed.");

    /* Parse --online-* from the game process command line. */
    OnlineArgs a = parse_online_args();
    bool ok = false;

    switch (a.mode) {
    case OnlineArgs::Mode::Host:
        ok = net::host_start(a.port);
        if (ok) host->log(host->context, "WWHD Online: hosting direct session");
        break;
    case OnlineArgs::Mode::Join:
        ok = net::client_connect(a.addr, a.port);
        if (ok) host->log(host->context, "WWHD Online: connected (direct)");
        break;
    case OnlineArgs::Mode::P2PCreate:
        ok = net::p2p_create(a.signal_url);
        if (ok) host->log(host->context, "WWHD Online: P2P lobby created");
        break;
    case OnlineArgs::Mode::P2PJoin:
        ok = net::p2p_join(a.signal_url, a.code);
        if (ok) host->log(host->context, "WWHD Online: P2P connected");
        break;
    case OnlineArgs::Mode::RelayCreate:
        ok = net::relay_create(a.addr, a.port);
        if (ok) host->log(host->context, "WWHD Online: relay lobby created");
        break;
    case OnlineArgs::Mode::RelayJoin:
        ok = net::relay_join(a.addr, a.port, a.code);
        if (ok) host->log(host->context, "WWHD Online: relay connected");
        break;
    case OnlineArgs::Mode::None:
        host->log(host->context, "WWHD Online: no --online-* args; mod loaded but inactive");
        ok = false;
        break;
    }

    if (a.mode != OnlineArgs::Mode::None && !ok) {
        host->log(host->context, "WWHD Online: connection failed; mod inactive");
    }

    /* Fill the mod descriptor. */
    mod->size             = sizeof(WWHDModV1);
    mod->abi_version      = WWHD_MOD_ABI_VERSION;
    mod->instance         = nullptr;
    mod->on_frame         = on_frame;
    mod->on_config_changed = on_config_changed;
    mod->on_unload        = on_unload;

    return 1;  /* always succeed so the game logs our messages */
}
