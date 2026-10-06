#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200112L
#endif
/* Live tests for gn_client against a gn.js server (tests/server.js).
 * Usage: test_client <ws-port> <http-port> <closed-port> */
#include "gn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <time.h>
static void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

static int ws_port, http_port, closed_port;
static int passed, failed;

/* ---- recorded events ---- */

#define MAX_PACKETS 4096

typedef struct {
    unsigned net_id;
    int count;
    gn_value values[8];
    char strings[8][128]; /* copies: readers die with the callback */
} rec_packet;

static struct {
    int connects;
    int disconnects;
    int disconnect_code;
    int errors;
    int last_error;
    int npackets;
    rec_packet packets[MAX_PACKETS];
    int reconnect_on_disconnect;
} ev;

static gn_client *client;

static void on_connect(void *u) { (void)u; ev.connects++; }

static void on_packet(void *u, gn_reader *r)
{
    rec_packet *p;
    gn_value v;
    (void)u;
    if (ev.npackets >= MAX_PACKETS) return;
    p = &ev.packets[ev.npackets++];
    memset(p, 0, sizeof *p);
    p->net_id = r->net_id;
    while (gn_read(r, &v) && p->count < 8) {
        if (v.type == GN_STRING) {
            size_t n = v.as.str.len < 127 ? v.as.str.len : 127;
            memcpy(p->strings[p->count], v.as.str.ptr, n);
            p->strings[p->count][n] = 0;
        }
        p->values[p->count++] = v;
    }
}

static void on_disconnect(void *u, int code)
{
    (void)u;
    ev.disconnects++;
    ev.disconnect_code = code;
    if (ev.reconnect_on_disconnect) {
        ev.reconnect_on_disconnect = 0;
        gn_client_connect(client, "127.0.0.1", (uint16_t)ws_port, "/");
    }
}

static void on_error(void *u, int err, const char *msg)
{
    (void)u;
    (void)msg;
    ev.errors++;
    ev.last_error = err;
}

static void reset(void) { memset(&ev, 0, sizeof ev); }

static int poll_until(int (*cond)(void), int timeout_ms)
{
    int waited = 0;
    while (!cond()) {
        if (waited >= timeout_ms) return 0;
        gn_client_poll(client);
        sleep_ms(1);
        waited++;
    }
    return 1;
}

static int want_packets;
static int got_packets(void) { return ev.npackets >= want_packets; }
static int got_connect(void) { return ev.connects > 0; }
static int got_disconnect(void) { return ev.disconnects > 0; }

#define CHECK(cond, msg)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            printf("  FAIL %s: %s (line %d)\n", name, msg, __LINE__); \
            failed++;                                            \
            return;                                              \
        }                                                        \
    } while (0)

static void ok(const char *name)
{
    printf("  pass: %s\n", name);
    passed++;
}

static int connect_open(void)
{
    reset();
    if (gn_client_connect(client, "127.0.0.1", (uint16_t)ws_port, NULL) != GN_OK) return 0;
    return poll_until(got_connect, 5000) && gn_client_get_state(client) == GN_OPEN;
}

static int send_simple(unsigned net_id)
{
    unsigned char buf[64];
    gn_writer w;
    gn_writer_init(&w, buf, sizeof buf, net_id);
    return gn_client_send_packet(client, &w);
}

static void close_and_wait(void)
{
    ev.disconnects = 0;
    gn_client_close(client);
    poll_until(got_disconnect, 3000);
}

/* ---- tests ---- */

