/* SPDX-License-Identifier: MPL-2.0
 * p2p.cpp — STUN-based P2P session setup + UDP hole punching.
 *
 * Windows: uses WinHTTP for HTTPS to the Cloudflare Worker (no extra deps).
 * POSIX:   uses libcurl (optional); falls back to a small raw TCP/TLS helper
 *          if curl is not available — but Cloudflare requires HTTPS, so on
 *          Linux builds add -lcurl (or build with WWHD_P2P_USE_CURL=0 to
 *          supply your own http_post / http_get wrappers).
 */
#include "p2p.h"
#include "stun.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>

/* ── Platform socket layer ─────────────────────────────────────────────────── */
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <wincrypt.h>  /* CryptGenRandom for session secret */
#  pragma comment(lib, "ws2_32.lib")
#  pragma comment(lib, "crypt32.lib")
   typedef SOCKET sock_t;
   static constexpr sock_t BAD_SOCK = INVALID_SOCKET;
#  define sock_close closesocket
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
   typedef int sock_t;
   static constexpr sock_t BAD_SOCK = -1;
#  define sock_close close
#endif

/* ── HTTP helpers ──────────────────────────────────────────────────────────── */

#ifdef _WIN32
/* Use WinHTTP for zero-dependency HTTPS on Windows. */
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

/* Minimal WinHTTP wrapper: performs POST or GET and returns response body.
 * Returns empty string on failure. */
static std::string winhttp_request(const char* url_utf8, const char* method,
                                   const char* body /* may be nullptr */)
{
    /* ---- parse URL ---- */
    int wlen = MultiByteToWideChar(CP_UTF8, 0, url_utf8, -1, nullptr, 0);
    if (wlen <= 0) return {};
    std::wstring wurl(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, url_utf8, -1, &wurl[0], wlen);

    URL_COMPONENTS uc = {};
    uc.dwStructSize        = sizeof uc;
    uc.dwSchemeLength      = (DWORD)-1;
    uc.dwHostNameLength    = (DWORD)-1;
    uc.dwUrlPathLength     = (DWORD)-1;
    uc.dwExtraInfoLength   = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return {};

    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath,  uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength)
        path += std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    bool is_https = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    HINTERNET hSession = WinHttpOpen(L"WWHD-Online/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return {};

    HINTERNET hConn = WinHttpConnect(hSession, host.c_str(),
                                     uc.nPort ? uc.nPort
                                              : (is_https ? INTERNET_DEFAULT_HTTPS_PORT
                                                          : INTERNET_DEFAULT_HTTP_PORT),
                                     0);
    if (!hConn) { WinHttpCloseHandle(hSession); return {}; }

    int wmethod_len = MultiByteToWideChar(CP_UTF8, 0, method, -1, nullptr, 0);
    std::wstring wmethod(wmethod_len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, method, -1, &wmethod[0], wmethod_len);

    HINTERNET hReq = WinHttpOpenRequest(
        hConn, wmethod.c_str(), path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        is_https ? WINHTTP_FLAG_SECURE : 0);
    if (!hReq) { WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSession); return {}; }

    LPCWSTR hdr = body
        ? L"Content-Type: application/json\r\n"
        : WINHTTP_NO_ADDITIONAL_HEADERS;
    DWORD   hdrlen = body ? (DWORD)-1 : 0;
    LPCVOID data   = body ? (LPCVOID)body : WINHTTP_NO_REQUEST_DATA;
    DWORD   datalen= body ? (DWORD)strlen(body) : 0;

    if (!WinHttpSendRequest(hReq, hdr, hdrlen, data, datalen, datalen, 0) ||
        !WinHttpReceiveResponse(hReq, nullptr))
    {
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn);
        WinHttpCloseHandle(hSession); return {};
    }

    std::string result;
    DWORD available = 0, downloaded = 0;
    char buf[4096];
    while (WinHttpQueryDataAvailable(hReq, &available) && available > 0) {
        DWORD read = available < sizeof buf ? available : sizeof buf;
        WinHttpReadData(hReq, buf, read, &downloaded);
        result.append(buf, downloaded);
    }
    WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSession);
    return result;
}

static std::string http_post(const char* url, const char* json_body) {
    return winhttp_request(url, "POST", json_body);
}
static std::string http_get(const char* url) {
    return winhttp_request(url, "GET", nullptr);
}
static std::string http_delete(const char* url) {
    return winhttp_request(url, "DELETE", nullptr);
}

