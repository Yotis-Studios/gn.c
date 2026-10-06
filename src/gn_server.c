/* gn_server.c - non-blocking WebSocket server for gn
 *
 * gn_server_poll() accepts new sockets, answers WebSocket upgrades, parses
 * frames and runs the close handshakes. Connections that end are only freed
 * at the end of a poll, so a gn_conn stays valid for the whole poll that
 * reports its on_disconnect. */
#include "gn_internal.h"

enum { CONN_HANDSHAKE, CONN_OPEN, CONN_CLOSING, CONN_DEAD };

struct gn_conn {
    gn_server *server;
    gn_ws ws;
    int state;
    uint32_t id;
    void *user;
    uint32_t deadline; /* handshake or close timeout */
    int close_code;
    int failed;        /* send error noticed outside poll */
    char path[256];
};

struct gn_server {
    gn_server_callbacks cb;
    gn_sock listener;
    uint16_t port;
    gn_conn **conns;
    size_t nconns;
    size_t cap;
    uint32_t next_id;
    uint64_t rng;
    int net;
};

/* ====================================================================== */
/* Lifecycle                                                              */
/* ====================================================================== */

gn_server *gn_server_create(const gn_server_callbacks *callbacks)
{
    gn_server *s = (gn_server *)calloc(1, sizeof *s);
    if (!s) {
        return NULL;
    }
    if (!gn__net_init()) {
        free(s);
        return NULL;
    }
    s->net = 1;
    if (callbacks) {
        s->cb = *callbacks;
    }
    s->listener = GN_BAD_SOCK;
    s->rng = gn__seed(s);
    return s;
}

static void stop_listening(gn_server *s)
{
    if (s->listener != GN_BAD_SOCK) {
        gn_closesocket(s->listener);
        s->listener = GN_BAD_SOCK;
    }
    s->port = 0;
}

void gn_server_destroy(gn_server *s)
{
    size_t i;
    if (!s) {
        return;
    }
    stop_listening(s);
    for (i = 0; i < s->nconns; i++) {
        gn__ws_reset(&s->conns[i]->ws);
        free(s->conns[i]);
    }
    free(s->conns);
    if (s->net) {
        gn__net_done();
    }
    free(s);
}

int gn_server_listen(gn_server *s, const char *host, uint16_t port)
{
    struct sockaddr_storage addr;
    socklen_t addr_len;
    gn_sock fd;
    int on = 1;
    if (!s) {
        return GN_ERR_ARG;
    }
    if (s->listener != GN_BAD_SOCK) {
        return GN_ERR_STATE;
    }
    memset(&addr, 0, sizeof addr);
    if (!host) {
        struct sockaddr_in *in = (struct sockaddr_in *)&addr;
        in->sin_family = AF_INET;
        in->sin_addr.s_addr = htonl(INADDR_ANY);
        in->sin_port = htons(port);
        addr_len = sizeof *in;
    } else {
        struct addrinfo hints, *res = NULL;
        char port_str[8], *p = port_str;
        gn__append_uint(&p, port);
        *p = 0;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;
        if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
            return GN_ERR_SOCKET;
        }
        memcpy(&addr, res->ai_addr, res->ai_addrlen);
        addr_len = (socklen_t)res->ai_addrlen;
        freeaddrinfo(res);
    }

    fd = socket(((struct sockaddr *)&addr)->sa_family, SOCK_STREAM, IPPROTO_TCP);
    if (fd == GN_BAD_SOCK) {
        return GN_ERR_SOCKET;
    }
#ifndef _WIN32
    /* rebind straight after a restart; on Windows this would allow port
     * stealing, and the default already behaves */
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof on);
#endif
    (void)on;
    if (bind(fd, (struct sockaddr *)&addr, addr_len) != 0 || listen(fd, 64) != 0 || !gn__set_nonblocking(fd)) {
        gn_closesocket(fd);
        return GN_ERR_SOCKET;
    }
    addr_len = sizeof addr;
    if (getsockname(fd, (struct sockaddr *)&addr, &addr_len) == 0) {
        if (((struct sockaddr *)&addr)->sa_family == AF_INET6) {
            port = ntohs(((struct sockaddr_in6 *)&addr)->sin6_port);
        } else {
            port = ntohs(((struct sockaddr_in *)&addr)->sin_port);
        }
    }
    s->listener = fd;
    s->port = port;
    return GN_OK;
}

uint16_t gn_server_port(const gn_server *s)
{
    return s ? s->port : 0;
}

size_t gn_server_connection_count(const gn_server *s)
{
    size_t i, n = 0;
    if (!s) {
        return 0;
    }
    for (i = 0; i < s->nconns; i++) {
        if (s->conns[i]->state == CONN_OPEN || s->conns[i]->state == CONN_CLOSING) {
            n++;
        }
    }
    return n;
}