static void test_echo_all_types(void)
{
    const char *name = "echo every value type";
    unsigned char buf[256];
    unsigned char raw[3] = { 1, 2, 255 };
    gn_writer w;
    rec_packet *p;
    CHECK(connect_open(), "connect");
    gn_writer_init(&w, buf, sizeof buf, 1);
    gn_write_int(&w, 200);
    gn_write_int(&w, -40000);
    gn_write_uint(&w, 4000000000u);
    gn_write_double(&w, 1.5);
    gn_write_double(&w, 1e10 + 0.5); /* gn.js re-encodes integral doubles as ints */
    gn_write_string(&w, "héllo");
    gn_write_buffer(&w, raw, 3);
    gn_write_undefined(&w);
    CHECK(gn_client_send_packet(client, &w) == GN_OK, "send");
    want_packets = 1;
    CHECK(poll_until(got_packets, 5000), "no echo");
    p = &ev.packets[0];
    CHECK(p->net_id == 1 && p->count == 8, "netId/count");
    CHECK(p->values[0].type == GN_U8 && p->values[0].as.i == 200, "u8");
    CHECK(p->values[1].type == GN_S32 && p->values[1].as.i == -40000, "s32");
    CHECK(p->values[2].type == GN_U32 && p->values[2].as.i == 4000000000LL, "u32");
    CHECK(p->values[3].type == GN_F32 && p->values[3].as.f == 1.5, "f32");
    CHECK(p->values[4].type == GN_F64 && p->values[4].as.f == 1e10 + 0.5, "f64");
    CHECK(p->values[5].type == GN_STRING && strcmp(p->strings[5], "héllo") == 0, "string");
    CHECK(p->values[6].type == GN_BUFFER && p->values[6].as.buf.len == 3, "buffer");
    CHECK(p->values[7].type == GN_UNDEFINED, "undefined");
    close_and_wait();
    CHECK(ev.disconnects == 1 && ev.disconnect_code == 1000, "clean close");
    CHECK(gn_client_get_state(client) == GN_DISCONNECTED, "state after close");
    ok(name);
}

static void test_multi_packet_message(void)
{
    const char *name = "two packets in one message";
    CHECK(connect_open(), "connect");
    send_simple(2);
    want_packets = 2;
    CHECK(poll_until(got_packets, 5000), "packets");
    CHECK(ev.packets[0].net_id == 21 && strcmp(ev.packets[0].strings[0], "first") == 0, "first");
    CHECK(ev.packets[1].net_id == 22 && strcmp(ev.packets[1].strings[0], "second") == 0, "second");
    close_and_wait();
    ok(name);
}

static void test_client_sends_many_packets(void)
{
    const char *name = "client sends several large packets in one message";
    static unsigned char msg[3 * GN_MAX_PACKET];
    static char big[60001];
    size_t total = 0;
    int i;
    CHECK(connect_open(), "connect");
    memset(big, 'z', 60000);
    big[60000] = 0;
    for (i = 0; i < 3; i++) {
        gn_writer w;
        gn_writer_init(&w, msg + total, GN_MAX_PACKET, 1);
        gn_write_int(&w, i);
        gn_write_string(&w, big);
        total += gn_writer_finish(&w);
    }
    CHECK(total > 65536, "message should need a 64-bit frame length");
    CHECK(gn_client_send(client, msg, total) == GN_OK, "send");
    want_packets = 3;
    CHECK(poll_until(got_packets, 5000), "echoes");
    for (i = 0; i < 3; i++) {
        CHECK(ev.packets[i].values[0].as.i == i, "order");
        CHECK(ev.packets[i].values[1].as.str.len == 60000, "60000-byte string");
    }
    close_and_wait();
    ok(name);
}

static void test_text_message(void)
{
    const char *name = "text message is reported, connection stays open";
    CHECK(connect_open(), "connect");
    send_simple(3);
    want_packets = 1;
    CHECK(poll_until(got_packets, 5000), "packet after text");
    CHECK(ev.errors == 1 && ev.last_error == GN_ERR_MALFORMED, "error reported");
    CHECK(gn_client_get_state(client) == GN_OPEN, "still open");
    close_and_wait();
    ok(name);
}

static void test_ping(void)
{
    const char *name = "answers server ping";
    CHECK(connect_open(), "connect");
    send_simple(5);
    want_packets = 1;
    CHECK(poll_until(got_packets, 5000), "pong ack");
    CHECK(strcmp(ev.packets[0].strings[0], "gn-ping") == 0, "pong payload");
    close_and_wait();
    ok(name);
}

static void test_large_message(void)
{
    const char *name = "2000 packets in one 100 KB message";
    int i;
    CHECK(connect_open(), "connect");
    send_simple(6);
    want_packets = 2000;
    CHECK(poll_until(got_packets, 5000), "all packets");
    for (i = 0; i < 2000; i++) {
        CHECK(ev.packets[i].net_id == 6 && ev.packets[i].values[0].as.i == i, "order");
    }
    close_and_wait();
    ok(name);
}

static void test_fragmented(void)
{
    const char *name = "fragmented message is reassembled";
    CHECK(connect_open(), "connect");
    send_simple(7);
    want_packets = 1;
    CHECK(poll_until(got_packets, 5000), "packet");
    CHECK(strcmp(ev.packets[0].strings[0], "fragmented") == 0, "string");
    CHECK(ev.packets[0].values[1].as.i == 123456, "int");
    close_and_wait();
    ok(name);
}