#else
/* POSIX: use libcurl (link with -lcurl).
 * If you don't have curl, replace these three functions with your own HTTPS impl. */
#include <curl/curl.h>
#include <string>

static size_t curl_write_cb(char* ptr, size_t, size_t nmemb, void* userdata) {
    ((std::string*)userdata)->append(ptr, nmemb);
    return nmemb;
}

static std::string curl_do(const char* url, const char* method,
                            const char* body)
{
    CURL* c = curl_easy_init();
    if (!c) return {};
    std::string out;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
    struct curl_slist* hdrs = nullptr;
    if (body) {
        hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    }
    if (strcmp(method, "DELETE") == 0)
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "DELETE");
    CURLcode rc = curl_easy_perform(c);
    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    return rc == CURLE_OK ? out : std::string{};
}

static std::string http_post(const char* url, const char* json_body) {
    return curl_do(url, "POST", json_body);
}
static std::string http_get(const char* url) {
    return curl_do(url, "GET", nullptr);
}
static std::string http_delete(const char* url) {
    return curl_do(url, "DELETE", nullptr);
}

#endif /* _WIN32 */

#include <string>  /* shared by both paths */

/* ── Room code generation ──────────────────────────────────────────────────── */
/* Same charset as the relay server: excludes 0, 1, I, O to avoid confusion. */
static const char CODE_CHARS[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
static constexpr int CODE_CHARS_LEN = 32;

static void gen_code(char out[7]) {
    srand((unsigned)time(nullptr) ^ (unsigned)(uintptr_t)out);
    for (int i = 0; i < 6; i++)
        out[i] = CODE_CHARS[rand() % CODE_CHARS_LEN];
    out[6] = '\0';
}

/* ── Session secret generation ─────────────────────────────────────────────
 * Generates 32 bytes of cryptographic randomness, encodes as 64 lowercase hex
 * chars, NUL-terminates into out[65].  Used to authenticate joiners so only
 * someone who retrieved the room entry from the signaling server can connect.
 */
static bool gen_secret(char out[65]) {
    uint8_t bytes[32] = {};
#ifdef _WIN32
    HCRYPTPROV prov = 0;
    if (!CryptAcquireContextW(&prov, nullptr, nullptr,
                               PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
        return false;
    BOOL ok = CryptGenRandom(prov, 32, bytes);
    CryptReleaseContext(prov, 0);
    if (!ok) return false;
#else
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f) return false;
    bool ok = (fread(bytes, 1, 32, f) == 32);
    fclose(f);
    if (!ok) return false;
#endif
    static const char HEX[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i*2]   = HEX[bytes[i] >> 4];
        out[i*2+1] = HEX[bytes[i] & 0xF];
    }
    out[64] = '\0';
    return true;
}

/* ── JSON helpers (no external parser needed) ──────────────────────────────── */

/* Extract the string value of "key" from a flat JSON object.
 * Writes at most buf_len-1 chars into buf.  Returns false if not found. */
static bool json_str(const char* json, const char* key, char* buf, int buf_len) {
    if (!json) return false;
    char search[128];
    snprintf(search, sizeof search, "\"%s\"", key);
    const char* p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return false;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < buf_len - 1) buf[i++] = *p++;
    buf[i] = '\0';
    return i > 0;
}

/* Extract an integer value. */
static bool json_int(const char* json, const char* key, int* out) {
    if (!json) return false;
    char search[128];
    snprintf(search, sizeof search, "\"%s\"", key);
    const char* p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ' || *p == ':') p++;
    if (*p < '0' || *p > '9') return false;
    *out = atoi(p);
    return true;
}

/* ── p2p_create ────────────────────────────────────────────────────────────── */

