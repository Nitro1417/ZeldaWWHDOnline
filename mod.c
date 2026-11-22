/* SPDX-License-Identifier: MPL-2.0
 * mod.c — WWHD Online guest mod.
 *
 * This guest mod (compiled from PPC C) is OPTIONAL for Phase 1.
 * All networking lives in the native mod (wwhd_online.dll via net_mod.cpp).
 *
 * This mod is the hook point for Phase 3 (ghost actor rendering):
 *   - Register ghost actors via wwhd_actor_register
 *   - Drive ghost positions from data the native mod writes to PPC memory
 *   - Hook enemy execute to sync enemy state
 *
 * For Phase 1–2 (position sync over network), the native mod reads Link's
 * PPC memory directly via WWHDModHostV1::read_guest. No guest hooks needed.
 *
 * Build with the WWHD Mod SDK v2 (guestmod.py):
 *   python guestmod.py mod.c -o wwhd_online_guest.elf
 *
 * Note: wwhd/net.h is NOT included here. The wwhd_net_* host services are
 * not registered by the runtime — all network calls go through the native mod.
 */

#include "wwhd_guest.h"
#include "wwhd/actor.h"
#include "wwhd/data.h"

/* ── Phase 3 placeholder: ghost actor rendering ───────────────────────────
 *
 * When the native mod is ready to drive ghost actors, add:
 *   WWHD_HOOK(WWHD_ADDR_fpcM_Execute, online_ghost_tick, (void* unused))
 *   {
 *       (void)unused;
 *       // For each connected remote player, update the ghost actor's
 *       // position from the shared PPC buffer the native mod writes.
 *   }
 *
 * The native mod writes received player positions to a PPC buffer whose
 * address is stored at a known location; the ghost actor reads that buffer.
 * No wwhd_net_* host services are required.
 */

/* ── Phase 4 placeholder: enemy sync ─────────────────────────────────────
 *
 * Hook fpcM_Execute to iterate the actor list, identify enemies, and collect
 * their state for the native mod to broadcast.  The native mod writes
 * received enemy updates back to PPC memory via write_guest.
 */
