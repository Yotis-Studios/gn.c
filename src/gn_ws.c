/* gn_ws.c - WebSocket (RFC 6455) plumbing shared by the client and server:
 * sockets, buffers, framing, ping/close handling and handshake helpers.
 *
 * Avoids C99 library functions missing from older MSVC runtimes (snprintf,
 * strcasecmp) so it builds against the VC9 CRT. */
#include "gn_internal.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* ====================================================================== */
/* Platform                                                               */
/* ====================================================================== */

int gn__net_init(void)
{
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    return 1;
#endif
}

void gn__net_done(void)
{
#ifdef _WIN32
    WSACleanup();
#endif
}

uint32_t gn__now_ms(void)
{
#ifdef _WIN32
    return (uint32_t)GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
#endif
}

int gn__past(uint32_t deadline)
{
    return (int32_t)(gn__now_ms() - deadline) >= 0; /* wrap-safe */
}

int gn__would_block(void)
{
#ifdef _WIN32
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS || errno == EINTR;
#endif
}

int gn__set_nonblocking(gn_sock fd)
{
#ifdef _WIN32
    u_long on = 1;
    return ioctlsocket(fd, FIONBIO, &on) == 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

void gn__tune_socket(gn_sock fd)
{
    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof on);
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&on, sizeof on);
#endif
}

/* ====================================================================== */
/* Helpers                                                                */
/* ====================================================================== */

