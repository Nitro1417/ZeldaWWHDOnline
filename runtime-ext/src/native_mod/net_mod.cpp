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
#include <cstdarg>
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

/* Save-info HP reading (from wwhd/data.h + wwhd/save.h):
 *   save_info_ptr  = *(u32 BE at kSaveInfoPtrAddr)
 *   dSv_player_status_a_view at save_info_ptr + kSaveInfoOff:
 *     u16 BE max_life (quarter-hearts)
 *     u16 BE life     (quarter-hearts, 3 hearts = 12) */
static constexpr uint32_t kSaveInfoPtrAddr = 0x101F84DC;
static constexpr uint32_t kSaveInfoOff     = 0x20;

/* ── File log (visible even in GUI processes) ─────────────────────────────── */
static FILE* g_log = nullptr;

static void log_open() {
    char path[MAX_PATH];
    GetTempPathA(MAX_PATH, path);
    strncat(path, "wwhd_online.log", MAX_PATH - strlen(path) - 1);
    g_log = fopen(path, "a");
    if (g_log) { fprintf(g_log, "--- wwhd_mod_init_v1 ---\n"); fflush(g_log); }
}
static void logf(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}

/* ── Module state ─────────────────────────────────────────────────────────── */

struct Mod {
    const WWHDModHostV1* host;
    uint32_t gameinfo_base;      /* decoded from kGetGameInfoAddr on init */
    uint32_t last_link_ppc;      /* last-known PPC address of Link's actor */
    uint64_t last_send_step;
    char     status_buf[128];
};

static Mod  g_mod;
static char g_local_name[16] = "Link";   /* from ini name= key */
static volatile LONG g_pending_chat_flag = 0;
static char          g_pending_chat_text[64] = {};

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

/* ── Read local player life from save data ───────────────────────────────── */
static bool read_local_life(uint8_t* life_out, uint8_t* max_life_out) {
    const WWHDModHostV1* h = g_mod.host;
    if (!h) return false;
    uint8_t ptr_buf[4] = {};
    if (!h->read_guest(h->context, kSaveInfoPtrAddr, ptr_buf, 4)) return false;
    uint32_t save_info_ptr = be32(ptr_buf);
    if (!save_info_ptr) return false;
    uint8_t life_buf[4] = {};
    if (!h->read_guest(h->context, save_info_ptr + kSaveInfoOff, life_buf, 4)) return false;
    uint16_t ml = be16(life_buf + 0);
    uint16_t lf = be16(life_buf + 2);
    *max_life_out = (ml > 255) ? 255 : (uint8_t)ml;
    *life_out     = (lf > 255) ? 255 : (uint8_t)lf;
    return true;
}

/* ── DLL path (set by DllMain) ───────────────────────────────────────────── */
static HMODULE g_hmod = nullptr;

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

/* ── Config file parser ──────────────────────────────────────────────────── */
/* Returns directory containing the DLL, with trailing backslash. */
static std::string dll_dir() {
    char buf[MAX_PATH] = {};
    GetModuleFileNameA(g_hmod, buf, MAX_PATH);
    std::string s(buf);
    auto pos = s.find_last_of("\\/");
    return (pos != std::string::npos) ? s.substr(0, pos + 1) : "";
}

/* Reads wwhd_online.ini from the same folder as the DLL. */
static OnlineArgs parse_config_file() {
    OnlineArgs a;
    std::string path = dll_dir() + "wwhd_online.ini";
    FILE* f = fopen(path.c_str(), "r");
    if (!f) { logf("no config file at %s", path.c_str()); return a; }

    char line[512];
    while (fgets(line, sizeof line, f)) {
        for (char* p = line; *p; p++) if (*p == '\r' || *p == '\n') { *p = '\0'; break; }
        if (!line[0] || line[0] == '#' || line[0] == ';') continue;
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char* key = line;
        const char* val = eq + 1;
        while (*val == ' ') val++;

        if (strcmp(key, "mode") == 0) {
            if (strcmp(val, "host") == 0 || strcmp(val, "p2p-host") == 0)
                a.mode = OnlineArgs::Mode::P2PCreate;
            else if (strcmp(val, "join") == 0 || strcmp(val, "p2p-join") == 0)
                a.mode = OnlineArgs::Mode::P2PJoin;
            else if (strcmp(val, "direct-host") == 0)  a.mode = OnlineArgs::Mode::Host;
            else if (strcmp(val, "direct-join") == 0)  a.mode = OnlineArgs::Mode::Join;
            else if (strcmp(val, "relay-host") == 0)   a.mode = OnlineArgs::Mode::RelayCreate;
            else if (strcmp(val, "relay-join") == 0)   a.mode = OnlineArgs::Mode::RelayJoin;
        } else if (strcmp(key, "signal_url") == 0) {
            strncpy(a.signal_url, val, 511); a.signal_url[511] = 0;
        } else if (strcmp(key, "addr") == 0) {
            strncpy(a.addr, val, 255); a.addr[255] = 0;
        } else if (strcmp(key, "port") == 0) {
            a.port = (uint16_t)atoi(val);
        } else if (strcmp(key, "code") == 0) {
            strncpy(a.code, val, 7); a.code[7] = 0;
        } else if (strcmp(key, "name") == 0) {
            strncpy(g_local_name, val, 15); g_local_name[15] = 0;
        }
    }
    fclose(f);
    logf("config: mode=%d signal_url=%s code=%s", (int)a.mode, a.signal_url, a.code);
    return a;
}

/* ── Merged config: manifest options (F1 Mods) > ini > cmdline ─────────── */
static OnlineArgs build_config() {
    OnlineArgs a = parse_config_file();
    if (a.mode == OnlineArgs::Mode::None) a = parse_online_args();

    const WWHDModHostV1* h = g_mod.host;
    if (!h) return a;
    const char* m = h->get_string(h->context, "mode");
    const char* s = h->get_string(h->context, "signal_url");

    if (m && m[0]) {
        if      (strcmp(m, "host")       == 0) a.mode = OnlineArgs::Mode::P2PCreate;
        else if (strcmp(m, "join")       == 0) a.mode = OnlineArgs::Mode::P2PJoin;
        else if (strcmp(m, "relay-host") == 0) a.mode = OnlineArgs::Mode::RelayCreate;
        else if (strcmp(m, "relay-join") == 0) a.mode = OnlineArgs::Mode::RelayJoin;
        else if (strcmp(m, "disabled")   == 0) a.mode = OnlineArgs::Mode::None;
    }
    if (s && s[0]) { strncpy(a.signal_url, s, 511); a.signal_url[511] = 0; }
    return a;
}

/* ── On-screen overlay (GDI layered windows) ────────────────────────────── */
/* Bar:   460×36 strip at top-center — always visible when lobby is active.  */
/* Panel: 352×215 info card at top-right — press F9 to toggle.              */

