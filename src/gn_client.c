/* gn_client.c - non-blocking WebSocket (RFC 6455) client for gn
 *
 * Everything happens in gn_client_poll(): connect progress, the HTTP upgrade,
 * frame parsing, ping/pong, and the close handshake. Sockets are always
 * non-blocking; the only blocking call is name resolution in connect.
 *
 * Avoids C99 library functions missing from older MSVC runtimes (snprintf,
 * strcasecmp) so it builds against the VC9 CRT. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200112L
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE /* SO_NOSIGPIPE */
#endif

#include "gn.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0501
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0501 /* getaddrinfo */
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET gn_sock;
#define GN_BAD_SOCK INVALID_SOCKET
#define gn_closesocket closesocket
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int gn_sock;
#define GN_BAD_SOCK (-1)
#define gn_closesocket close
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* Largest WebSocket message accepted (a message may hold many packets). */
#define GN_MAX_MESSAGE (16u * 1024u * 1024u)
/* Largest unsent backlog before gn_client_send() refuses. */
#define GN_MAX_BACKLOG (16u * 1024u * 1024u)
#define GN_MAX_HANDSHAKE 8192u
#define GN_CLOSE_TIMEOUT_MS 2000u
#define GN_CONNECT_TIMEOUT_MS 10000u

enum { PHASE_TCP, PHASE_HANDSHAKE, PHASE_WS };

typedef struct gn_bytes {
    uint8_t *data;
    size_t len;
    size_t cap;
} gn_bytes;

struct gn_client {
    gn_client_callbacks cb;
    gn_client_state state;
    int phase;
    gn_sock fd;
    struct addrinfo *addrs; /* remaining candidates for the TCP connect */
    struct addrinfo *addr;
    char *request;          /* HTTP upgrade request, sent once TCP connects */
    char accept[32];        /* expected Sec-WebSocket-Accept */
    gn_bytes out;           /* bytes waiting to be sent */
    size_t out_pos;
    gn_bytes in;            /* bytes received, not yet parsed */
    gn_bytes msg;           /* fragmented message being assembled */
    int msg_opcode;         /* 0 when not inside a fragmented message */
    uint64_t rng;
    uint32_t deadline;      /* connect or close timeout (ms clock) */
    int close_sent;
    int close_code;
    int failed;             /* fatal error noticed outside poll */
    int failed_err;
#ifdef _WIN32
    int wsa;
#endif
};

/* ====================================================================== */
/* Platform                                                               */
/* ====================================================================== */

static uint32_t now_ms(void)
{
#ifdef _WIN32
    return (uint32_t)GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
#endif
}

/* deadline reached? (wrap-safe) */
static int past(uint32_t deadline)
{
    return (int32_t)(now_ms() - deadline) >= 0;
}

static int would_block(void)
{
#ifdef _WIN32
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS || errno == EINTR;
#endif
}

static int set_nonblocking(gn_sock fd)
{
#ifdef _WIN32
    u_long on = 1;
    return ioctlsocket(fd, FIONBIO, &on) == 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

static void tune_socket(gn_sock fd)
{
    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof on);
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&on, sizeof on);
#endif
}

/* 1 = connected, 0 = still connecting, -1 = failed */
static int connect_status(gn_sock fd)
{
    fd_set wr, ex;
    struct timeval tv;
    int err = 0;
#ifdef _WIN32
    int len = sizeof err;
#else
    socklen_t len = sizeof err;
#endif
    FD_ZERO(&wr);
    FD_ZERO(&ex);
    FD_SET(fd, &wr);
    FD_SET(fd, &ex); /* Windows reports a failed connect here */
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    if (select((int)fd + 1, NULL, &wr, &ex, &tv) < 0) {
        return -1;
    }
    if (FD_ISSET(fd, &ex)) {
        return -1;
    }
    if (!FD_ISSET(fd, &wr)) {
        return 0;
    }
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &len) != 0 || err != 0) {
        return -1;
    }
    return 1;
}

/* ====================================================================== */
/* Small helpers                                                          */
/* ====================================================================== */