int gn__bytes_reserve(gn_bytes *b, size_t extra, size_t limit)
{
    size_t need = b->len + extra, cap;
    uint8_t *p;
    if (need <= b->cap && b->data) {
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

void gn__bytes_consume(gn_bytes *b, size_t n)
{
    if (n >= b->len) {
        b->len = 0;
        return;
    }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
}

void gn__bytes_free(gn_bytes *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

uint64_t gn__seed(const void *salt)
{
    static uint64_t counter;
    counter += 0x9e3779b97f4a7c15ULL;
    return (uint64_t)time(NULL) ^ ((uint64_t)gn__now_ms() << 32) ^ (uint64_t)(size_t)salt ^
           (uint64_t)clock() ^ counter;
}

uint64_t gn__random(uint64_t *state)
{
    /* splitmix64: masking keys and handshake nonces only need to be
     * unpredictable to intermediaries, not cryptographically strong. Never
     * touches the C library's rand(), so game RNG state is unaffected. */
    uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static int ascii_lower(int ch)
{
    return (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch;
}

/* case-insensitive: does s[0..len) contain needle? */
int gn__contains_ci(const char *s, size_t len, const char *needle)
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

void gn__append_str(char **p, const char *s)
{
    size_t n = strlen(s);
    memcpy(*p, s, n);
    *p += n;
}

void gn__append_uint(char **p, unsigned v)
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

size_t gn__header_end(const uint8_t *s, size_t len)
{
    size_t i;
    for (i = 3; i < len; i++) {
        if (s[i - 3] == '\r' && s[i - 2] == '\n' && s[i - 1] == '\r' && s[i] == '\n') {
            return i + 1;
        }
    }
    return 0;
}

const char *gn__header_value(const char *s, size_t len, const char *name, size_t *value_len)
{
    const char *line = s, *end;
    size_t name_len = strlen(name);
    while (line < s + len) {
        size_t line_len;
        end = line;
        while (end + 1 < s + len && !(end[0] == '\r' && end[1] == '\n')) {
            end++;
        }
        if (end + 1 >= s + len) {
            end = s + len;
        }
        line_len = (size_t)(end - line);
        if (line_len >= name_len && gn__contains_ci(line, name_len, name)) {
            const char *v = line + name_len, *e = end;
            while (v < e && (*v == ' ' || *v == '\t')) v++;
            while (e > v && (e[-1] == ' ' || e[-1] == '\t')) e--;
            *value_len = (size_t)(e - v);
            return v;
        }
        line = end + 2;
    }
    return NULL;
}

/* ---- SHA-1 and base64, for Sec-WebSocket-Accept (RFC 6455 4.2.2) ---- */

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
        uint32_t w[80], a, b, c, d, e;
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
        a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];
        for (i = 0; i < 80; i++) {
            uint32_t f, k, t;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdcu; }
            else { f = b ^ c ^ d; k = 0xca62c1d6u; }
            t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8);
        out[4 * i + 3] = (uint8_t)h[i];
    }
}

void gn__base64(const uint8_t *in, size_t len, char *out)
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

void gn__accept_key(const char *key, size_t key_len, char *out)
{
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t buf[128 + 36], digest[20];
    if (key_len > 128) {
        key_len = 128;
    }
    memcpy(buf, key, key_len);
    memcpy(buf + key_len, guid, 36);
    sha1(buf, key_len + 36, digest);
    gn__base64(digest, 20, out);
}

/* ====================================================================== */
/* Connection                                                             */
/* ====================================================================== */

void gn__ws_init(gn_ws *ws, gn_sock fd, int is_client, uint64_t seed)
{
    memset(ws, 0, sizeof *ws);
    ws->fd = fd;
    ws->is_client = is_client;
    ws->rng = seed;
}

void gn__ws_reset(gn_ws *ws)
{
    if (ws->fd != GN_BAD_SOCK) {
        gn_closesocket(ws->fd);
        ws->fd = GN_BAD_SOCK;
    }
    gn__bytes_free(&ws->out);
    gn__bytes_free(&ws->in);
    gn__bytes_free(&ws->msg);
    ws->out_pos = 0;
    ws->msg_opcode = 0;
    ws->close_sent = 0;
}

int gn__ws_flush(gn_ws *ws)
{
    while (ws->out_pos < ws->out.len) {
        int n = (int)send(ws->fd, (const char *)ws->out.data + ws->out_pos, (int)(ws->out.len - ws->out_pos), MSG_NOSIGNAL);
        if (n < 0) {
            return gn__would_block() ? 1 : 0;
        }
        ws->out_pos += (size_t)n;
    }
    ws->out.len = 0;
    ws->out_pos = 0;
    return 1;
}

int gn__ws_queue(gn_ws *ws, const void *data, size_t len)
{
    if (!gn__bytes_reserve(&ws->out, len, GN_MAX_BACKLOG)) {
        return 0;
    }
    if (len > 0) {
        memcpy(ws->out.data + ws->out.len, data, len);
        ws->out.len += len;
    }
    return 1;
}

size_t gn__ws_frame_size(size_t len, int masked)
{
    size_t header = len < 126 ? 2 : (len < 65536 ? 4 : 10);
    return header + (masked ? 4 : 0) + len;
}

void gn__ws_write_frame(uint8_t *p, int opcode, const uint8_t *payload, size_t len, const uint8_t *mask)
{
    size_t header = len < 126 ? 2 : (len < 65536 ? 4 : 10), i;
    uint8_t mask_bit = mask ? 0x80 : 0;
    p[0] = (uint8_t)(0x80 | opcode); /* FIN */
    if (len < 126) {
        p[1] = (uint8_t)(mask_bit | len);
    } else if (len < 65536) {
        p[1] = (uint8_t)(mask_bit | 126);
        p[2] = (uint8_t)(len >> 8);
        p[3] = (uint8_t)len;
    } else {
        uint64_t l = (uint64_t)len;
        p[1] = (uint8_t)(mask_bit | 127);
        for (i = 0; i < 8; i++) {
            p[2 + i] = (uint8_t)(l >> (8 * (7 - i)));
        }
    }
    if (mask) {
        memcpy(p + header, mask, 4);
        for (i = 0; i < len; i++) {
            p[header + 4 + i] = payload[i] ^ mask[i & 3];
        }
    } else if (len > 0) {
        memcpy(p + header, payload, len);
    }
}

int gn__ws_queue_frame(gn_ws *ws, int opcode, const uint8_t *payload, size_t len)
{
    size_t size = gn__ws_frame_size(len, ws->is_client);
    uint8_t mask[4];
    if (!gn__bytes_reserve(&ws->out, size, GN_MAX_BACKLOG)) {
        return 0;
    }
    if (ws->is_client) {
        uint32_t m = (uint32_t)gn__random(&ws->rng);
        mask[0] = (uint8_t)m;
        mask[1] = (uint8_t)(m >> 8);
        mask[2] = (uint8_t)(m >> 16);
        mask[3] = (uint8_t)(m >> 24);
    }
    gn__ws_write_frame(ws->out.data + ws->out.len, opcode, payload, len, ws->is_client ? mask : NULL);
    ws->out.len += size;
    return 1;
}

void gn__ws_queue_close(gn_ws *ws, int code)
{
    uint8_t payload[2];
    if (ws->close_sent) {
        return;
    }
    payload[0] = (uint8_t)(code >> 8);
    payload[1] = (uint8_t)code;
    gn__ws_queue_frame(ws, 0x8, payload, 2);
    ws->close_sent = 1;
}

int gn__ws_receive(gn_ws *ws)
{
    for (;;) {
        int n;
        if (!gn__bytes_reserve(&ws->in, 4096, GN_MAX_MESSAGE + 4096)) {
            return 1; /* full: parse what we have first */
        }
        n = (int)recv(ws->fd, (char *)ws->in.data + ws->in.len, (int)(ws->in.cap - ws->in.len), 0);
        if (n > 0) {
            ws->in.len += (size_t)n;
            continue;
        }
        if (n == 0) {
            return 0;
        }
        return gn__would_block() ? 1 : -1;
    }
}

static int ws_error(gn_ws *ws, int err, int close_code, const char *message)
{
    ws->error = err;
    ws->error_message = message;
    gn__ws_queue_close(ws, close_code);
    return GN_WS_ERROR;
}

static int deliver_text(gn_ws *ws, const gn_ws_events *ev, void *ctx)
{
    (void)ws;
    return ev->on_text ? ev->on_text(ctx) : 1;
}

int gn__ws_parse(gn_ws *ws, const gn_ws_events *ev, void *ctx)
{
    size_t pos = 0;
    for (;;) {
        const uint8_t *p = ws->in.data + pos;
        size_t avail = ws->in.len - pos, header = 2, i;
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
            return ws_error(ws, GN_ERR_MALFORMED, 1002, "WebSocket frame uses reserved bits");
        }
        if (!masked && !ws->is_client) {
            return ws_error(ws, GN_ERR_MALFORMED, 1002, "client frame is not masked");
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
            return ws_error(ws, GN_ERR_TOO_LARGE, 1009, "WebSocket message too large");
        }
        if (avail < header + len) {
            break; /* wait for the rest of the frame */
        }
        payload = ws->in.data + pos + header;
        if (masked) {
            for (i = 0; i < len; i++) {
                payload[i] ^= mask[i & 3];
            }
        }
        pos += header + (size_t)len;

        if (opcode >= 0x8) { /* control frame */
            if (!fin || len > 125) {
                return ws_error(ws, GN_ERR_MALFORMED, 1002, "invalid WebSocket control frame");
            }
            if (opcode == 0x9) { /* ping */
                gn__ws_queue_frame(ws, 0xA, payload, (size_t)len);
            } else if (opcode == 0x8) { /* close */
                int code = len >= 2 ? ((int)payload[0] << 8) | payload[1] : 1005;
                ws->peer_close_code = code;
                gn__ws_queue_close(ws, code == 1005 ? 1000 : code);
                return GN_WS_CLOSED;
            } else if (opcode != 0xA) {
                return ws_error(ws, GN_ERR_MALFORMED, 1002, "unknown WebSocket opcode");
            }
            continue;
        }

        if (opcode == 0x0) { /* continuation */
            if (!ws->msg_opcode) {
                return ws_error(ws, GN_ERR_MALFORMED, 1002, "unexpected WebSocket continuation frame");
            }
            if (!gn__bytes_reserve(&ws->msg, (size_t)len, GN_MAX_MESSAGE)) {
                return ws_error(ws, GN_ERR_TOO_LARGE, 1009, "WebSocket message too large");
            }
            if (len > 0) {
                memcpy(ws->msg.data + ws->msg.len, payload, (size_t)len);
                ws->msg.len += (size_t)len;
            }
            if (fin) {
                int op = ws->msg_opcode, keep;
                ws->msg_opcode = 0;
                if (op == 0x2) {
                    keep = ev->on_binary ? ev->on_binary(ctx, ws->msg.data, ws->msg.len) : 1;
                } else {
                    keep = deliver_text(ws, ev, ctx);
                }
                if (!keep) {
                    return GN_WS_STOPPED;
                }
                ws->msg.len = 0;
            }
        } else if (opcode == 0x1 || opcode == 0x2) {
            if (ws->msg_opcode) {
                return ws_error(ws, GN_ERR_MALFORMED, 1002, "WebSocket message interleaved with a fragmented one");
            }
            if (!fin) {
                ws->msg_opcode = opcode;
                ws->msg.len = 0;
                if (!gn__bytes_reserve(&ws->msg, (size_t)len, GN_MAX_MESSAGE)) {
                    return ws_error(ws, GN_ERR_TOO_LARGE, 1009, "WebSocket message too large");
                }
                if (len > 0) {
                    memcpy(ws->msg.data, payload, (size_t)len);
                }
                ws->msg.len = (size_t)len;
            } else {
                int keep = opcode == 0x2
                               ? (ev->on_binary ? ev->on_binary(ctx, payload, (size_t)len) : 1)
                               : deliver_text(ws, ev, ctx);
                if (!keep) {
                    return GN_WS_STOPPED;
                }
            }
        } else {
            return ws_error(ws, GN_ERR_MALFORMED, 1002, "unknown WebSocket opcode");
        }
    }
    gn__bytes_consume(&ws->in, pos);
    return GN_WS_OK;
}
