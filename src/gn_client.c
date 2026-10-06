/* gn_client.c - non-blocking WebSocket client for gn
 *
 * Everything happens in gn_client_poll(): connect progress, the HTTP upgrade,
 * frames, ping/pong and the close handshake. The only blocking call is name
 * resolution in gn_client_connect(). */
#include "gn_internal.h"

enum { PHASE_TCP, PHASE_HANDSHAKE, PHASE_WS };

struct gn_client {
    gn_client_callbacks cb;
    gn_client_state state;
    int phase;
    gn_ws ws;
    struct addrinfo *addrs; /* remaining candidates for the TCP connect */
    struct addrinfo *addr;
    char *request;          /* HTTP upgrade request, sent once TCP connects */
    char accept[32];        /* expected Sec-WebSocket-Accept */
    uint64_t rng;
    uint32_t deadline;      /* connect or close timeout (ms clock) */
    int close_code;
    int failed;             /* fatal error noticed outside poll */
    int failed_err;
    int net;
};

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
/* Lifecycle                                                              */
/* ====================================================================== */

gn_client *gn_client_create(const gn_client_callbacks *callbacks)
{
    gn_client *c = (gn_client *)calloc(1, sizeof *c);
    if (!c) {
        return NULL;
    }
    if (!gn__net_init()) {
        free(c);
        return NULL;
    }
    c->net = 1;
    if (callbacks) {
        c->cb = *callbacks;
    }
    c->rng = gn__seed(c);
    gn__ws_init(&c->ws, GN_BAD_SOCK, 1, gn__random(&c->rng));
    c->state = GN_DISCONNECTED;
    return c;
}

/* Releases the socket and per-connection buffers; no callbacks. */
static void teardown(gn_client *c)
{
    gn__ws_reset(&c->ws);
    if (c->addrs) {
        freeaddrinfo(c->addrs);
        c->addrs = NULL;
        c->addr = NULL;
    }
    free(c->request);
    c->request = NULL;
    c->failed = 0;
    c->state = GN_DISCONNECTED;
}

void gn_client_destroy(gn_client *c)
{
    if (!c) {
        return;
    }
    teardown(c);
    if (c->net) {
        gn__net_done();
    }
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
        if (c->ws.fd != GN_BAD_SOCK) {
            gn_closesocket(c->ws.fd);
        }
        c->ws.fd = socket(c->addr->ai_family, c->addr->ai_socktype, c->addr->ai_protocol);
        if (c->ws.fd == GN_BAD_SOCK) {
            continue;
        }
        if (!gn__set_nonblocking(c->ws.fd)) {
            continue;
        }
        gn__tune_socket(c->ws.fd);
        r = connect(c->ws.fd, c->addr->ai_addr, (int)c->addr->ai_addrlen);
        if (r == 0 || gn__would_block()) {
            return 1;
        }
    }
    return 0;
}

int gn_client_connect(gn_client *c, const char *host, uint16_t port, const char *path)
{
    struct addrinfo hints;
    char port_str[8], *p;
    uint8_t nonce[16];
    char key[32];
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
    gn__append_uint(&p, port);
    *p = 0;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(host, port_str, &hints, &c->addrs) != 0 || !c->addrs) {
        c->addrs = NULL;
        return GN_ERR_SOCKET;
    }
    gn__ws_init(&c->ws, GN_BAD_SOCK, 1, gn__random(&c->rng));
    c->addr = c->addrs;
    if (!start_tcp(c)) {
        teardown(c);
        return GN_ERR_SOCKET;
    }

    /* Handshake: random 16-byte key, and the accept value we expect back. */
    for (i = 0; i < 16; i += 8) {
        uint64_t r = gn__random(&c->rng);
        memcpy(nonce + i, &r, 8);
    }
    gn__base64(nonce, 16, key);
    gn__accept_key(key, strlen(key), c->accept);

    c->request = (char *)malloc(strlen(host) + strlen(path) + 256);
    if (!c->request) {
        teardown(c);
        return GN_ERR_MEMORY;
    }
    p = c->request;
    gn__append_str(&p, "GET ");
    gn__append_str(&p, path);
    gn__append_str(&p, " HTTP/1.1\r\nHost: ");
    gn__append_str(&p, host);
    gn__append_str(&p, ":");
    gn__append_uint(&p, port);
    gn__append_str(&p, "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ");
    gn__append_str(&p, key);
    gn__append_str(&p, "\r\nSec-WebSocket-Version: 13\r\n\r\n");
    *p = 0;

    c->state = GN_CONNECTING;
    c->phase = PHASE_TCP;
    c->deadline = gn__now_ms() + GN_CONNECT_TIMEOUT_MS;
    return GN_OK;
}