static volatile HWND  g_bar_hwnd     = nullptr;
static volatile HWND  g_panel_hwnd   = nullptr;
static volatile HWND  g_hud_hwnd     = nullptr;
static volatile HWND  g_toast_hwnd   = nullptr;
static volatile bool  g_panel_vis    = false;
static HANDLE         g_overlay_thrd = nullptr;
static DWORD          g_overlay_tid  = 0;
static char          g_bar_text[128]     = {};
static char          g_panel_line[5][80] = {};
static UINT          g_wm_update         = 0;
static const int     HOTKEY_ID_PANEL     = 1;
static const int     HOTKEY_ID_COPY      = 2;
static const int     HOTKEY_ID_CHAT      = 3;
static const UINT_PTR TIMER_ID_FLASH     = 1;
static const UINT_PTR TIMER_ID_TOAST     = 2;
static char          g_bar_saved[128]    = {};
static volatile bool g_toast_new_msg     = false;

/* Bar: single-line status strip ─────────────────────────────────────────── */
static LRESULT CALLBACK BarWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    (void)lp;
    if (g_wm_update && msg == g_wm_update) {
        InvalidateRect(hwnd, nullptr, TRUE);
        if (g_panel_hwnd && g_panel_vis)
            InvalidateRect((HWND)g_panel_hwnd, nullptr, TRUE);
        if (g_hud_hwnd)
            PostMessageA((HWND)g_hud_hwnd, g_wm_update, 0, 0);
        if (g_toast_hwnd)
            PostMessageA((HWND)g_toast_hwnd, g_wm_update, 0, 0);
        return 0;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(8, 10, 22));
        FillRect(hdc, &rc, bg); DeleteObject(bg);
        SetTextColor(hdc, RGB(255, 220, 50));
        SetBkMode(hdc, TRANSPARENT);
        HFONT font = CreateFontA(18, 0, 0, 0, FW_BOLD, 0, 0, 0,
                                 ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Consolas");
        HFONT old = (HFONT)SelectObject(hdc, font);
        char buf[128]; memcpy(buf, g_bar_text, 128);
        DrawTextA(hdc, buf, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, old); DeleteObject(font);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_TIMER && wp == TIMER_ID_FLASH) {
        KillTimer(hwnd, TIMER_ID_FLASH);
        memcpy(g_bar_text, g_bar_saved, 128);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* Panel: multi-line info card ───────────────────────────────────────────── */
static LRESULT CALLBACK PanelWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    (void)wp; (void)lp;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);

        HBRUSH bg = CreateSolidBrush(RGB(10, 12, 28));
        FillRect(hdc, &rc, bg); DeleteObject(bg);

        /* Cyan left accent stripe */
        HBRUSH stripe = CreateSolidBrush(RGB(0, 190, 225));
        RECT sl = {0, 0, 4, rc.bottom};
        FillRect(hdc, &sl, stripe); DeleteObject(stripe);

        SetBkMode(hdc, TRANSPARENT);

        HFONT ftitle = CreateFontA(20, 0, 0, 0, FW_BOLD, 0, 0, 0,
                                   ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        HFONT fbody  = CreateFontA(15, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                   ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Consolas");

        /* Title */
        SelectObject(hdc, ftitle);
        SetTextColor(hdc, RGB(0, 210, 255));
        RECT tr = {12, 10, rc.right - 6, 34};
        DrawTextA(hdc, "WWHD ONLINE", -1, &tr, DT_LEFT | DT_TOP);

        /* Separator line */
        HPEN hpen = CreatePen(PS_SOLID, 1, RGB(0, 90, 120));
        HPEN hpen_old = (HPEN)SelectObject(hdc, hpen);
        MoveToEx(hdc, 12, 36, nullptr); LineTo(hdc, rc.right - 8, 36);
        SelectObject(hdc, hpen_old); DeleteObject(hpen);

        /* Content lines */
        SelectObject(hdc, fbody);
        int y = 44;
        for (int i = 0; i < 5; i++) {
            char buf[80]; memcpy(buf, g_panel_line[i], 80);
            if (!buf[0]) { y += 22; continue; }
            /* Line 1 (room code) is bright yellow; others are light grey */
            SetTextColor(hdc, (i == 1) ? RGB(255, 230, 50) : RGB(190, 205, 225));
            RECT lr = {12, y, rc.right - 8, y + 20};
            DrawTextA(hdc, buf, -1, &lr, DT_LEFT | DT_TOP);
            y += 22;
        }

        /* Footer hint */
        SetTextColor(hdc, RGB(70, 85, 120));
        RECT fr = {12, rc.bottom - 20, rc.right - 8, rc.bottom - 4};
        DrawTextA(hdc, "[F7] chat  [F8] copy code  [F9] close", -1, &fr, DT_LEFT | DT_TOP);

        DeleteObject(ftitle); DeleteObject(fbody);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_DESTROY) { return 0; }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* Forward declarations for dialogs used by the overlay thread */
static bool prompt_chat_msg(char* out, size_t n);

/* ── Peer HUD window (top-left, always visible when peers are connected) ──── */
static LRESULT CALLBACK HudWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    (void)wp; (void)lp;
    if (g_wm_update && msg == g_wm_update) {
        uint8_t peers = net::peer_count();
        bool vis = IsWindowVisible(hwnd) != 0;
        if (peers > 0 && !vis)  ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        if (peers == 0 && vis)  ShowWindow(hwnd, SW_HIDE);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);

        HBRUSH bg = CreateSolidBrush(RGB(10, 12, 28));
        FillRect(hdc, &rc, bg); DeleteObject(bg);

        HBRUSH stripe = CreateSolidBrush(RGB(0, 190, 225));
        RECT sl = {0, 0, 4, rc.bottom};
        FillRect(hdc, &sl, stripe); DeleteObject(stripe);

        SetBkMode(hdc, TRANSPARENT);

        HFONT ftitle = CreateFontA(13, 0, 0, 0, FW_BOLD, 0, 0, 0,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        HFONT fbody = CreateFontA(13, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Consolas");

        SelectObject(hdc, ftitle);
        SetTextColor(hdc, RGB(0, 210, 255));
        RECT tr = {8, 4, rc.right - 4, 22};
        DrawTextA(hdc, "PLAYERS", -1, &tr, DT_LEFT | DT_TOP);

        HPEN sep = CreatePen(PS_SOLID, 1, RGB(0, 70, 100));
        HPEN oldPen = (HPEN)SelectObject(hdc, sep);
        MoveToEx(hdc, 4, 24, nullptr); LineTo(hdc, rc.right, 24);
        SelectObject(hdc, oldPen); DeleteObject(sep);

        SelectObject(hdc, fbody);
        int y = 28;
        uint8_t local_id = net::local_player_id();
        for (uint8_t id = 0; id < session::MAX_PLAYERS; id++) {
            const session::Player* p = session::get_player(id);
            if (!p || !p->active || id == local_id) continue;

            char name[16];
            strncpy(name, p->name[0] ? p->name : "Player", 15); name[15] = '\0';

            SetTextColor(hdc, RGB(190, 205, 225));
            RECT nr = {8, y, 100, y + 18};
            DrawTextA(hdc, name, -1, &nr, DT_LEFT | DT_TOP | DT_END_ELLIPSIS);

            if (p->max_life > 0) {
                /* HP bar */
                RECT barBack = {104, y + 4, 204, y + 14};
                HBRUSH bbr = CreateSolidBrush(RGB(20, 25, 45));
                FillRect(hdc, &barBack, bbr); DeleteObject(bbr);

                int pct = (int)(100 * p->life / p->max_life);
                if (pct > 0) {
                    COLORREF col = (pct > 50) ? RGB(220, 180, 50) :
                                   (pct > 25) ? RGB(220, 110, 30) : RGB(200, 50, 50);
                    RECT barFill = {104, y + 4, 104 + pct, y + 14};
                    HBRUSH fbr = CreateSolidBrush(col);
                    FillRect(hdc, &barFill, fbr); DeleteObject(fbr);
                }

                char hptxt[8];
                snprintf(hptxt, sizeof hptxt, "%u", (unsigned)(p->life / 4));
                SetTextColor(hdc, RGB(255, 220, 50));
                RECT hr = {208, y, rc.right - 4, y + 18};
                DrawTextA(hdc, hptxt, -1, &hr, DT_LEFT | DT_TOP);
            } else {
                SetTextColor(hdc, RGB(70, 85, 120));
                RECT hr = {104, y, rc.right - 4, y + 18};
                DrawTextA(hdc, "?", -1, &hr, DT_LEFT | DT_TOP);
            }

            y += 26;
            if (y + 26 > rc.bottom) break;
        }

        DeleteObject(ftitle); DeleteObject(fbody);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_DESTROY) return 0;
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ── Chat toast window (bottom-center, appears on new message) ───────────── */
static LRESULT CALLBACK ChatToastWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    (void)lp;
    if (g_wm_update && msg == g_wm_update) {
        if (g_toast_new_msg) {
            g_toast_new_msg = false;
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            SetTimer(hwnd, TIMER_ID_TOAST, 8000, nullptr);
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
    }
    if (msg == WM_TIMER && wp == TIMER_ID_TOAST) {
        KillTimer(hwnd, TIMER_ID_TOAST);
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);

        HBRUSH bg = CreateSolidBrush(RGB(10, 12, 28));
        FillRect(hdc, &rc, bg); DeleteObject(bg);

        HBRUSH stripe = CreateSolidBrush(RGB(0, 190, 225));
        RECT sl = {0, 0, 4, rc.bottom};
        FillRect(hdc, &sl, stripe); DeleteObject(stripe);

        SetBkMode(hdc, TRANSPARENT);
        HFONT fbody = CreateFontA(13, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Consolas");
        SelectObject(hdc, fbody);

        int n = session::chat_count();
        if (n > 4) n = 4;
        int y = 6;
        for (int i = 0; i < n; i++) {
            session::ChatEntry e = session::chat_get(i);
            char line[82];
            snprintf(line, sizeof line, "%s: %s", e.name, e.text);
            SetTextColor(hdc, (i == 0) ? RGB(255, 230, 50) : RGB(190, 205, 225));
            RECT lr = {8, y, rc.right - 4, y + 18};
            DrawTextA(hdc, line, -1, &lr, DT_LEFT | DT_TOP | DT_END_ELLIPSIS);
            y += 20;
        }

        DeleteObject(fbody);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_DESTROY) return 0;
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static DWORD WINAPI overlay_thread_fn(LPVOID) {
    g_wm_update = RegisterWindowMessageA("WWHD_ONLINE_OVERLAY_UPDATE");

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc); wc.hInstance = g_hmod;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);

    wc.lpfnWndProc = BarWndProc;       wc.lpszClassName = "WWHDOnlineBar";
    RegisterClassExA(&wc);
    wc.lpfnWndProc = PanelWndProc;    wc.lpszClassName = "WWHDOnlinePanel";
    RegisterClassExA(&wc);
    wc.lpfnWndProc = HudWndProc;      wc.lpszClassName = "WWHDOnlineHud";
    RegisterClassExA(&wc);
    wc.lpfnWndProc = ChatToastWndProc; wc.lpszClassName = "WWHDOnlineChatToast";
    RegisterClassExA(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    /* Bar window */
    HWND bar = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        "WWHDOnlineBar", "", WS_POPUP,
        (sw - 460) / 2, 8, 460, 36,
        nullptr, nullptr, g_hmod, nullptr);
    if (!bar) return 1;
    SetLayeredWindowAttributes(bar, 0, 215, LWA_ALPHA);
    ShowWindow(bar, SW_SHOWNOACTIVATE); UpdateWindow(bar);
    g_bar_hwnd = bar;

    /* Panel window (initially hidden) */
    HWND panel = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        "WWHDOnlinePanel", "", WS_POPUP,
        sw - 360, 55, 352, 215,
        nullptr, nullptr, g_hmod, nullptr);
    if (panel) {
        SetLayeredWindowAttributes(panel, 0, 235, LWA_ALPHA);
        g_panel_hwnd = panel;
    }

    /* HUD window — top-left, initially hidden; shown when peers connect */
    HWND hud = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        "WWHDOnlineHud", "", WS_POPUP,
        10, 50, 240, 160,
        nullptr, nullptr, g_hmod, nullptr);
    if (hud) {
        SetLayeredWindowAttributes(hud, 0, 215, LWA_ALPHA);
        g_hud_hwnd = hud;
    }

    /* Chat toast window — bottom-center, initially hidden */
    HWND toast = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        "WWHDOnlineChatToast", "", WS_POPUP,
        (sw - 340) / 2, sh - 120, 340, 96,
        nullptr, nullptr, g_hmod, nullptr);
    if (toast) {
        SetLayeredWindowAttributes(toast, 0, 215, LWA_ALPHA);
        g_toast_hwnd = toast;
    }

    /* Hotkeys — received as thread messages (hwnd=NULL) */
    RegisterHotKey(nullptr, HOTKEY_ID_PANEL, 0, VK_F9);
    RegisterHotKey(nullptr, HOTKEY_ID_COPY,  0, VK_F8);
    RegisterHotKey(nullptr, HOTKEY_ID_CHAT,  0, VK_F7);

    MSG m;
    while (GetMessageA(&m, nullptr, 0, 0) > 0) {
        if (m.message == WM_HOTKEY && (int)m.wParam == HOTKEY_ID_PANEL) {
            g_panel_vis = !g_panel_vis;
            if (g_panel_hwnd)
                ShowWindow((HWND)g_panel_hwnd,
                           g_panel_vis ? SW_SHOWNOACTIVATE : SW_HIDE);
            continue;
        }
        if (m.message == WM_HOTKEY && (int)m.wParam == HOTKEY_ID_CHAT) {
            if (net::is_connected()) {
                char text[64] = {};
                if (prompt_chat_msg(text, sizeof text)) {
                    /* Pass to game thread via atomic flag */
                    strncpy(g_pending_chat_text, text, 63); g_pending_chat_text[63] = '\0';
                    InterlockedExchange(&g_pending_chat_flag, 1);
                    /* Show locally immediately */
                    session::add_local_chat(g_local_name, text);
                    g_toast_new_msg = true;
                    if (g_bar_hwnd && g_wm_update)
                        PostMessageA((HWND)g_bar_hwnd, g_wm_update, 0, 0);
                }
            }
            continue;
        }
        if (m.message == WM_HOTKEY && (int)m.wParam == HOTKEY_ID_COPY) {
            const char* code = net::lobby_code();
            if (code && code[0]) {
                size_t len = strlen(code) + 1;
                HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, len);
                if (hg) {
                    memcpy(GlobalLock(hg), code, len);
                    GlobalUnlock(hg);
                    if (OpenClipboard(nullptr)) {
                        EmptyClipboard();
                        SetClipboardData(CF_TEXT, hg);
                        CloseClipboard();
                    } else {
                        GlobalFree(hg);
                    }
                }
                memcpy(g_bar_saved, g_bar_text, 128);
                snprintf(g_bar_text, 128, "Copied!  %s  — paste to share  [F9]", code);
                InvalidateRect((HWND)g_bar_hwnd, nullptr, TRUE);
                SetTimer((HWND)g_bar_hwnd, TIMER_ID_FLASH, 2000, nullptr);
            }
            continue;
        }
        TranslateMessage(&m); DispatchMessageA(&m);
    }
    UnregisterHotKey(nullptr, HOTKEY_ID_PANEL);
    UnregisterHotKey(nullptr, HOTKEY_ID_COPY);
    UnregisterHotKey(nullptr, HOTKEY_ID_CHAT);
    g_bar_hwnd = nullptr; g_panel_hwnd = nullptr;
    g_hud_hwnd = nullptr; g_toast_hwnd = nullptr;
    return 0;
}