/* ====================================================================== */
/* Connections                                                            */
/* ====================================================================== */

uint32_t gn_conn_id(const gn_conn *c) { return c ? c->id : 0; }
const char *gn_conn_path(const gn_conn *c) { return c ? c->path : ""; }
void gn_conn_set_user(gn_conn *c, void *user) { if (c) c->user = user; }
void *gn_conn_get_user(const gn_conn *c) { return c ? c->user : NULL; }

static void report_error(gn_server *s, gn_conn *c, int err, const char *message)
{
    if (s->cb.on_error) {
        s->cb.on_error(s->cb.user, c, err, message);
    }
}

/* Ends a connection. on_disconnect fires only for clients that connected;
 * the gn_conn is freed later, by sweep(). */
static void end_conn(gn_conn *c, int code)
{
    int was_connected = c->state == CONN_OPEN || c->state == CONN_CLOSING;
    gn__ws_reset(&c->ws);
    c->state = CONN_DEAD;
    if (was_connected && c->server->cb.on_disconnect) {
        c->server->cb.on_disconnect(c->server->cb.user, c, code);
    }
}

static void fail_conn(gn_conn *c, int err, const char *message)
{
    if (c->state == CONN_HANDSHAKE) {
        report_error(c->server, NULL, err, message);
    } else {
        report_error(c->server, c, err, message);
    }
    end_conn(c, 1006);
}

static void sweep(gn_server *s)
{
    size_t i, j = 0;
    for (i = 0; i < s->nconns; i++) {
        if (s->conns[i]->state == CONN_DEAD) {
            free(s->conns[i]);
        } else {
            s->conns[j++] = s->conns[i];
        }
    }
    s->nconns = j;
}

int gn_conn_send(gn_conn *c, const void *data, size_t len)
{
    if (!c || (!data && len > 0)) {
        return GN_ERR_ARG;
    }
    if (c->state != CONN_OPEN || c->failed) {
        return GN_ERR_STATE;
    }
    if (len > GN_MAX_MESSAGE) {
        return GN_ERR_TOO_LARGE;
    }
    if (!gn__ws_queue_frame(&c->ws, 0x2, (const uint8_t *)data, len)) {
        return GN_ERR_NO_SPACE;
    }
    if (!gn__ws_flush(&c->ws)) {
        c->failed = 1; /* reported from the next poll */
    }
    return GN_OK;
}

int gn_conn_send_packet(gn_conn *c, gn_writer *w)
{
    size_t len = gn_writer_finish(w);
    if (!len) {
        return w ? w->err : GN_ERR_ARG;
    }
    return gn_conn_send(c, w->data, len);
}

void gn_conn_close(gn_conn *c, int code)
{
    if (!c || c->state != CONN_OPEN) {
        return;
    }
    gn__ws_queue_close(&c->ws, code);
    c->state = CONN_CLOSING;
    c->close_code = code;
    c->deadline = gn__now_ms() + GN_CLOSE_TIMEOUT_MS;
    if (!gn__ws_flush(&c->ws)) {
        c->failed = 1;
    }
}

int gn_server_broadcast(gn_server *s, const void *data, size_t len, gn_conn *except)
{
    size_t i;
    int result = GN_OK;
    if (!s || (!data && len > 0)) {
        return GN_ERR_ARG;
    }
    for (i = 0; i < s->nconns; i++) {
        gn_conn *c = s->conns[i];
        if (c != except && c->state == CONN_OPEN && !c->failed) {
            int r = gn_conn_send(c, data, len);
            if (r != GN_OK) {
                result = r;
            }
        }
    }
    return result;
}

int gn_server_broadcast_packet(gn_server *s, gn_writer *w, gn_conn *except)
{
    size_t len = gn_writer_finish(w);
    if (!len) {
        return w ? w->err : GN_ERR_ARG;
    }
    return gn_server_broadcast(s, w->data, len, except);
}

void gn_server_close(gn_server *s)
{
    size_t i;
    if (!s) {
        return;
    }
    stop_listening(s);
    for (i = 0; i < s->nconns; i++) {
        gn_conn *c = s->conns[i];
        if (c->state == CONN_HANDSHAKE) {
            end_conn(c, 1001); /* never connected: no callback */
        } else {
            gn_conn_close(c, 1001);
        }
    }
}

/* ====================================================================== */
/* Handshake                                                              */
/* ====================================================================== */