/* ====================================================================== */
/* Sending                                                                */
/* ====================================================================== */

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
    if (!gn__ws_queue_frame(&c->ws, 0x2, (const uint8_t *)data, len)) {
        return GN_ERR_NO_SPACE;
    }
    if (!gn__ws_flush(&c->ws)) {
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
    gn__ws_queue_close(&c->ws, 1000);
    c->state = GN_CLOSING;
    c->close_code = 1000;
    c->deadline = gn__now_ms() + GN_CLOSE_TIMEOUT_MS;
    if (!gn__ws_flush(&c->ws)) {
        c->failed = 1;
        c->failed_err = GN_ERR_SOCKET;
    }
}

/* ====================================================================== */
/* Receiving                                                              */
/* ====================================================================== */

/* 1 = handshake done, 0 = need more bytes, -1 = rejected */
static int parse_handshake(gn_client *c)
{
    const char *s = (const char *)c->ws.in.data, *v;
    size_t header_len = gn__header_end(c->ws.in.data, c->ws.in.len), vlen;
    size_t alen = strlen(c->accept);
    if (!header_len) {
        return c->ws.in.len > GN_MAX_HANDSHAKE ? -1 : 0;
    }
    if (header_len < 12 || memcmp(s, "HTTP/1.1 101", 12) != 0) {
        return -1;
    }
    v = gn__header_value(s, header_len, "upgrade:", &vlen);
    if (!v || !gn__contains_ci(v, vlen, "websocket")) {
        return -1;
    }
    v = gn__header_value(s, header_len, "connection:", &vlen);
    if (!v || !gn__contains_ci(v, vlen, "upgrade")) {
        return -1;
    }
    v = gn__header_value(s, header_len, "sec-websocket-accept:", &vlen);
    if (!v || vlen != alen || memcmp(v, c->accept, alen) != 0) {
        return -1;
    }
    gn__bytes_consume(&c->ws.in, header_len);
    return 1;
}

/* Delivers each packet in a binary message. 0 if the connection ended
 * inside a callback. */
static int on_binary(void *ctx, const uint8_t *data, size_t len)
{
    gn_client *c = (gn_client *)ctx;
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

static int on_text(void *ctx)
{
    gn_client *c = (gn_client *)ctx;
    report_error(c, GN_ERR_MALFORMED, "received a text message (gn uses binary messages)");
    return c->state == GN_OPEN || c->state == GN_CLOSING;
}

/* 0 if the connection ended (callbacks already fired). */
static int parse_frames(gn_client *c)
{
    static const gn_ws_events events = { on_binary, on_text };
    switch (gn__ws_parse(&c->ws, &events, c)) {
    case GN_WS_OK:
        return 1;
    case GN_WS_STOPPED:
        return 0;
    case GN_WS_CLOSED:
        gn__ws_flush(&c->ws);
        finish(c, c->state == GN_CLOSING ? c->close_code : c->ws.peer_close_code);
        return 0;
    default: /* GN_WS_ERROR */
        gn__ws_flush(&c->ws);
        fail(c, c->ws.error, c->ws.error_message);
        return 0;
    }
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
        r = connect_status(c->ws.fd);
        if (r < 0 || (r == 0 && gn__past(c->deadline))) {
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
        if (!gn__ws_queue(&c->ws, c->request, strlen(c->request))) {
            fail(c, GN_ERR_MEMORY, "out of memory");
            return;
        }
        free(c->request);
        c->request = NULL;
        c->phase = PHASE_HANDSHAKE;
    }

    if (!gn__ws_flush(&c->ws)) {
        fail(c, GN_ERR_SOCKET, "connection lost while sending");
        return;
    }

    r = gn__ws_receive(&c->ws);
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
            } else if (gn__past(c->deadline)) {
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
    if (!gn__ws_flush(&c->ws)) { /* pongs queued while parsing */
        fail(c, GN_ERR_SOCKET, "connection lost while sending");
        return;
    }
    if (c->state == GN_CLOSING && gn__past(c->deadline)) {
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
    if (gn__bytes_reserve(&c->ws.in, len - 1, GN_MAX_MESSAGE + 4096)) {
        memcpy(c->ws.in.data, data + 1, len - 1);
        c->ws.in.len = len - 1;
        if (c->phase == PHASE_HANDSHAKE && parse_handshake(c) == 1) {
            c->phase = PHASE_WS;
            c->state = GN_OPEN;
        }
        if (c->phase == PHASE_WS && c->state == GN_OPEN) {
            parse_frames(c);
        }
    }
    gn_client_destroy(c);
}
#endif
