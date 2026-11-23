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
static volatile bool  g_panel_vis    = false;
static HANDLE         g_overlay_thrd = nullptr;
static DWORD          g_overlay_tid  = 0;
static char          g_bar_text[128]     = {};
static char          g_panel_line[5][80] = {};
static UINT          g_wm_update         = 0;
static const int     HOTKEY_ID_PANEL     = 1;
static const int     HOTKEY_ID_COPY      = 2;
static const UINT_PTR TIMER_ID_FLASH     = 1;
static char          g_bar_saved[128]    = {};

/* Bar: single-line status strip ─────────────────────────────────────────── */
static LRESULT CALLBACK BarWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    (void)lp;
    if (g_wm_update && msg == g_wm_update) {
        InvalidateRect(hwnd, nullptr, TRUE);
        if (g_panel_hwnd && g_panel_vis)
            InvalidateRect((HWND)g_panel_hwnd, nullptr, TRUE);
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
        DrawTextA(hdc, "[F8] copy code  [F9] close", -1, &fr, DT_LEFT | DT_TOP);

        DeleteObject(ftitle); DeleteObject(fbody);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_DESTROY) { return 0; }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static DWORD WINAPI overlay_thread_fn(LPVOID) {
    g_wm_update = RegisterWindowMessageA("WWHD_ONLINE_OVERLAY_UPDATE");

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc); wc.hInstance = g_hmod;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);

    wc.lpfnWndProc = BarWndProc; wc.lpszClassName = "WWHDOnlineBar";
    RegisterClassExA(&wc);
    wc.lpfnWndProc = PanelWndProc; wc.lpszClassName = "WWHDOnlinePanel";
    RegisterClassExA(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);

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

    /* F9 hotkey for panel toggle — received as thread message (hwnd=NULL) */
    RegisterHotKey(nullptr, HOTKEY_ID_PANEL, 0, VK_F9);
    RegisterHotKey(nullptr, HOTKEY_ID_COPY,  0, VK_F8);

    MSG m;
    while (GetMessageA(&m, nullptr, 0, 0) > 0) {
        if (m.message == WM_HOTKEY && (int)m.wParam == HOTKEY_ID_PANEL) {
            g_panel_vis = !g_panel_vis;
            if (g_panel_hwnd)
                ShowWindow((HWND)g_panel_hwnd,
                           g_panel_vis ? SW_SHOWNOACTIVATE : SW_HIDE);
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
    g_bar_hwnd = nullptr; g_panel_hwnd = nullptr;
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

/* ── Start network from resolved args ───────────────────────────────────── */
static void do_net_start(const OnlineArgs& a) {
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
            overlay_show("WWHD Online  join mode  [F9]",
                         "Mode:    Joining (P2P)", "Room code: (not set)",
                         "Add code=XXXXXX to wwhd_online.ini", "", "");
            h->log(h->context, "WWHD Online: join mode — set code= in wwhd_online.ini");
            logf("p2p_join skipped: no code");
            break;
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
