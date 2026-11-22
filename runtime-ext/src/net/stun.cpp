/* SPDX-License-Identifier: MPL-2.0
 * stun.cpp — STUN Binding Request implementation (RFC 5389, IPv4 only).
 *
 * No dependencies beyond the platform socket layer.
 */
#include "stun.h"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   typedef SOCKET sock_t;
   static constexpr sock_t BAD_SOCK = INVALID_SOCKET;
#  define sock_close closesocket
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <errno.h>
   typedef int sock_t;
   static constexpr sock_t BAD_SOCK = -1;
#  define sock_close close
#endif

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>

/* ── STUN wire constants ── */
static constexpr uint32_t MAGIC        = 0x2112A442u;
static constexpr uint16_t BIND_REQ     = 0x0001u;
static constexpr uint16_t BIND_RESP    = 0x0101u;
static constexpr uint16_t ATTR_XOR_MAP = 0x0020u;  /* XOR-MAPPED-ADDRESS */
static constexpr uint16_t ATTR_MAP     = 0x0001u;  /* MAPPED-ADDRESS (fallback) */

#pragma pack(push, 1)
struct StunHdr {
    uint16_t type;
    uint16_t length;
    uint32_t magic;
    uint8_t  txid[12];
};
#pragma pack(pop)

static void set_timeout(sock_t s, int ms) {
#ifdef _WIN32
    DWORD t = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof t);
#else
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
}

StunResult stun_discover(const char* server, uint16_t stun_port,
                         uint16_t local_port) {
    StunResult out = {};

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    /* ── Resolve STUN server ── */
    addrinfo hints = {}, *ai = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    char port_str[8];
    snprintf(port_str, sizeof port_str, "%u", stun_port);
    if (getaddrinfo(server, port_str, &hints, &ai) != 0) return out;

    /* ── Create UDP socket ── */
    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == BAD_SOCK) { freeaddrinfo(ai); return out; }

    /* Allow ENet to bind the same local port afterward. */
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);

    /* Bind to the requested local port. */
    if (local_port != 0) {
        sockaddr_in la = {};
        la.sin_family      = AF_INET;
        la.sin_port        = htons(local_port);
        la.sin_addr.s_addr = INADDR_ANY;
        if (bind(s, (sockaddr*)&la, sizeof la) != 0) {
            sock_close(s); freeaddrinfo(ai); return out;
        }
    }

    /* ── Build STUN Binding Request ── */
    StunHdr req = {};
    req.type   = htons(BIND_REQ);
    req.length = 0;
    req.magic  = htonl(MAGIC);
    srand((unsigned)time(nullptr));
    for (int i = 0; i < 12; i++) req.txid[i] = (uint8_t)(rand() & 0xFF);

    sendto(s, (const char*)&req, sizeof req, 0,
           ai->ai_addr, (int)ai->ai_addrlen);
    freeaddrinfo(ai);

    set_timeout(s, 2000);

    /* ── Receive Binding Response ── */
    uint8_t buf[512];
    int n = (int)recv(s, (char*)buf, sizeof buf, 0);
    sock_close(s);
    if (n < (int)sizeof(StunHdr)) return out;

    const StunHdr* resp = (const StunHdr*)buf;
    if (ntohl(resp->magic) != MAGIC)      return out;
    if (ntohs(resp->type)  != BIND_RESP)  return out;

    /* ── Parse attributes for XOR-MAPPED-ADDRESS ── */
    int pos = (int)sizeof(StunHdr);
    while (pos + 4 <= n) {
        uint16_t atype = ntohs(*(uint16_t*)(buf + pos));
        uint16_t alen  = ntohs(*(uint16_t*)(buf + pos + 2));
        pos += 4;
        int next = pos + ((alen + 3) & ~3);  /* 4-byte aligned */

        if ((atype == ATTR_XOR_MAP || atype == ATTR_MAP) && alen >= 8) {
            /* byte 0: ignored, byte 1: family (1=IPv4), bytes 2-3: port,
               bytes 4-7: address — all in network byte order. */
            uint8_t  family = buf[pos + 1];
            if (family != 0x01) { pos = next; continue; }

            uint16_t raw_port = *(uint16_t*)(buf + pos + 2);
            uint32_t raw_addr = *(uint32_t*)(buf + pos + 4);

            uint16_t port;
            uint32_t addr_net;

            if (atype == ATTR_XOR_MAP) {
                /* XOR-MAPPED-ADDRESS: values XOR'd with magic cookie
                 * in network byte order. */
                port     = ntohs(raw_port ^ htons((uint16_t)(MAGIC >> 16)));
                addr_net = raw_addr ^ htonl(MAGIC);
            } else {
                port     = ntohs(raw_port);
                addr_net = raw_addr;
            }

            inet_ntop(AF_INET, &addr_net, out.ip, sizeof out.ip);
            out.port = port;
            out.ok   = true;
            return out;
        }
        pos = next;
    }

    return out;
}
