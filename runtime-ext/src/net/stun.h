/* SPDX-License-Identifier: MPL-2.0
 * stun.h — Minimal STUN Binding Request (RFC 5389).
 *
 * Sends one UDP packet to a free STUN server and reads back the
 * XOR-MAPPED-ADDRESS — our public IP and port as the internet sees us.
 *
 * Usage:
 *   StunResult r = stun_discover("stun.l.google.com", 19302, 7777);
 *   // r.ip = "1.2.3.4", r.port = 7777 (or whatever the NAT mapped it to)
 */
#pragma once
#include <cstdint>

struct StunResult {
    char     ip[64];   /* dotted-decimal public IPv4 */
    uint16_t port;     /* public port as seen externally */
    bool     ok;
};

/*
 * Query a STUN server for our mapped public address.
 *
 *   server      — hostname of a free STUN server
 *                 ("stun.l.google.com", "stun.cloudflare.com", …)
 *   stun_port   — STUN UDP port, almost always 3478 or 19302
 *   local_port  — bind the STUN socket to this local port (0 = any).
 *                 Set to your ENet server port so the NAT maps them together.
 *
 * Returns .ok = false on network error or parse failure.
 * Times out after 2 seconds.
 */
StunResult stun_discover(const char* server, uint16_t stun_port,
                         uint16_t local_port = 0);
