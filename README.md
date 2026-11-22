# WWHD Online

> True online multiplayer for *The Legend of Zelda: The Wind Waker HD*, built on the [ZeldaWWHDRecomp](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp) native PC port.

![Status](https://img.shields.io/badge/status-early%20development-f0a830?style=flat-square)
![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20macOS%20%7C%20Linux-3dccb0?style=flat-square)
![License](https://img.shields.io/badge/license-MPL--2.0-7fafc8?style=flat-square)

**WWHD Online is an unofficial fan project. It is not affiliated with or endorsed by Nintendo.**  
Requires a legally-obtained copy of The Legend of Zelda: The Wind Waker HD (Wii U, USA or EUR).

---

## Features

- **Real-time player sync** — world position, rotation, and animation state at 20 Hz with interpolated rendering
- **Full enemy sync** — position, rotation, HP, every animation, and attack state replicated exactly across all clients
- **All players are targets** — enemy targeting and hitboxes extended to every connected player; any player can damage and be damaged
- **Shared world state** — chest flags, door states, switches, and enemy kills synchronized; area transitions gated to prevent divergence
- **Mod SDK v2 native** — implemented as a guest mod using the official hook system (`daPy_Execute`, `fpcM_Execute`)
- **ENet UDP transport** — position data unreliable at 20 Hz; critical state changes (HP, deaths) sent reliably

---

## Architecture

Two layers: a guest mod (PowerPC C) reads and writes game state; a native host layer (C++) handles network transport.

```
┌─────────────────────────────────────────────────────────┐
│                 Game Instance (each player)              │
├─────────────────────────────────────────────────────────┤
│  GUEST LAYER  (PowerPC C mod, Mod SDK v2)                │
│  daPy_Execute hook  ─► read Link pos/rot/anim each step  │
│  fpcM_Execute hook  ─► iterate actors, read enemy state  │
│  wwhd_net_send/recv ─► bridge to host network layer      │
├─────────────────────────────────────────────────────────┤
│  HOST LAYER   (C++, runtime/src/net/)                    │
│  ENet UDP socket    ─► reliable + unreliable channels    │
│  Delta encoder      ─► skip position if change < 1 unit  │
│  Session manager    ─► host/client roles, player registry│
└─────────────────────────────────────────────────────────┘
                            │  UDP
            ┌───────────────┼──────────────┐
       Instance A       Instance B     Instance C
```

### Enemy sync (Phase 4)

The host owns enemy AI and broadcasts full state each tick. Clients receive position/rotation/HP/animation deltas and write them into actor memory. Enemy targeting is extended by hooking the perception-radius check and injecting all remote player positions alongside Link's.

```c
/* Per-enemy packet, 20 bytes, sent at 20 Hz unreliable / HP changes reliable */
typedef struct {
    u32  actor_id;    /* fopAc_ac_c process handle            */
    s16  x, y, z;    /* position * 4, range ±8192 m          */
    u16  yaw;         /* y-rotation                           */
    u8   hp;          /* current health                       */
    u8   phase;       /* WWHD_COMBAT_ALIVE / FLINCH / DEAD    */
    u8   anim_id;     /* index into enemy's animation table   */
    u8   attack_flag; /* attack_active_frames != 0            */
    u8   flags;
    u8   pad[3];
} EnemyPacket; /* sizeof == 20 */
```

---

## Build setup (Windows)

### 1 — Prerequisites

Clang is required. MSVC cannot build the recompiled game code due to `musttail`.  
Also install the [Vulkan SDK](https://vulkan.lunarg.com/sdk/home) from LunarG.

```powershell
winget install LLVM.LLVM
winget install Kitware.CMake
winget install Ninja-build.Ninja
pip install pycryptodome
```

### 2 — Clone with submodules

```powershell
git clone --recurse-submodules https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp.git
cd ZeldaWWHDRecomp
```

### 3 — Extract and recompile

Provide your own `.wux` or `.wud` Wii U disc image. This runs once (~2 minutes).

```powershell
python tools/wudextract.py game.wux extract game
python tools/recomp/recomp.py game/code/cking.rpx build/gen
```

### 4 — Build the runtime

```powershell
cmake -S . -B build/win -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=clang `
  -DCMAKE_CXX_COMPILER=clang++
cmake --build build/win
.\build\win\wwhd.exe
```

### 5 — Build and install the mod

Requires Clang with a PowerPC cross-compilation target and `ld.lld`.

```powershell
cd examples/guest-mods
make CLANG=clang LLD=ld.lld
# In-game: Mods → Installed packages → Choose folder → enable → restart
```

---

## Project status

| Phase | Description | Status |
|-------|-------------|--------|
| 1 | Link state reader (position hook) | ✅ Done |
| 2 | Host-side ENet transport layer | 🔨 In progress |
| 3 | Remote player rendering (phantom actor) | 📋 Planned |
| 4 | Full enemy sync + player targeting | 📋 Planned |
| 5 | World state sync (flags, items) | 📋 Planned |

---

## Credits

This project stands entirely on the work of the following teams and contributors:

| Project | Role |
|---------|------|
| [ZeldaWWHDRecomp](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp) | Native PC port — the entire foundation |
| [ZeldaWWHDDecomp](https://github.com/ZeldaWWHDDecomp/wwhd) | Game decompilation — all actor struct knowledge |
| [ZeldaWWHDMods](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDMods) | Mod SDK v2 and examples (sdk-v3-enemy directly informed the enemy sync design) |
| [Cemu Project](https://cemu.info) | Latte shader decompiler vendored in the WWHD runtime |
| [ENet](http://enet.bespin.org) | Reliable UDP networking library |
| Nintendo | Original game (*The Legend of Zelda: The Wind Waker HD*, 2013) |

**Discord** (ZeldaWWHDRecomp community): https://discord.gg/dhFYzCJUAm

---

*WWHD Online is an unofficial fan project. Not affiliated with or endorsed by Nintendo.*
