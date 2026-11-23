# WWHD Online

> True online multiplayer for *The Legend of Zelda: The Wind Waker HD*, built as a native mod for the [ZeldaWWHDRecomp](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp) PC port.

![Status](https://img.shields.io/badge/status-early%20development-f0a830?style=flat-square)
![Platform](https://img.shields.io/badge/platform-Windows-3dccb0?style=flat-square)
![License](https://img.shields.io/badge/license-MPL--2.0-7fafc8?style=flat-square)

**Unofficial fan project. Not affiliated with or endorsed by Nintendo.**  
Requires a legally-obtained copy of The Legend of Zelda: The Wind Waker HD (Wii U, USA or EUR).

---

## Features

- **P2P multiplayer** — direct peer-to-peer connections via UDP hole punching; no dedicated server required
- **Relay support** — optional relay server for players behind strict NAT
- **In-game overlay** — GDI bar shows room code and status at all times
- **F1 Mods integration** — select Host or Join mode directly from the game's built-in Mods menu
- **F8 clipboard copy** — instantly copy your lobby code to share with friends
- **F9 info panel** — toggle a detailed overlay showing room code, mode, and peer count
- **Real-time position sync** — player position and rotation replicated at 20 Hz

---

## Quick start

### 1 — Install the mod

1. Download or build `wwhd_online.dll` and `manifest.json`
2. Place both files in your Mod Manager mods folder:
   ```
   data\user\ModManager\Mods\wwhd-online\
   ```
3. Launch the game, press **F1**, go to the **Mods** tab, and enable **WWHD Online**

### 2 — Host a session

1. In F1 → Mods → WWHD Online, set **Mode** to `host`
2. Click **Apply** — the mod creates a P2P lobby and shows your room code in the top-center bar
3. Press **F8** to copy the room code to your clipboard, then share it with your friend

### 3 — Join a session

1. Set **Mode** to `join` in the F1 → Mods UI
2. Edit `wwhd_online.ini` (in the same folder as the DLL) to add:
   ```ini
   code=XXXXXX
   ```
3. Click **Apply** — the mod connects to the host's lobby

> **Tip:** You can also configure everything via `wwhd_online.ini` without touching the F1 menu. See [Configuration](#configuration).

---

## Overlay controls

| Key | Action |
|-----|--------|
| **F8** | Copy lobby code to clipboard (flashes confirmation for 2 seconds) |
| **F9** | Toggle the info panel (room code, mode, peer count) |

---

## Configuration

Create `wwhd_online.ini` next to `wwhd_online.dll`. F1 Mods settings override the ini file.

```ini
# Mode: host, join, relay-host, relay-join, direct-host, direct-join
mode=host

# P2P signaling server (default: Cloudflare Worker)
signal_url=https://wwhd-signal.wwhdmm.workers.dev

# Join: 6-character room code
code=XXXXXX

# Relay / direct: server address and port
addr=127.0.0.1
port=7777
```

---

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                   Game Process (each player)                 │
├─────────────────────────────────────────────────────────────┤
│  WWHD Online native mod  (wwhd_online.dll)                   │
│                                                              │
│  wwhd_mod_init_v1  ── reads config (F1 > ini > cmdline)      │
│  on_frame          ── reads Link pos/rot/anim, sends packets │
│  on_config_changed ── live reconnect when F1 Mods changes    │
│  on_unload         ── clean shutdown (overlay + network)     │
│                                                              │
│  GDI overlay thread ── top bar + F9 panel + F8/F9 hotkeys    │
│  ENet transport     ── reliable + unreliable UDP channels    │
└─────────────────────────────────────────────────────────────┘
                              │  UDP
              ┌───────────────┼───────────────┐
         Player A         Player B        Player C

P2P mode:  Cloudflare Worker signaling → STUN → UDP hole punch
Relay mode: wwhd-relay server (see relay/)
```

---

## Building (Windows)

Requires **Visual Studio 2022** (MSVC + Windows SDK).

```powershell
.\build_native_mod.ps1
```

Output: `wwhd_online.dll` in the project root. Copy it and `manifest.json` to your mod folder.

The build script links against: `enet`, `ws2_32`, `winmm`, `advapi32`, `crypt32`, `winhttp`, `user32`, `gdi32`.

---

## Signaling server

The default signaling server is a Cloudflare Worker in `signaling/`. It brokers the initial STUN exchange so peers can punch through NAT — no traffic is relayed through it after the connection is established.

To self-host:
```bash
cd signaling
npm install
npx wrangler deploy
```

Update `signal_url` in your `wwhd_online.ini` to point to your worker.

---

## Project status

| Component | Status |
|-----------|--------|
| Native mod ABI v1 integration | ✅ Done |
| P2P networking (ENet + hole punching) | ✅ Done |
| Relay networking | ✅ Done |
| F1 Mods UI integration | ✅ Done |
| GDI overlay + F8/F9 hotkeys | ✅ Done |
| Player position/rotation sync | ✅ Done |
| Remote player rendering | 📋 Planned |
| Enemy sync | 📋 Planned |
| World state sync (flags, chests) | 📋 Planned |

---

## Credits

| Project | Role |
|---------|------|
| [ZeldaWWHDRecomp](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp) | Native PC port and mod ABI |
| [ZeldaWWHDDecomp](https://github.com/ZeldaWWHDDecomp/wwhd) | Game decompilation — actor struct offsets |
| [ENet](http://enet.bespin.org) | Reliable UDP networking library |
| Nintendo | Original game (*The Legend of Zelda: The Wind Waker HD*, 2013) |

**Discord** (ZeldaWWHDRecomp community): https://discord.gg/dhFYzCJUAm

---

*WWHD Online is an unofficial fan project. Not affiliated with or endorsed by Nintendo.*