static void test_server_close(void)
{
    const char *name = "server close code is reported";
    CHECK(connect_open(), "connect");
    send_simple(4);
    CHECK(poll_until(got_disconnect, 5000), "disconnect");
    CHECK(ev.disconnect_code == 4000, "code 4000");
    CHECK(ev.disconnects == 1, "fires once");
    CHECK(gn_client_send(client, "x", 1) == GN_ERR_STATE, "send after close refused");
    ok(name);
}

static void test_reconnect_from_callback(void)
{
    const char *name = "reconnect from inside on_disconnect";
    CHECK(connect_open(), "connect");
    ev.reconnect_on_disconnect = 1;
    send_simple(4);
    CHECK(poll_until(got_disconnect, 5000), "disconnect");
    ev.connects = 0;
    CHECK(poll_until(got_connect, 5000), "reconnected");
    send_simple(1);
    want_packets = 1;
    CHECK(poll_until(got_packets, 5000), "echo after reconnect");
    close_and_wait();
    ok(name);
}

static void test_refused(void)
{
    const char *name = "connection refused";
    reset();
    CHECK(gn_client_connect(client, "127.0.0.1", (uint16_t)closed_port, NULL) == GN_OK, "connect starts");
    CHECK(poll_until(got_disconnect, 5000), "disconnect");
    CHECK(ev.errors == 1 && ev.last_error == GN_ERR_SOCKET, "socket error");
    CHECK(ev.disconnect_code == 1006 && ev.connects == 0, "1006, never connected");
    ok(name);
}

static void test_handshake_rejected(void)
{
    const char *name = "non-WebSocket server is rejected";
    reset();
    CHECK(gn_client_connect(client, "127.0.0.1", (uint16_t)http_port, NULL) == GN_OK, "connect starts");
    CHECK(poll_until(got_disconnect, 5000), "disconnect");
    CHECK(ev.last_error == GN_ERR_HANDSHAKE, "handshake error");
    CHECK(ev.connects == 0, "never connected");
    ok(name);
}

static void test_close_while_connecting(void)
{
    const char *name = "close while connecting";
    reset();
    CHECK(gn_client_connect(client, "127.0.0.1", (uint16_t)ws_port, NULL) == GN_OK, "connect starts");
    gn_client_close(client);
    CHECK(poll_until(got_disconnect, 3000), "disconnect");
    CHECK(ev.connects == 0 && ev.errors == 0 && ev.disconnect_code == 1000, "clean, no connect");
    ok(name);
}

static void test_bad_calls(void)
{
    const char *name = "argument and state checks";
    reset();
    CHECK(gn_client_send(client, "x", 1) == GN_ERR_STATE, "send while disconnected");
    CHECK(gn_client_connect(client, "", 1, NULL) == GN_ERR_ARG, "empty host");
    CHECK(gn_client_connect(client, "127.0.0.1", 1, "no-slash") == GN_ERR_ARG, "bad path");
    CHECK(gn_client_connect(client, "no-such-host.invalid", 1, NULL) == GN_ERR_SOCKET, "dns failure");
    CHECK(ev.disconnects == 0 && ev.errors == 0, "immediate failures fire no callbacks");
    CHECK(connect_open(), "connect");
    CHECK(gn_client_connect(client, "127.0.0.1", (uint16_t)ws_port, NULL) == GN_ERR_STATE, "connect while open");
    close_and_wait();
    ok(name);
}

int main(int argc, char **argv)
{
    gn_client_callbacks cb;
    if (argc < 4) {
        printf("usage: test_client <ws-port> <http-port> <closed-port>\n");
        return 2;
    }
    ws_port = atoi(argv[1]);
    http_port = atoi(argv[2]);
    closed_port = atoi(argv[3]);
    cb.on_connect = on_connect;
    cb.on_packet = on_packet;
    cb.on_disconnect = on_disconnect;
    cb.on_error = on_error;
    cb.user = NULL;
    client = gn_client_create(&cb);
    if (!client) {
        printf("gn_client_create failed\n");
        return 1;
    }

    test_echo_all_types();
    test_multi_packet_message();
    test_client_sends_many_packets();
    test_text_message();
    test_ping();
    test_large_message();
    test_fragmented();
    test_server_close();
    test_reconnect_from_callback();
    test_refused();
    test_handshake_rejected();
    test_close_while_connecting();
    test_bad_calls();

    gn_client_destroy(client);
    printf("Client tests: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