static void overlay_show(const char* bar,
                         const char* l0 = nullptr, const char* l1 = nullptr,
                         const char* l2 = nullptr, const char* l3 = nullptr,
                         const char* l4 = nullptr) {
    auto cp = [](char* dst, size_t n, const char* src) {
        if (src) { strncpy(dst, src, n - 1); dst[n - 1] = '\0'; }
    };
    cp(g_bar_text,       128, bar);
    cp(g_panel_line[0],  80,  l0);
    cp(g_panel_line[1],  80,  l1);
    cp(g_panel_line[2],  80,  l2);
    cp(g_panel_line[3],  80,  l3);
    cp(g_panel_line[4],  80,  l4);

    if (!g_bar_hwnd) {
        g_overlay_thrd = CreateThread(nullptr, 0, overlay_thread_fn, nullptr, 0, &g_overlay_tid);
    } else if (g_wm_update)
        PostMessageA((HWND)g_bar_hwnd, g_wm_update, 0, 0);
}

/* ── Clipboard helper: returns true if clipboard has a valid 6-char code ── */
static bool read_clipboard_code(char* out, size_t n) {
    if (!OpenClipboard(nullptr)) return false;
    HANDLE h = GetClipboardData(CF_TEXT);
    if (!h) { CloseClipboard(); return false; }
    const char* text = (const char*)GlobalLock(h);
    bool valid = false;
    if (text) {
        const char* p = text;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        size_t len = 0;
        while (p[len] && p[len] != ' ' && p[len] != '\t' && p[len] != '\r' && p[len] != '\n') len++;
        if (len == 6) {
            valid = true;
            for (size_t i = 0; i < 6; i++)
                if (!isalnum((unsigned char)p[i])) { valid = false; break; }
            if (valid && n > 6) { memcpy(out, p, 6); out[6] = '\0'; }
        }
        GlobalUnlock(h);
    }
    CloseClipboard();
    return valid;
}