P2PCreateResult p2p_create(const char* signaling_url, uint16_t local_port) {
    P2PCreateResult result = {};

#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    /* 1. Discover our public address via STUN. */
    StunResult sr = stun_discover("stun.l.google.com", 19302, local_port);
    if (!sr.ok) {
        fprintf(stderr, "[p2p] STUN failed — check network\n");
        return result;
    }
    fprintf(stderr, "[p2p] public address: %s:%u\n", sr.ip, sr.port);

    /* 2. Generate a room code and 32-byte session secret. */
    gen_code(result.code);
    if (!gen_secret(result.secret)) {
        fprintf(stderr, "[p2p] secret generation failed\n");
        return result;
    }

    /* 3. POST to signaling Worker:
     *    { "code": "XXXXXX", "ip": "1.2.3.4", "port": 7777, "secret": "hex64" }
     * The Worker stores the secret; joiners must present it to prove they looked
     * up the room through the signaling server (not just guessing the code). */
    char body[384];
    snprintf(body, sizeof body,
             "{\"code\":\"%s\",\"ip\":\"%s\",\"port\":%u,\"secret\":\"%s\"}",
             result.code, sr.ip, (unsigned)sr.port, result.secret);

    char url[512];
    snprintf(url, sizeof url, "%s/rooms", signaling_url);

    std::string resp = http_post(url, body);
    if (resp.empty()) {
        fprintf(stderr, "[p2p] signaling POST failed\n");
        result.code[0] = '\0';
        return result;
    }

    result.local_port = local_port ? local_port : sr.port;
    result.ok = true;
    fprintf(stderr, "[p2p] room created: %s\n", result.code);
    return result;
}

/* ── p2p_join ──────────────────────────────────────────────────────────────── */

P2PJoinResult p2p_join(const char* signaling_url, const char* code,
                       uint16_t local_port) {
    P2PJoinResult result = {};

    /* Validate code length. */
    if (!code || strnlen(code, 8) != 6) {
        fprintf(stderr, "[p2p] invalid room code\n");
        return result;
    }

    /* GET signaling Worker: /rooms/XXXXXX */
    char url[512];
    snprintf(url, sizeof url, "%s/rooms/%s", signaling_url, code);

    std::string resp = http_get(url);
    if (resp.empty()) {
        fprintf(stderr, "[p2p] signaling GET failed\n");
        return result;
    }

    /* Parse: { "ip": "1.2.3.4", "port": 7777, "secret": "hex64" } */
    if (!json_str(resp.c_str(), "ip", result.host_ip, sizeof result.host_ip)) {
        fprintf(stderr, "[p2p] room not found or invalid response: %s\n", resp.c_str());
        return result;
    }
    int port_int = 0;
    if (!json_int(resp.c_str(), "port", &port_int)) {
        fprintf(stderr, "[p2p] missing port in response\n");
        return result;
    }
    if (!json_str(resp.c_str(), "secret", result.secret, sizeof result.secret)) {
        /* Older Worker without secret support — treat as insecure but allow. */
        fprintf(stderr, "[p2p] WARNING: signaling server returned no session secret\n");
        result.secret[0] = '\0';
    }
    result.host_port  = (uint16_t)port_int;
    result.local_port = local_port;
    result.ok         = true;

    fprintf(stderr, "[p2p] host is %s:%u\n", result.host_ip, result.host_port);
    return result;
}

/* ── p2p_punch ─────────────────────────────────────────────────────────────── */

void p2p_punch(const char* ip, uint16_t port, uint16_t local_port, int count) {
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == BAD_SOCK) return;

    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);

    if (local_port != 0) {
        sockaddr_in la = {};
        la.sin_family      = AF_INET;
        la.sin_port        = htons(local_port);
        la.sin_addr.s_addr = INADDR_ANY;
        if (bind(s, (sockaddr*)&la, sizeof la) != 0) {
            sock_close(s); return;
        }
    }

    sockaddr_in dst = {};
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(port);
    inet_pton(AF_INET, ip, &dst.sin_addr);

    /* Send small punch packets — the payload is intentionally invalid ENet data
     * so ENet on the other side will discard it, but the NAT will open the hole. */
    static const uint8_t punch_pkt[] = { 0xFF, 'W', 'W', 'H', 'D' };
    for (int i = 0; i < count; i++) {
        sendto(s, (const char*)punch_pkt, sizeof punch_pkt, 0,
               (sockaddr*)&dst, sizeof dst);
#ifdef _WIN32
        Sleep(20);
#else
        struct timeval tv = {0, 20000}; select(0, nullptr, nullptr, nullptr, &tv);
#endif
    }
    sock_close(s);
    fprintf(stderr, "[p2p] punched %s:%u (%d packets)\n", ip, port, count);
}

/* ── p2p_delete_room ───────────────────────────────────────────────────────── */

void p2p_delete_room(const char* signaling_url, const char* code) {
    if (!code || !code[0]) return;
    char url[512];
    snprintf(url, sizeof url, "%s/rooms/%s", signaling_url, code);
    http_delete(url);  /* fire-and-forget */
}