static int bytes_reserve(gn_bytes *b, size_t extra, size_t limit)
{
    size_t need = b->len + extra, cap;
    uint8_t *p;
    if (need <= b->cap) {
        return 1;
    }
    if (need > limit) {
        return 0;
    }
    cap = b->cap ? b->cap : 4096;
    while (cap < need) {
        cap *= 2;
    }
    if (cap > limit) {
        cap = limit;
    }
    p = (uint8_t *)realloc(b->data, cap);
    if (!p) {
        return 0;
    }
    b->data = p;
    b->cap = cap;
    return 1;
}

static void bytes_consume(gn_bytes *b, size_t n)
{
    if (n >= b->len) {
        b->len = 0;
        return;
    }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
}

static void bytes_free(gn_bytes *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static uint64_t next_random(gn_client *c)
{
    /* splitmix64: masking keys and the handshake nonce only need to be
     * unpredictable to intermediaries, not cryptographically strong. Never
     * touches the C library's rand(), so game RNG state is unaffected. */
    uint64_t z = (c->rng += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static int ascii_lower(int ch)
{
    return (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch;
}

/* case-insensitive: does s[0..len) contain needle? */
static int contains_ci(const char *s, size_t len, const char *needle)
{
    size_t n = strlen(needle), i, j;
    for (i = 0; i + n <= len; i++) {
        for (j = 0; j < n && ascii_lower((unsigned char)s[i + j]) == ascii_lower((unsigned char)needle[j]); j++) {
        }
        if (j == n) {
            return 1;
        }
    }
    return 0;
}

static void append_str(char **p, const char *s)
{
    size_t n = strlen(s);
    memcpy(*p, s, n);
    *p += n;
}

static void append_uint(char **p, unsigned v)
{
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) {
        *(*p)++ = tmp[--n];
    }
}

/* ---------------------------------------------------------------------- */
/* SHA-1 and base64, for Sec-WebSocket-Accept (RFC 6455 section 4.2.2)     */

static uint32_t rol(uint32_t v, int n)
{
    return (v << n) | (v >> (32 - n));
}

static void sha1(const uint8_t *data, size_t len, uint8_t out[20])
{
    uint32_t h[5] = { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u };
    uint8_t block[64];
    uint64_t bits = (uint64_t)len * 8;
    size_t total = ((len + 8) / 64 + 1) * 64, off, i;
    for (off = 0; off < total; off += 64) {
        uint32_t w[80], a, b, cc, d, e;
        for (i = 0; i < 64; i++) {
            size_t k = off + i;
            if (k < len) block[i] = data[k];
            else if (k == len) block[i] = 0x80;
            else if (k >= total - 8) block[i] = (uint8_t)(bits >> (8 * (total - 1 - k)));
            else block[i] = 0;
        }
        for (i = 0; i < 16; i++) {
            w[i] = ((uint32_t)block[4 * i] << 24) | ((uint32_t)block[4 * i + 1] << 16) |
                   ((uint32_t)block[4 * i + 2] << 8) | block[4 * i + 3];
        }
        for (i = 16; i < 80; i++) {
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        a = h[0]; b = h[1]; cc = h[2]; d = h[3]; e = h[4];
        for (i = 0; i < 80; i++) {
            uint32_t f, k, t;
            if (i < 20) { f = (b & cc) | (~b & d); k = 0x5a827999u; }
            else if (i < 40) { f = b ^ cc ^ d; k = 0x6ed9eba1u; }
            else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8f1bbcdcu; }
            else { f = b ^ cc ^ d; k = 0xca62c1d6u; }
            t = rol(a, 5) + f + e + k + w[i];
            e = d; d = cc; cc = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += cc; h[3] += d; h[4] += e;
    }
    for (i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8);
        out[4 * i + 3] = (uint8_t)h[i];
    }
}

static void base64(const uint8_t *in, size_t len, char *out)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i;
    for (i = 0; i + 2 < len; i += 3) {
        *out++ = tbl[in[i] >> 2];
        *out++ = tbl[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
        *out++ = tbl[((in[i + 1] & 15) << 2) | (in[i + 2] >> 6)];
        *out++ = tbl[in[i + 2] & 63];
    }
    if (i < len) {
        *out++ = tbl[in[i] >> 2];
        if (i + 1 < len) {
            *out++ = tbl[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
            *out++ = tbl[(in[i + 1] & 15) << 2];
        } else {
            *out++ = tbl[(in[i] & 3) << 4];
            *out++ = '=';
        }
        *out++ = '=';
    }
    *out = 0;
}

/* ====================================================================== */
/* Lifecycle                                                              */
/* ====================================================================== */

gn_client *gn_client_create(const gn_client_callbacks *callbacks)
{
    gn_client *c = (gn_client *)calloc(1, sizeof *c);
    if (!c) {
        return NULL;
    }
    if (callbacks) {
        c->cb = *callbacks;
    }
    c->fd = GN_BAD_SOCK;
    c->state = GN_DISCONNECTED;
    c->rng = (uint64_t)time(NULL) ^ ((uint64_t)now_ms() << 32) ^ (uint64_t)(size_t)c ^ (uint64_t)clock();
#ifdef _WIN32
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            free(c);
            return NULL;
        }
        c->wsa = 1;
    }
#endif
    return c;
}

/* Releases the socket and per-connection buffers; no callbacks. */
static void teardown(gn_client *c)
{
    if (c->fd != GN_BAD_SOCK) {
        gn_closesocket(c->fd);
        c->fd = GN_BAD_SOCK;
    }
    if (c->addrs) {
        freeaddrinfo(c->addrs);
        c->addrs = NULL;
        c->addr = NULL;
    }
    free(c->request);
    c->request = NULL;
    bytes_free(&c->out);
    bytes_free(&c->in);
    bytes_free(&c->msg);
    c->out_pos = 0;
    c->msg_opcode = 0;
    c->close_sent = 0;
    c->failed = 0;
    c->state = GN_DISCONNECTED;
}

void gn_client_destroy(gn_client *c)
{
    if (!c) {
        return;
    }
    teardown(c);
#ifdef _WIN32
    if (c->wsa) {
        WSACleanup();
    }
#endif
    free(c);
}

gn_client_state gn_client_get_state(const gn_client *c)
{
    return c ? c->state : GN_DISCONNECTED;
}

static void report_error(gn_client *c, int err, const char *message)
{
    if (c->cb.on_error) {
        c->cb.on_error(c->cb.user, err, message);
    }
}

/* Ends the connection and fires on_disconnect. The callback runs last, so it
 * may reconnect; callers must return right after this. */
static void finish(gn_client *c, int code)
{
    teardown(c);
    if (c->cb.on_disconnect) {
        c->cb.on_disconnect(c->cb.user, code);
    }
}

static void fail(gn_client *c, int err, const char *message)
{
    report_error(c, err, message);
    finish(c, 1006);
}

/* ====================================================================== */
/* Connecting                                                             */
/* ====================================================================== */

/* Starts a non-blocking connect to the next candidate address. */
static int start_tcp(gn_client *c)
{
    for (; c->addr; c->addr = c->addr->ai_next) {
        int r;
        if (c->fd != GN_BAD_SOCK) {
            gn_closesocket(c->fd);
        }
        c->fd = socket(c->addr->ai_family, c->addr->ai_socktype, c->addr->ai_protocol);
        if (c->fd == GN_BAD_SOCK) {
            continue;
        }
        if (!set_nonblocking(c->fd)) {
            continue;
        }
        tune_socket(c->fd);
        r = connect(c->fd, c->addr->ai_addr, (int)c->addr->ai_addrlen);
        if (r == 0 || would_block()) {
            return 1;
        }
    }
    return 0;
}

int gn_client_connect(gn_client *c, const char *host, uint16_t port, const char *path)
{
    struct addrinfo hints;
    char port_str[8], *p;
    uint8_t nonce[16], digest[20];
    char key[32], concat[64];
    size_t i;
    if (!c || !host || !*host) {
        return GN_ERR_ARG;
    }
    if (c->state != GN_DISCONNECTED) {
        return GN_ERR_STATE;
    }
    if (!path || !*path) {
        path = "/";
    }
    if (strlen(host) > 255 || strlen(path) > 1024 || path[0] != '/') {
        return GN_ERR_ARG;
    }

    p = port_str;
    append_uint(&p, port);
    *p = 0;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(host, port_str, &hints, &c->addrs) != 0 || !c->addrs) {
        c->addrs = NULL;
        return GN_ERR_SOCKET;
    }
    c->addr = c->addrs;
    if (!start_tcp(c)) {
        teardown(c);
        return GN_ERR_SOCKET;
    }

    /* Handshake: random 16-byte key, and the accept value we expect back. */
    for (i = 0; i < 16; i += 8) {
        uint64_t r = next_random(c);
        memcpy(nonce + i, &r, 8);
    }
    base64(nonce, 16, key);
    memcpy(concat, key, 24);
    memcpy(concat + 24, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", 36);
    sha1((const uint8_t *)concat, 60, digest);
    base64(digest, 20, c->accept);

    c->request = (char *)malloc(strlen(host) + strlen(path) + 256);
    if (!c->request) {
        teardown(c);
        return GN_ERR_MEMORY;
    }
    p = c->request;
    append_str(&p, "GET ");
    append_str(&p, path);
    append_str(&p, " HTTP/1.1\r\nHost: ");
    append_str(&p, host);
    append_str(&p, ":");
    append_uint(&p, port);
    append_str(&p, "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ");
    append_str(&p, key);
    append_str(&p, "\r\nSec-WebSocket-Version: 13\r\n\r\n");
    *p = 0;

    c->state = GN_CONNECTING;
    c->phase = PHASE_TCP;
    c->deadline = now_ms() + GN_CONNECT_TIMEOUT_MS;
    return GN_OK;
}

/* ====================================================================== */
/* Sending                                                                */
/* ====================================================================== */

/* Writes as much of the backlog as the socket takes. 0 on a fatal error. */
static int flush(gn_client *c)
{
    while (c->out_pos < c->out.len) {
        int n = (int)send(c->fd, (const char *)c->out.data + c->out_pos, (int)(c->out.len - c->out_pos), MSG_NOSIGNAL);
        if (n < 0) {
            return would_block() ? 1 : 0;
        }
        c->out_pos += (size_t)n;
    }
    c->out.len = 0;
    c->out_pos = 0;
    return 1;
}

static int queue_bytes(gn_client *c, const void *data, size_t len)
{
    if (!bytes_reserve(&c->out, len, GN_MAX_BACKLOG)) {
        return 0;
    }
    memcpy(c->out.data + c->out.len, data, len);
    c->out.len += len;
    return 1;
}

/* Queues one masked frame (client frames must be masked). */
static int queue_frame(gn_client *c, int opcode, const uint8_t *payload, size_t len)
{
    uint8_t *p;
    size_t header = len < 126 ? 2 : (len < 65536 ? 4 : 10), i;
    uint32_t mask32 = (uint32_t)next_random(c);
    uint8_t mask[4];
    mask[0] = (uint8_t)mask32;
    mask[1] = (uint8_t)(mask32 >> 8);
    mask[2] = (uint8_t)(mask32 >> 16);
    mask[3] = (uint8_t)(mask32 >> 24);
    if (!bytes_reserve(&c->out, header + 4 + len, GN_MAX_BACKLOG)) {
        return 0;
    }
    p = c->out.data + c->out.len;
    p[0] = (uint8_t)(0x80 | opcode); /* FIN */
    if (len < 126) {
        p[1] = (uint8_t)(0x80 | len);
    } else if (len < 65536) {
        p[1] = 0x80 | 126;
        p[2] = (uint8_t)(len >> 8);
        p[3] = (uint8_t)len;
    } else {
        uint64_t l = (uint64_t)len;
        p[1] = 0x80 | 127;
        for (i = 0; i < 8; i++) {
            p[2 + i] = (uint8_t)(l >> (8 * (7 - i)));
        }
    }
    memcpy(p + header, mask, 4);
    for (i = 0; i < len; i++) {
        p[header + 4 + i] = payload[i] ^ mask[i & 3];
    }
    c->out.len += header + 4 + len;
    return 1;
}

int gn_client_send(gn_client *c, const void *data, size_t len)
{
    if (!c || (!data && len > 0)) {
        return GN_ERR_ARG;
    }
    if (c->state != GN_OPEN || c->failed) {
        return GN_ERR_STATE;
    }
    if (len > GN_MAX_MESSAGE) {
        return GN_ERR_TOO_LARGE;
    }
    if (!queue_frame(c, 0x2, (const uint8_t *)data, len)) {
        return GN_ERR_NO_SPACE;
    }
    if (!flush(c)) {
        /* reported from the next poll, never from inside send */
        c->failed = 1;
        c->failed_err = GN_ERR_SOCKET;
    }
    return GN_OK;
}

int gn_client_send_packet(gn_client *c, gn_writer *w)
{
    size_t len = gn_writer_finish(w);
    if (!len) {
        return w ? w->err : GN_ERR_ARG;
    }
    return gn_client_send(c, w->data, len);
}

static void send_close(gn_client *c, int code)
{
    uint8_t payload[2];
    payload[0] = (uint8_t)(code >> 8);
    payload[1] = (uint8_t)code;
    if (!c->close_sent) {
        queue_frame(c, 0x8, payload, 2);
        c->close_sent = 1;
    }
}

void gn_client_close(gn_client *c)
{
    if (!c) {
        return;
    }
    if (c->state == GN_CONNECTING) {
        /* nothing to say goodbye to yet: on_disconnect fires on next poll */
        c->failed = 1;
        c->failed_err = GN_OK;
        c->close_code = 1000;
        return;
    }
    if (c->state != GN_OPEN) {
        return;
    }
    send_close(c, 1000);
    c->state = GN_CLOSING;
    c->close_code = 1000;
    c->deadline = now_ms() + GN_CLOSE_TIMEOUT_MS;
    if (!flush(c)) {
        c->failed = 1;
        c->failed_err = GN_ERR_SOCKET;
    }
}

/* ====================================================================== */
/* Receiving                                                              */
/* ====================================================================== */

/* Reads everything available. 1 = ok, 0 = peer closed, -1 = error. */
static int receive(gn_client *c)
{
    for (;;) {
        int n;
        if (!bytes_reserve(&c->in, 4096, GN_MAX_MESSAGE + 4096)) {
            return 1; /* full: parse what we have first */
        }
        n = (int)recv(c->fd, (char *)c->in.data + c->in.len, (int)(c->in.cap - c->in.len), 0);
        if (n > 0) {
            c->in.len += (size_t)n;
            continue;
        }
        if (n == 0) {
            return 0;
        }
        return would_block() ? 1 : -1;
    }
}

/* 1 = handshake done, 0 = need more bytes, -1 = rejected */
static int parse_handshake(gn_client *c)
{
    const char *s = (const char *)c->in.data, *line, *end;
    size_t i, header_len = 0;
    int upgrade = 0, connection = 0, accept = 0;
    for (i = 3; i < c->in.len; i++) {
        if (s[i - 3] == '\r' && s[i - 2] == '\n' && s[i - 1] == '\r' && s[i] == '\n') {
            header_len = i + 1;
            break;
        }
    }
    if (!header_len) {
        return c->in.len > GN_MAX_HANDSHAKE ? -1 : 0;
    }
    /* status line: HTTP/1.1 101 */
    if (header_len < 12 || memcmp(s, "HTTP/1.1 101", 12) != 0) {
        return -1;
    }
    for (line = s; line < s + header_len; line = end + 2) {
        size_t len, alen = strlen(c->accept);
        end = line;
        while (end < s + header_len && !(end[0] == '\r' && end[1] == '\n')) {
            end++;
        }
        len = (size_t)(end - line);
        if (len > 8 && contains_ci(line, 8, "upgrade:") && contains_ci(line, len, "websocket")) {
            upgrade = 1;
        } else if (len > 11 && contains_ci(line, 11, "connection:") && contains_ci(line, len, "upgrade")) {
            connection = 1;
        } else if (len > 21 && contains_ci(line, 21, "sec-websocket-accept:")) {
            const char *v = line + 21;
            while (v < end && *v == ' ') {
                v++;
            }
            accept = (size_t)(end - v) >= alen && memcmp(v, c->accept, alen) == 0;
        }
    }
    if (!upgrade || !connection || !accept) {
        return -1;
    }
    bytes_consume(&c->in, header_len);
    return 1;
}

/* Delivers each packet in a complete binary message. 0 if the connection
 * ended inside a callback. */
static int deliver(gn_client *c, const uint8_t *data, size_t len)
{
    gn_frames frames;
    const uint8_t *payload;
    size_t plen;
    gn_frames_init(&frames, data, len);
    while (gn_frames_next(&frames, &payload, &plen)) {
        gn_reader r;
        gn_reader_init(&r, payload, plen);
        if (c->cb.on_packet) {
            c->cb.on_packet(c->cb.user, &r);
        }
        if (c->state != GN_OPEN && c->state != GN_CLOSING) {
            return 0;
        }
    }
    return 1;
}

/* Parses complete frames from c->in. 0 if the connection ended (callbacks
 * already fired), 1 otherwise. */
static int parse_frames(gn_client *c)
{
    size_t pos = 0;
    int keep_going = 1;
    while (keep_going) {
        const uint8_t *p = c->in.data + pos;
        size_t avail = c->in.len - pos, header = 2, i;
        uint64_t len;
        int fin, opcode, masked;
        uint8_t mask[4] = { 0, 0, 0, 0 };
        uint8_t *payload;
        if (avail < 2) {
            break;
        }
        fin = p[0] & 0x80;
        opcode = p[0] & 0x0f;
        masked = p[1] & 0x80;
        len = p[1] & 0x7f;
        if (p[0] & 0x70) {
            fail(c, GN_ERR_MALFORMED, "WebSocket frame uses reserved bits");
            return 0;
        }
        if (len == 126) {
            if (avail < 4) break;
            len = ((uint64_t)p[2] << 8) | p[3];
            header = 4;
        } else if (len == 127) {
            if (avail < 10) break;
            len = 0;
            for (i = 0; i < 8; i++) {
                len = (len << 8) | p[2 + i];
            }
            header = 10;
        }
        if (masked) {
            if (avail < header + 4) break;
            memcpy(mask, p + header, 4);
            header += 4;
        }
        if (len > GN_MAX_MESSAGE) {
            send_close(c, 1009);
            flush(c);
            fail(c, GN_ERR_TOO_LARGE, "WebSocket message too large");
            return 0;
        }
        if (avail < header + len) {
            break; /* wait for the rest of the frame */
        }
        payload = c->in.data + pos + header;
        if (masked) {
            for (i = 0; i < len; i++) {
                payload[i] ^= mask[i & 3];
            }
        }
        pos += header + (size_t)len;

        if (opcode >= 0x8) { /* control frame */
            if (!fin || len > 125) {
                fail(c, GN_ERR_MALFORMED, "invalid WebSocket control frame");
                return 0;
            }
            if (opcode == 0x9) { /* ping */
                queue_frame(c, 0xA, payload, (size_t)len);
            } else if (opcode == 0x8) { /* close */
                int code = len >= 2 ? ((int)payload[0] << 8) | payload[1] : 1005;
                send_close(c, code == 1005 ? 1000 : code);
                flush(c);
                finish(c, c->state == GN_CLOSING ? c->close_code : code);
                return 0;
            }
            continue; /* pong: nothing to do */
        }

        if (opcode == 0x0) { /* continuation */
            if (!c->msg_opcode) {
                fail(c, GN_ERR_MALFORMED, "unexpected WebSocket continuation frame");
                return 0;
            }
            if (!bytes_reserve(&c->msg, (size_t)len, GN_MAX_MESSAGE)) {
                fail(c, GN_ERR_TOO_LARGE, "WebSocket message too large");
                return 0;
            }
            if (len > 0) {
                memcpy(c->msg.data + c->msg.len, payload, (size_t)len);
                c->msg.len += (size_t)len;
            }
            if (fin) {
                int op = c->msg_opcode;
                c->msg_opcode = 0;
                if (op == 0x2) {
                    keep_going = deliver(c, c->msg.data, c->msg.len);
                } else {
                    report_error(c, GN_ERR_MALFORMED, "received a text message (gn uses binary messages)");
                }
                c->msg.len = 0;
            }
        } else if (opcode == 0x1 || opcode == 0x2) {
            if (c->msg_opcode) {
                fail(c, GN_ERR_MALFORMED, "WebSocket message interleaved with a fragmented one");
                return 0;
            }
            if (!fin) {
                c->msg_opcode = opcode;
                c->msg.len = 0;
                if (!bytes_reserve(&c->msg, (size_t)len, GN_MAX_MESSAGE)) {
                    fail(c, GN_ERR_TOO_LARGE, "WebSocket message too large");
                    return 0;
                }
                if (len > 0) {
                    memcpy(c->msg.data, payload, (size_t)len);
                }
                c->msg.len = (size_t)len;
            } else if (opcode == 0x2) {
                keep_going = deliver(c, payload, (size_t)len);
            } else {
                report_error(c, GN_ERR_MALFORMED, "received a text message (gn uses binary messages)");
                keep_going = c->state == GN_OPEN || c->state == GN_CLOSING;
            }
        } else {
            fail(c, GN_ERR_MALFORMED, "unknown WebSocket opcode");
            return 0;
        }
    }
    if (!keep_going) {
        return 0;
    }
    bytes_consume(&c->in, pos);
    return 1;
}

/* ====================================================================== */
/* Poll                                                                   */
/* ====================================================================== */

void gn_client_poll(gn_client *c)
{
    int r;
    if (!c || c->state == GN_DISCONNECTED) {
        return;
    }
    if (c->failed) {
        if (c->failed_err) {
            fail(c, c->failed_err, "connection lost while sending");
        } else {
            finish(c, c->close_code);
        }
        return;
    }

    if (c->state == GN_CONNECTING && c->phase == PHASE_TCP) {
        r = connect_status(c->fd);
        if (r < 0 || (r == 0 && past(c->deadline))) {
            /* try the next resolved address, if any */
            if (r < 0 && c->addr && (c->addr = c->addr->ai_next) != NULL && start_tcp(c)) {
                return;
            }
            fail(c, GN_ERR_SOCKET, r < 0 ? "could not connect" : "connect timed out");
            return;
        }
        if (r == 0) {
            return;
        }
        freeaddrinfo(c->addrs);
        c->addrs = NULL;
        c->addr = NULL;
        if (!queue_bytes(c, c->request, strlen(c->request))) {
            fail(c, GN_ERR_MEMORY, "out of memory");
            return;
        }
        free(c->request);
        c->request = NULL;
        c->phase = PHASE_HANDSHAKE;
    }

    if (!flush(c)) {
        fail(c, GN_ERR_SOCKET, "connection lost while sending");
        return;
    }

    r = receive(c);
    if (r < 0) {
        fail(c, GN_ERR_SOCKET, "connection lost");
        return;
    }

    if (c->phase == PHASE_HANDSHAKE) {
        int h = parse_handshake(c);
        if (h < 0) {
            fail(c, GN_ERR_HANDSHAKE, "server did not accept the WebSocket upgrade");
            return;
        }
        if (h == 0) {
            if (r == 0) {
                fail(c, GN_ERR_HANDSHAKE, "server closed the connection during the handshake");
            } else if (past(c->deadline)) {
                fail(c, GN_ERR_HANDSHAKE, "WebSocket handshake timed out");
            }
            return;
        }
        c->phase = PHASE_WS;
        c->state = GN_OPEN;
        if (c->cb.on_connect) {
            c->cb.on_connect(c->cb.user);
        }
        if (c->state != GN_OPEN) {
            return; /* closed or reconnected from the callback */
        }
    }

    if (!parse_frames(c)) {
        return;
    }
    if (r == 0) {
        /* peer closed TCP without a close frame */
        finish(c, c->state == GN_CLOSING ? c->close_code : 1006);
        return;
    }
    if (!flush(c)) { /* pongs / close replies queued while parsing */
        fail(c, GN_ERR_SOCKET, "connection lost while sending");
        return;
    }
    if (c->state == GN_CLOSING && past(c->deadline)) {
        finish(c, c->close_code);
    }
}

#ifdef GN_FUZZING
/* Test hook: feed raw bytes to a client as if the server had sent them,
 * starting from the handshake (first byte odd) or an open connection. */
void gn__fuzz_client_feed(const uint8_t *data, size_t len);
void gn__fuzz_client_feed(const uint8_t *data, size_t len)
{
    gn_client *c = gn_client_create(NULL);
    if (!c || len < 2) {
        gn_client_destroy(c);
        return;
    }
    c->state = GN_CONNECTING;
    c->phase = PHASE_HANDSHAKE;
    strcpy(c->accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    if (data[0] & 1) {
        c->state = GN_OPEN;
        c->phase = PHASE_WS;
    }
    if (bytes_reserve(&c->in, len - 1, GN_MAX_MESSAGE + 4096)) {
        memcpy(c->in.data, data + 1, len - 1);
        c->in.len = len - 1;
        if (c->phase == PHASE_HANDSHAKE) {
            if (parse_handshake(c) == 1) {
                c->phase = PHASE_WS;
                c->state = GN_OPEN;
            }
        }
        if (c->phase == PHASE_WS && c->state == GN_OPEN) {
            parse_frames(c);
        }
    }
    gn_client_destroy(c);
}
#endif