static void reject(gn_conn *c, const char *status, const char *message)
{
    char response[160], *p = response;
    gn__append_str(&p, "HTTP/1.1 ");
    gn__append_str(&p, status);
    gn__append_str(&p, "\r\nSec-WebSocket-Version: 13\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    gn__ws_queue(&c->ws, response, (size_t)(p - response));
    gn__ws_flush(&c->ws);
    fail_conn(c, GN_ERR_HANDSHAKE, message);
}

/* 1 = upgraded, 0 = need more bytes, -1 = rejected (already handled) */
static int handshake(gn_conn *c)
{
    const char *s = (const char *)c->ws.in.data, *v, *path, *path_end;
    size_t header_len = gn__header_end(c->ws.in.data, c->ws.in.len), vlen;
    char accept[32], response[192], *p;
    if (!header_len) {
        if (c->ws.in.len > GN_MAX_HANDSHAKE) {
            reject(c, "431 Request Header Fields Too Large", "WebSocket handshake too large");
            return -1;
        }
        return 0;
    }
    /* request line: GET <path> HTTP/1.1 */
    if (header_len < 14 || memcmp(s, "GET ", 4) != 0) {
        reject(c, "400 Bad Request", "not a WebSocket upgrade request");
        return -1;
    }
    path = s + 4;
    path_end = path;
    while (path_end < s + header_len && *path_end != ' ' && *path_end != '\r') {
        path_end++;
    }
    if (path_end - path < 1 || (size_t)(path_end - path) >= sizeof c->path || *path != '/' ||
        (size_t)(s + header_len - path_end) < 9 || memcmp(path_end, " HTTP/1.1", 9) != 0) {
        reject(c, "400 Bad Request", "malformed WebSocket upgrade request");
        return -1;
    }
    v = gn__header_value(s, header_len, "upgrade:", &vlen);
    if (!v || !gn__contains_ci(v, vlen, "websocket")) {
        reject(c, "400 Bad Request", "request is not a WebSocket upgrade");
        return -1;
    }
    v = gn__header_value(s, header_len, "connection:", &vlen);
    if (!v || !gn__contains_ci(v, vlen, "upgrade")) {
        reject(c, "400 Bad Request", "request is not a WebSocket upgrade");
        return -1;
    }
    v = gn__header_value(s, header_len, "sec-websocket-version:", &vlen);
    if (!v || vlen != 2 || memcmp(v, "13", 2) != 0) {
        reject(c, "426 Upgrade Required", "unsupported WebSocket version");
        return -1;
    }
    v = gn__header_value(s, header_len, "sec-websocket-key:", &vlen);
    if (!v || vlen == 0 || vlen > 64) {
        reject(c, "400 Bad Request", "missing Sec-WebSocket-Key");
        return -1;
    }

    memcpy(c->path, path, (size_t)(path_end - path));
    c->path[path_end - path] = 0;
    gn__accept_key(v, vlen, accept);
    p = response;
    gn__append_str(&p, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ");
    gn__append_str(&p, accept);
    gn__append_str(&p, "\r\n\r\n");
    gn__bytes_consume(&c->ws.in, header_len);
    if (!gn__ws_queue(&c->ws, response, (size_t)(p - response))) {
        fail_conn(c, GN_ERR_MEMORY, "out of memory");
        return -1;
    }
    return 1;
}

/* ====================================================================== */
/* Poll                                                                   */
/* ====================================================================== */

static void accept_new(gn_server *s)
{
    for (;;) {
        gn_conn *c;
        gn_sock fd = accept(s->listener, NULL, NULL);
        if (fd == GN_BAD_SOCK) {
            return; /* would block, or a transient error: try next poll */
        }
        if (!gn__set_nonblocking(fd)) {
            gn_closesocket(fd);
            continue;
        }
        gn__tune_socket(fd);
        if (s->nconns == s->cap) {
            size_t cap = s->cap ? s->cap * 2 : 16;
            gn_conn **conns = (gn_conn **)realloc(s->conns, cap * sizeof *conns);
            if (!conns) {
                gn_closesocket(fd);
                report_error(s, NULL, GN_ERR_MEMORY, "out of memory accepting a connection");
                return;
            }
            s->conns = conns;
            s->cap = cap;
        }
        c = (gn_conn *)calloc(1, sizeof *c);
        if (!c) {
            gn_closesocket(fd);
            report_error(s, NULL, GN_ERR_MEMORY, "out of memory accepting a connection");
            return;
        }
        c->server = s;
        c->state = CONN_HANDSHAKE;
        c->deadline = gn__now_ms() + GN_CONNECT_TIMEOUT_MS;
        gn__ws_init(&c->ws, fd, 0, gn__random(&s->rng));
        s->conns[s->nconns++] = c;
    }
}

static int on_binary(void *ctx, const uint8_t *data, size_t len)
{
    gn_conn *c = (gn_conn *)ctx;
    gn_server *s = c->server;
    gn_frames frames;
    const uint8_t *payload;
    size_t plen;
    gn_frames_init(&frames, data, len);
    while (gn_frames_next(&frames, &payload, &plen)) {
        gn_reader r;
        gn_reader_init(&r, payload, plen);
        if (s->cb.on_packet) {
            s->cb.on_packet(s->cb.user, c, &r);
        }
    }
    return 1;
}

static int on_text(void *ctx)
{
    gn_conn *c = (gn_conn *)ctx;
    report_error(c->server, c, GN_ERR_MALFORMED, "received a text message (gn uses binary messages)");
    return 1;
}

static void poll_conn(gn_conn *c)
{
    static const gn_ws_events events = { on_binary, on_text };
    gn_server *s = c->server;
    int r;
    if (c->failed) {
        fail_conn(c, GN_ERR_SOCKET, "connection lost while sending");
        return;
    }
    if (!gn__ws_flush(&c->ws)) {
        fail_conn(c, GN_ERR_SOCKET, "connection lost while sending");
        return;
    }
    r = gn__ws_receive(&c->ws);
    if (r < 0) {
        fail_conn(c, GN_ERR_SOCKET, "connection lost");
        return;
    }

    if (c->state == CONN_HANDSHAKE) {
        int h = handshake(c);
        if (h < 0) {
            return;
        }
        if (h == 0) {
            if (r == 0 || gn__past(c->deadline)) {
                fail_conn(c, GN_ERR_HANDSHAKE, r == 0 ? "client left during the handshake" : "WebSocket handshake timed out");
            }
            return;
        }
        c->id = ++s->next_id;
        c->state = CONN_OPEN;
        if (!gn__ws_flush(&c->ws)) {
            fail_conn(c, GN_ERR_SOCKET, "connection lost while sending");
            return;
        }
        if (s->cb.on_connect) {
            s->cb.on_connect(s->cb.user, c);
        }
        if (c->state != CONN_OPEN && c->state != CONN_CLOSING) {
            return;
        }
    }

    switch (gn__ws_parse(&c->ws, &events, c)) {
    case GN_WS_CLOSED:
        gn__ws_flush(&c->ws);
        end_conn(c, c->state == CONN_CLOSING ? c->close_code : c->ws.peer_close_code);
        return;
    case GN_WS_ERROR:
        gn__ws_flush(&c->ws);
        fail_conn(c, c->ws.error, c->ws.error_message);
        return;
    default:
        break;
    }
    if (r == 0) {
        /* client closed TCP without a close frame */
        end_conn(c, c->state == CONN_CLOSING ? c->close_code : 1006);
        return;
    }
    if (!gn__ws_flush(&c->ws)) { /* pongs queued while parsing */
        fail_conn(c, GN_ERR_SOCKET, "connection lost while sending");
        return;
    }
    if (c->state == CONN_CLOSING && gn__past(c->deadline)) {
        end_conn(c, c->close_code);
    }
}

void gn_server_poll(gn_server *s)
{
    size_t i;
    if (!s) {
        return;
    }
    if (s->listener != GN_BAD_SOCK) {
        accept_new(s);
    }
    /* Connections only join in accept_new and only leave in sweep, so the
     * list is stable while callbacks run. */
    for (i = 0; i < s->nconns; i++) {
        if (s->conns[i]->state != CONN_DEAD) {
            poll_conn(s->conns[i]);
        }
    }
    sweep(s);
}

#ifdef GN_FUZZING
/* Test hook: feed raw bytes to a server connection as if a client had sent
 * them, starting from the handshake (first byte odd) or an open connection. */
void gn__fuzz_server_feed(const uint8_t *data, size_t len);
void gn__fuzz_server_feed(const uint8_t *data, size_t len)
{
    gn_server *s = gn_server_create(NULL);
    gn_conn *c;
    if (!s || len < 2) {
        gn_server_destroy(s);
        return;
    }
    c = (gn_conn *)calloc(1, sizeof *c);
    s->conns = (gn_conn **)malloc(sizeof *s->conns);
    if (!c || !s->conns) {
        free(c);
        gn_server_destroy(s);
        return;
    }
    s->conns[0] = c;
    s->nconns = s->cap = 1;
    c->server = s;
    c->state = (data[0] & 1) ? CONN_OPEN : CONN_HANDSHAKE;
    gn__ws_init(&c->ws, GN_BAD_SOCK, 0, 1);
    if (gn__bytes_reserve(&c->ws.in, len - 1, GN_MAX_MESSAGE + 4096)) {
        static const gn_ws_events events = { on_binary, on_text };
        memcpy(c->ws.in.data, data + 1, len - 1);
        c->ws.in.len = len - 1;
        if (c->state == CONN_HANDSHAKE && handshake(c) == 1) {
            c->state = CONN_OPEN;
        }
        if (c->state == CONN_OPEN) {
            gn__ws_parse(&c->ws, &events, c);
        }
    }
    gn_server_destroy(s);
}
#endif