/* ── Join-code input dialog ──────────────────────────────────────────────── */
static bool  s_code_ok  = false;
static char  s_code_buf[8] = {};
#define WC_CODEINPUT   "WWHDCodeInput"
#define IDC_EDIT_CODE  101
#define IDC_BTN_JOIN   102

/* GDI objects owned by the dialog (created in WM_CREATE, freed in WM_DESTROY) */
static HBRUSH s_dlg_bg_br     = nullptr;
static HBRUSH s_dlg_edit_br   = nullptr;
static HBRUSH s_dlg_hdr_br    = nullptr;
static HFONT  s_dlg_fnt_title = nullptr;
static HFONT  s_dlg_fnt_label = nullptr;
static HFONT  s_dlg_fnt_btn   = nullptr;

/*
 * Client layout (304 × 180 client area):
 *   y  0-42  : header ("JOIN WWHD ONLINE", dark navy + cyan stripe)
 *   y 42     : separator line
 *   y 52-76  : label "Room code from your friend:"
 *   y 76-108 : edit border ring; edit child at (16, 78, 272×28)
 *  y118-154  : Join (14,118,133×36) | Cancel (157,118,133×36)
 */
static LRESULT CALLBACK CodeInputProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        s_dlg_bg_br   = CreateSolidBrush(RGB(10, 12, 28));
        s_dlg_edit_br = CreateSolidBrush(RGB(18, 22, 44));
        s_dlg_hdr_br  = CreateSolidBrush(RGB(8, 10, 22));
        s_dlg_fnt_title = CreateFontA(20, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        s_dlg_fnt_label = CreateFontA(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        s_dlg_fnt_btn = CreateFontA(15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, ANSI_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

        HWND ed = CreateWindowExA(0, "EDIT", "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_UPPERCASE | ES_CENTER,
            16, 78, 272, 28, hwnd, (HMENU)(UINT_PTR)IDC_EDIT_CODE, g_hmod, nullptr);
        SendMessageA(ed, WM_SETFONT, (WPARAM)s_dlg_fnt_label, TRUE);
        SendMessageA(ed, EM_SETLIMITTEXT, 6, 0);

        char clip[8] = {};
        if (read_clipboard_code(clip, sizeof clip)) SetWindowTextA(ed, clip);

        HWND bj = CreateWindowExA(0, "BUTTON", "Join",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            14, 118, 133, 36, hwnd, (HMENU)(UINT_PTR)IDC_BTN_JOIN, g_hmod, nullptr);
        SendMessageA(bj, WM_SETFONT, (WPARAM)s_dlg_fnt_btn, TRUE);

        HWND bc = CreateWindowExA(0, "BUTTON", "Cancel",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            157, 118, 133, 36, hwnd, (HMENU)(UINT_PTR)IDCANCEL, g_hmod, nullptr);
        SendMessageA(bc, WM_SETFONT, (WPARAM)s_dlg_fnt_btn, TRUE);

        SetFocus(ed);
        /* Enter key triggers Join */
        SendMessageA(hwnd, DM_SETDEFID, IDC_BTN_JOIN, 0);
        return 0;
    }

    case WM_ERASEBKGND:
        return TRUE;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT cr; GetClientRect(hwnd, &cr);

        FillRect(hdc, &cr, s_dlg_bg_br);

        /* header background */
        RECT hr = {4, 0, cr.right, 42};
        FillRect(hdc, &hr, s_dlg_hdr_br);

        /* left cyan stripe */
        RECT stripe = {0, 0, 4, cr.bottom};
        HBRUSH stripeBr = CreateSolidBrush(RGB(0, 190, 225));
        FillRect(hdc, &stripe, stripeBr);
        DeleteObject(stripeBr);

        /* title */
        SetBkMode(hdc, TRANSPARENT);
        SelectObject(hdc, s_dlg_fnt_title);
        SetTextColor(hdc, RGB(0, 210, 255));
        RECT tlr = {14, 0, cr.right - 4, 42};
        DrawTextA(hdc, "JOIN WWHD ONLINE", -1, &tlr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        /* separator */
        HPEN sep = CreatePen(PS_SOLID, 1, RGB(0, 70, 100));
        HPEN oldPen = (HPEN)SelectObject(hdc, sep);
        MoveToEx(hdc, 4, 42, nullptr);
        LineTo(hdc, cr.right, 42);
        SelectObject(hdc, oldPen);
        DeleteObject(sep);

        /* label */
        SelectObject(hdc, s_dlg_fnt_label);
        SetTextColor(hdc, RGB(150, 170, 200));
        RECT lr = {14, 52, cr.right - 14, 76};
        DrawTextA(hdc, "Room code from your friend:", -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        /* border ring around edit child (child clips interior) */
        HPEN edgePen = CreatePen(PS_SOLID, 1, RGB(0, 100, 140));
        HPEN oldPen2 = (HPEN)SelectObject(hdc, edgePen);
        HBRUSH oldBr = (HBRUSH)SelectObject(hdc, s_dlg_edit_br);
        Rectangle(hdc, 14, 76, 290, 108);
        SelectObject(hdc, oldPen2);
        SelectObject(hdc, oldBr);
        DeleteObject(edgePen);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wp;
        SetBkColor(hdc, RGB(18, 22, 44));
        SetTextColor(hdc, RGB(255, 220, 50));
        return (LRESULT)s_dlg_edit_br;
    }

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)lp;
        if (!di) return FALSE;
        HDC   hdc     = di->hDC;
        RECT  rc      = di->rcItem;
        bool  isJoin  = (di->CtlID == IDC_BTN_JOIN);
        bool  pressed = (di->itemState & ODS_SELECTED) != 0;
        bool  focused = (di->itemState & ODS_FOCUS)    != 0;

        COLORREF bgCol, fgCol, bdCol;
        if (isJoin) {
            bgCol = pressed ? RGB(0, 110, 145) : RGB(0, 160, 200);
            fgCol = RGB(8, 10, 22);
            bdCol = RGB(0, 200, 240);
        } else {
            bgCol = pressed ? RGB(25, 32, 55) : RGB(35, 45, 70);
            fgCol = RGB(160, 185, 220);
            bdCol = RGB(60, 80, 115);
        }

        HBRUSH bgBr = CreateSolidBrush(bgCol);
        FillRect(hdc, &rc, bgBr);
        DeleteObject(bgBr);

        HPEN bpen = CreatePen(PS_SOLID, 1, bdCol);
        HPEN bpenOld = (HPEN)SelectObject(hdc, bpen);
        HBRUSH noBr  = (HBRUSH)GetStockObject(NULL_BRUSH);
        HBRUSH noOld = (HBRUSH)SelectObject(hdc, noBr);
        Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
        SelectObject(hdc, bpenOld);
        SelectObject(hdc, noOld);
        DeleteObject(bpen);

        if (focused) {
            HPEN fp = CreatePen(PS_DOT, 1, bdCol);
            HPEN fpOld = (HPEN)SelectObject(hdc, fp);
            HBRUSH fOld = (HBRUSH)SelectObject(hdc, noBr);
            Rectangle(hdc, rc.left + 3, rc.top + 3, rc.right - 3, rc.bottom - 3);
            SelectObject(hdc, fpOld);
            SelectObject(hdc, fOld);
            DeleteObject(fp);
        }

        char text[32] = {};
        GetWindowTextA(di->hwndItem, text, sizeof text);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, fgCol);
        SelectObject(hdc, s_dlg_fnt_btn);
        DrawTextA(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return TRUE;
    }

    case WM_NCHITTEST: {
        LRESULT hit = DefWindowProcA(hwnd, msg, wp, lp);
        if (hit == HTCLIENT) {
            POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
            ScreenToClient(hwnd, &pt);
            if (pt.y < 42) return HTCAPTION;
        }
        return hit;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == IDC_BTN_JOIN) {
            char buf[8] = {};
            GetWindowTextA(GetDlgItem(hwnd, IDC_EDIT_CODE), buf, 8);
            for (int i = 0; buf[i]; i++) buf[i] = (char)toupper((unsigned char)buf[i]);
            bool valid = (strlen(buf) == 6);
            for (int i = 0; valid && i < 6; i++)
                if (!isalnum((unsigned char)buf[i])) valid = false;
            if (valid) {
                s_code_ok = true;
                strncpy(s_code_buf, buf, 7); s_code_buf[7] = '\0';
                DestroyWindow(hwnd);
            } else {
                SetWindowTextA(GetDlgItem(hwnd, IDC_EDIT_CODE), "");
                SetFocus(GetDlgItem(hwnd, IDC_EDIT_CODE));
            }
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) { s_code_ok = false; DestroyWindow(hwnd); return 0; }
        return 0;

    case WM_DESTROY:
        if (s_dlg_bg_br)     { DeleteObject(s_dlg_bg_br);     s_dlg_bg_br = nullptr; }
        if (s_dlg_edit_br)   { DeleteObject(s_dlg_edit_br);   s_dlg_edit_br = nullptr; }
        if (s_dlg_hdr_br)    { DeleteObject(s_dlg_hdr_br);    s_dlg_hdr_br = nullptr; }
        if (s_dlg_fnt_title) { DeleteObject(s_dlg_fnt_title); s_dlg_fnt_title = nullptr; }
        if (s_dlg_fnt_label) { DeleteObject(s_dlg_fnt_label); s_dlg_fnt_label = nullptr; }
        if (s_dlg_fnt_btn)   { DeleteObject(s_dlg_fnt_btn);   s_dlg_fnt_btn = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* Shows a topmost input dialog, blocks until the user submits or cancels.
   Returns true and fills `out` on success. */
static bool prompt_join_code(char* out, size_t n) {
    s_code_ok = false; s_code_buf[0] = '\0';

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc); wc.hInstance = g_hmod;
    wc.lpfnWndProc = CodeInputProc; wc.lpszClassName = WC_CODEINPUT;
    wc.hbrBackground = nullptr;  /* painted in WM_PAINT */
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    RegisterClassExA(&wc);  /* may fail if already registered — that's fine */

    /* compute outer window size for a 304×180 client area */
    RECT cr = {0, 0, 304, 180};
    AdjustWindowRectEx(&cr, WS_POPUP | WS_CLIPCHILDREN, FALSE, WS_EX_DLGMODALFRAME);
    int w = cr.right - cr.left;
    int h = cr.bottom - cr.top;

    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_DLGMODALFRAME,
        WC_CODEINPUT, "",
        WS_POPUP | WS_CLIPCHILDREN,
        (sw - w) / 2, (sh - h) / 2, w, h,
        nullptr, nullptr, g_hmod, nullptr);
    if (!hwnd) return false;
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageA(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageA(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
    }

    if (s_code_ok && n > 6) {
        strncpy(out, s_code_buf, 6); out[6] = '\0';
        return true;
    }
    return false;
}

/* ── Chat message input dialog ───────────────────────────────────────────── */
static bool  s_chat_ok  = false;
static char  s_chat_buf[64] = {};
#define WC_CHATINPUT   "WWHDChatInput"
#define IDC_CHAT_EDIT  201
#define IDC_CHAT_SEND  202

static HBRUSH s_chat_bg_br     = nullptr;
static HBRUSH s_chat_edit_br   = nullptr;
static HFONT  s_chat_fnt_title = nullptr;
static HFONT  s_chat_fnt_label = nullptr;
static HFONT  s_chat_fnt_btn   = nullptr;

static LRESULT CALLBACK ChatInputProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        s_chat_bg_br   = CreateSolidBrush(RGB(10, 12, 28));
        s_chat_edit_br = CreateSolidBrush(RGB(18, 22, 44));
        s_chat_fnt_title = CreateFontA(20, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        s_chat_fnt_label = CreateFontA(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        s_chat_fnt_btn = CreateFontA(15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, ANSI_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

        HWND ed = CreateWindowExA(0, "EDIT", "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT | ES_AUTOHSCROLL,
            16, 78, 272, 28, hwnd, (HMENU)(UINT_PTR)IDC_CHAT_EDIT, g_hmod, nullptr);
        SendMessageA(ed, WM_SETFONT, (WPARAM)s_chat_fnt_label, TRUE);
        SendMessageA(ed, EM_SETLIMITTEXT, 61, 0);

        HWND bs = CreateWindowExA(0, "BUTTON", "Send",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            14, 118, 133, 36, hwnd, (HMENU)(UINT_PTR)IDC_CHAT_SEND, g_hmod, nullptr);
        SendMessageA(bs, WM_SETFONT, (WPARAM)s_chat_fnt_btn, TRUE);

        HWND bc = CreateWindowExA(0, "BUTTON", "Cancel",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            157, 118, 133, 36, hwnd, (HMENU)(UINT_PTR)IDCANCEL, g_hmod, nullptr);
        SendMessageA(bc, WM_SETFONT, (WPARAM)s_chat_fnt_btn, TRUE);

        SetFocus(ed);
        SendMessageA(hwnd, DM_SETDEFID, IDC_CHAT_SEND, 0);
        return 0;
    }
    case WM_ERASEBKGND: return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT cr; GetClientRect(hwnd, &cr);
        FillRect(hdc, &cr, s_chat_bg_br);
        RECT hr = {4, 0, cr.right, 42};
        HBRUSH hdrBr = CreateSolidBrush(RGB(8, 10, 22));
        FillRect(hdc, &hr, hdrBr); DeleteObject(hdrBr);
        HBRUSH stripeBr = CreateSolidBrush(RGB(0, 190, 225));
        RECT stripe = {0, 0, 4, cr.bottom};
        FillRect(hdc, &stripe, stripeBr); DeleteObject(stripeBr);
        SetBkMode(hdc, TRANSPARENT);
        SelectObject(hdc, s_chat_fnt_title);
        SetTextColor(hdc, RGB(0, 210, 255));
        RECT tlr = {14, 0, cr.right - 4, 42};
        DrawTextA(hdc, "SEND MESSAGE", -1, &tlr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        HPEN sep = CreatePen(PS_SOLID, 1, RGB(0, 70, 100));
        HPEN oldPen = (HPEN)SelectObject(hdc, sep);
        MoveToEx(hdc, 4, 42, nullptr); LineTo(hdc, cr.right, 42);
        SelectObject(hdc, oldPen); DeleteObject(sep);
        SelectObject(hdc, s_chat_fnt_label);
        SetTextColor(hdc, RGB(150, 170, 200));
        RECT lr = {14, 52, cr.right - 14, 76};
        DrawTextA(hdc, "Chat message:", -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        HPEN edgePen = CreatePen(PS_SOLID, 1, RGB(0, 100, 140));
        HPEN oldPen2 = (HPEN)SelectObject(hdc, edgePen);
        HBRUSH oldBr = (HBRUSH)SelectObject(hdc, s_chat_edit_br);
        Rectangle(hdc, 14, 76, 290, 108);
        SelectObject(hdc, oldPen2); SelectObject(hdc, oldBr);
        DeleteObject(edgePen);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wp;
        SetBkColor(hdc, RGB(18, 22, 44));
        SetTextColor(hdc, RGB(220, 220, 240));
        return (LRESULT)s_chat_edit_br;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)lp;
        if (!di) return FALSE;
        HDC   hdc     = di->hDC;
        RECT  rc      = di->rcItem;
        bool  isSend  = (di->CtlID == IDC_CHAT_SEND);
        bool  pressed = (di->itemState & ODS_SELECTED) != 0;
        bool  focused = (di->itemState & ODS_FOCUS)    != 0;
        COLORREF bgCol = isSend ?
            (pressed ? RGB(0, 110, 145) : RGB(0, 160, 200)) :
            (pressed ? RGB(25, 32, 55)  : RGB(35, 45, 70));
        COLORREF fgCol = isSend ? RGB(8, 10, 22) : RGB(160, 185, 220);
        COLORREF bdCol = isSend ? RGB(0, 200, 240) : RGB(60, 80, 115);
        HBRUSH bgBr = CreateSolidBrush(bgCol);
        FillRect(hdc, &rc, bgBr); DeleteObject(bgBr);
        HPEN bpen = CreatePen(PS_SOLID, 1, bdCol);
        HPEN bpenOld = (HPEN)SelectObject(hdc, bpen);
        HBRUSH noBr  = (HBRUSH)GetStockObject(NULL_BRUSH);
        HBRUSH noOld = (HBRUSH)SelectObject(hdc, noBr);
        Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
        SelectObject(hdc, bpenOld); SelectObject(hdc, noOld); DeleteObject(bpen);
        if (focused) {
            HPEN fp = CreatePen(PS_DOT, 1, bdCol);
            HPEN fpOld = (HPEN)SelectObject(hdc, fp);
            HBRUSH fOld = (HBRUSH)SelectObject(hdc, noBr);
            Rectangle(hdc, rc.left+3, rc.top+3, rc.right-3, rc.bottom-3);
            SelectObject(hdc, fpOld); SelectObject(hdc, fOld); DeleteObject(fp);
        }
        char text[32] = {}; GetWindowTextA(di->hwndItem, text, sizeof text);
        SetBkMode(hdc, TRANSPARENT); SetTextColor(hdc, fgCol);
        SelectObject(hdc, s_chat_fnt_btn);
        DrawTextA(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return TRUE;
    }
    case WM_NCHITTEST: {
        LRESULT hit = DefWindowProcA(hwnd, msg, wp, lp);
        if (hit == HTCLIENT) {
            POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
            ScreenToClient(hwnd, &pt);
            if (pt.y < 42) return HTCAPTION;
        }
        return hit;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_CHAT_SEND) {
            char buf[64] = {};
            GetWindowTextA(GetDlgItem(hwnd, IDC_CHAT_EDIT), buf, 63);
            if (buf[0]) {
                s_chat_ok = true;
                strncpy(s_chat_buf, buf, 63); s_chat_buf[63] = '\0';
                DestroyWindow(hwnd);
            }
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) { s_chat_ok = false; DestroyWindow(hwnd); return 0; }
        return 0;
    case WM_DESTROY:
        if (s_chat_bg_br)     { DeleteObject(s_chat_bg_br);     s_chat_bg_br = nullptr; }
        if (s_chat_edit_br)   { DeleteObject(s_chat_edit_br);   s_chat_edit_br = nullptr; }
        if (s_chat_fnt_title) { DeleteObject(s_chat_fnt_title); s_chat_fnt_title = nullptr; }
        if (s_chat_fnt_label) { DeleteObject(s_chat_fnt_label); s_chat_fnt_label = nullptr; }
        if (s_chat_fnt_btn)   { DeleteObject(s_chat_fnt_btn);   s_chat_fnt_btn = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static bool prompt_chat_msg(char* out, size_t n) {
    s_chat_ok = false; s_chat_buf[0] = '\0';

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc); wc.hInstance = g_hmod;
    wc.lpfnWndProc = ChatInputProc; wc.lpszClassName = WC_CHATINPUT;
    wc.hbrBackground = nullptr;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    RegisterClassExA(&wc);

    RECT cr = {0, 0, 304, 180};
    AdjustWindowRectEx(&cr, WS_POPUP | WS_CLIPCHILDREN, FALSE, WS_EX_DLGMODALFRAME);
    int w = cr.right - cr.left, h = cr.bottom - cr.top;
    int sw = GetSystemMetrics(SM_CXSCREEN), ssh = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_DLGMODALFRAME,
        WC_CHATINPUT, "",
        WS_POPUP | WS_CLIPCHILDREN,
        (sw - w) / 2, (ssh - h) / 2, w, h,
        nullptr, nullptr, g_hmod, nullptr);
    if (!hwnd) return false;
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageA(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageA(hwnd, &m)) {
            TranslateMessage(&m); DispatchMessageA(&m);
        }
    }
    if (s_chat_ok && n > 1) {
        strncpy(out, s_chat_buf, n - 1); out[n - 1] = '\0';
        return true;
    }
    return false;
}

/* ── Start network from resolved args ───────────────────────────────────── */
static void do_net_start(OnlineArgs a) {
    const WWHDModHostV1* h = g_mod.host;
    bool ok = false;
    switch (a.mode) {
    case OnlineArgs::Mode::Host:
        ok = net::host_start(a.port);
        logf("host_start(%u) -> %d", a.port, ok);
        if (ok) h->log(h->context, "WWHD Online: hosting direct session");
        break;
    case OnlineArgs::Mode::Join:
        ok = net::client_connect(a.addr, a.port);
        logf("client_connect -> %d", ok);
        if (ok) h->log(h->context, "WWHD Online: connected (direct)");
        break;
    case OnlineArgs::Mode::P2PCreate:
        ok = net::p2p_create(a.signal_url);
        logf("p2p_create -> %d  lobby_code='%s'", ok, net::lobby_code() ? net::lobby_code() : "(null)");
        if (ok) {
            h->log(h->context, "WWHD Online: P2P lobby created");
            const char* code = net::lobby_code();
            if (code && code[0]) {
                char bar[128], l1[80];
                snprintf(bar, sizeof bar, "WWHD Online  room: %s  (waiting...)  [F9]", code);
                snprintf(l1, 80, "Room code:  %s", code);
                overlay_show(bar, "Mode:    Hosting (P2P)", l1,
                             "Status:  Waiting for players...",
                             "", "Share code with friends");
            }
        }
        break;
    case OnlineArgs::Mode::P2PJoin:
        if (!a.code[0]) {
            char entered[8] = {};
            if (!prompt_join_code(entered, sizeof entered)) {
                overlay_show("WWHD Online  join mode  [F9]",
                             "Mode:    Joining (P2P)", "Room code: (not set)",
                             "Switch to join again to enter a code", "", "");
                logf("p2p_join cancelled by user");
                break;
            }
            strncpy(a.code, entered, 7); a.code[7] = '\0';
            logf("join code from dialog: %s", a.code);
        }
        ok = net::p2p_join(a.signal_url, a.code);
        logf("p2p_join -> %d", ok);
        if (ok) {
            h->log(h->context, "WWHD Online: P2P connected");
            char bar[128], l1[80];
            snprintf(bar, sizeof bar, "WWHD Online  joined room: %s  [F9]", a.code);
            snprintf(l1, 80, "Room code:  %s", a.code);
            overlay_show(bar, "Mode:    Joining (P2P)", l1, "Status:  Connected", "", "");
        } else {
            char bar[128], l1[80];
            snprintf(bar, sizeof bar, "WWHD Online  join FAILED  [F9]");
            snprintf(l1, 80, "Room code:  %s", a.code);
            overlay_show(bar, "Mode:    Joining (P2P)", l1, "Status:  Connection failed", "", "");
        }
        break;
    case OnlineArgs::Mode::RelayCreate:
        ok = net::relay_create(a.addr, a.port);
        logf("relay_create -> %d", ok);
        if (ok) h->log(h->context, "WWHD Online: relay lobby created");
        break;
    case OnlineArgs::Mode::RelayJoin:
        ok = net::relay_join(a.addr, a.port, a.code);
        logf("relay_join -> %d", ok);
        if (ok) h->log(h->context, "WWHD Online: relay connected");
        break;
    case OnlineArgs::Mode::None:
        h->log(h->context, "WWHD Online: mod loaded (inactive — set mode in F1 -> Mods)");
        logf("mode=disabled");
        overlay_show("WWHD Online  inactive  — set mode in F1 Mods  [F9]",
                     "Mode:    None", "", "Select host or join in F1 Mods", "", "");
        break;
    }
    if (a.mode != OnlineArgs::Mode::None && !ok)
        logf("connection FAILED for mode %d", (int)a.mode);
}

/* ── Frame callbacks ─────────────────────────────────────────────────────── */

static void on_frame(void* /*instance*/, uint64_t logic_step) {
    /* Drain ENet and handle session events. */
    net::poll();
    session::tick();

    const WWHDModHostV1* h = g_mod.host;

    /* Update bar + panel once per second. */
    if ((net::is_relay() || net::is_p2p()) && net::is_host() && logic_step % 60 == 0) {
        const char* code = net::lobby_code();
        if (code && code[0]) {
            uint8_t peers = net::peer_count();
            char peer_str[48];
            if (peers > 0)
                snprintf(peer_str, sizeof peer_str, "Players: %u connected", (unsigned)peers);
            else
                strncpy(peer_str, "Status:  Waiting for players...", 48);
            if (peers > 0)
                snprintf(g_mod.status_buf, sizeof g_mod.status_buf,
                         "WWHD Online  room: %s  (%u peer%s)  [F9]",
                         code, (unsigned)peers, peers == 1 ? "" : "s");
            else
                snprintf(g_mod.status_buf, sizeof g_mod.status_buf,
                         "WWHD Online  room: %s  (waiting...)  [F9]", code);
            char l1[80];
            snprintf(l1, 80, "Room code:  %s", code);
            overlay_show(g_mod.status_buf,
                         "Mode:    Hosting (P2P)", l1, peer_str,
                         "", "Share code with friends");
            h->status(h->context, g_mod.status_buf);
        }
    }

    if (!net::is_connected()) return;

    /* Send pending chat message queued by overlay thread. */
    if (InterlockedExchange(&g_pending_chat_flag, 0)) {
        ChatPacket cpkt = {};
        cpkt.type      = MSG_CHAT;
        cpkt.player_id = net::local_player_id();
        strncpy(cpkt.text, g_pending_chat_text, 61); cpkt.text[61] = '\0';
        if (net::is_host())
            net::send_reliable(net::NET_BROADCAST, net::Channel::State, &cpkt, sizeof cpkt);
        else
            net::send_reliable(net::NET_HOST_ID,   net::Channel::State, &cpkt, sizeof cpkt);
    }

    /* Send PlayerInfoPacket at ~1Hz (every 60 logic steps). */
    if (logic_step % 60 == 0) {
        PlayerInfoPacket ipkt = {};
        ipkt.type      = MSG_PLAYER_INFO;
        ipkt.player_id = net::local_player_id();
        read_local_life(&ipkt.life, &ipkt.max_life);
        strncpy(ipkt.name, g_local_name, 15); ipkt.name[15] = '\0';
        if (net::is_host())
            net::send_reliable(net::NET_BROADCAST, net::Channel::State, &ipkt, sizeof ipkt);
        else
            net::send_reliable(net::NET_HOST_ID,   net::Channel::State, &ipkt, sizeof ipkt);
        /* Trigger HUD repaint */
        if (g_bar_hwnd && g_wm_update)
            PostMessageA((HWND)g_bar_hwnd, g_wm_update, 0, 0);
    }

    /* Watch for new chat messages and trigger toast. */
    {
        static int s_last_chat_count = 0;
        int cur = session::chat_count();
        if (cur != s_last_chat_count) {
            s_last_chat_count = cur;
            g_toast_new_msg = true;
            if (g_bar_hwnd && g_wm_update)
                PostMessageA((HWND)g_bar_hwnd, g_wm_update, 0, 0);
        }
    }

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

static void on_config_changed(void* /*instance*/) {
    net::shutdown();
    /* Clear bar immediately so the old room code doesn't linger while
       the new mode starts up. */
    snprintf(g_bar_text, 128, "WWHD Online  switching...");
    if (g_bar_hwnd && g_wm_update)
        PostMessageA((HWND)g_bar_hwnd, g_wm_update, 0, 0);
    OnlineArgs a = build_config();
    logf("on_config_changed: mode=%d signal=%s", (int)a.mode, a.signal_url);
    do_net_start(a);
}

static void overlay_stop() {
    if (g_overlay_tid) {
        PostThreadMessageA(g_overlay_tid, WM_QUIT, 0, 0);
        if (g_overlay_thrd) {
            WaitForSingleObject(g_overlay_thrd, 2000);
            CloseHandle(g_overlay_thrd);
            g_overlay_thrd = nullptr;
        }
        g_overlay_tid = 0;
    }
}

static void on_unload(void* /*instance*/) {
    net::shutdown();
    overlay_stop();
}

/* ── DllMain — capture our HMODULE ──────────────────────────────────────── */
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) g_hmod = hInst;
    return TRUE;
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

WWHD_MOD_EXPORT int wwhd_mod_init_v1(const WWHDModHostV1* host, WWHDModV1* mod) {
    log_open();
    if (!host || !mod) { logf("ERROR: null host/mod"); return 0; }
    if (host->abi_version < WWHD_MOD_ABI_VERSION) {
        logf("ERROR: ABI mismatch (host=%u need=%u)", host->abi_version, WWHD_MOD_ABI_VERSION);
        return 0;
    }

    g_mod.host          = host;
    g_mod.gameinfo_base = decode_gameinfo_base(host);
    logf("gameinfo_base: 0x%08X", g_mod.gameinfo_base);

    if (g_mod.gameinfo_base)
        host->log(host->context, "WWHD Online: gameInfo decoded OK");
    else
        host->log(host->context,
                  "WWHD Online: WARNING — could not decode gameInfo base; "
                  "position sync disabled until addressed.");

    /* Resolve config: manifest options > ini > cmdline, then start network. */
    OnlineArgs a = build_config();
    logf("final mode: %d  signal_url: %s  addr: %s  port: %u  code: %s",
         (int)a.mode, a.signal_url, a.addr, a.port, a.code);
    do_net_start(a);

    /* Fill the mod descriptor. */
    mod->size             = sizeof(WWHDModV1);
    mod->abi_version      = WWHD_MOD_ABI_VERSION;
    mod->instance         = nullptr;
    mod->on_frame         = on_frame;
    mod->on_config_changed = on_config_changed;
    mod->on_unload        = on_unload;

    return 1;  /* always succeed so the game logs our messages */
}
