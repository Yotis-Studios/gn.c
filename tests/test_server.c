/* gn_server tests: gn.c clients against a gn.c server in one process, both
 * polled from the same loop. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200112L
#endif
#include "gn.h"

#include <stdio.h>
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

static int passed, failed;

#define CHECK(cond, msg)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            printf("  FAIL %s: %s (line %d)\n", name, msg, __LINE__); \
            failed++;                                                 \
            goto done;                                                \
        }                                                             \
    } while (0)

/* ---- server side ---- */

static gn_server *server;

static struct {
    int connects, disconnects, packets, errors;
    int last_code, last_error;
    gn_conn *last_conn;
    char last_path[64];
    int64_t last_int;
    int echo;          /* echo every packet back */
    int broadcast;     /* rebroadcast every packet to the other clients */
    int kick_code;     /* close the sender with this code (0 = no) */
    int close_all;     /* gn_server_close from inside on_packet */
} sv;

static void sv_connect(void *u, gn_conn *c)
{
    (void)u;
    sv.connects++;
    sv.last_conn = c;
    strncpy(sv.last_path, gn_conn_path(c), sizeof sv.last_path - 1);
    gn_conn_set_user(c, (void *)(size_t)(100 + gn_conn_id(c)));
}

static void sv_packet(void *u, gn_conn *c, gn_reader *r)
{
    gn_value v;
    static unsigned char buf[GN_MAX_PACKET];
    gn_writer w;
    (void)u;
    sv.packets++;
    sv.last_conn = c;
    gn_writer_init(&w, buf, sizeof buf, r->net_id);
    while (gn_read(r, &v)) {
        if (v.type <= GN_S32) sv.last_int = v.as.i;
        gn_write_value(&w, &v);
    }
    if (sv.echo) gn_conn_send_packet(c, &w);
    if (sv.broadcast) gn_server_broadcast_packet(server, &w, c);
    if (sv.kick_code) gn_conn_close(c, sv.kick_code);
    if (sv.close_all) gn_server_close(server);
}

static void sv_disconnect(void *u, gn_conn *c, int code)
{
    (void)u;
    sv.disconnects++;
    sv.last_code = code;
    sv.last_conn = c;
}

static void sv_error(void *u, gn_conn *c, int err, const char *m)
{
    (void)u;
    (void)c;
    (void)m;
    sv.errors++;
    sv.last_error = err;
}

/* ---- client side ---- */

#define NCLIENTS 3

typedef struct {
    gn_client *c;
    int connects, disconnects, packets, code;
    int64_t last_int;
    unsigned last_net_id;
} cl_state;

static cl_state cl[NCLIENTS];

static void cl_connect(void *u) { ((cl_state *)u)->connects++; }
static void cl_disconnect(void *u, int code)
{
    ((cl_state *)u)->disconnects++;
    ((cl_state *)u)->code = code;
}
static void cl_packet(void *u, gn_reader *r)
{
    cl_state *s = (cl_state *)u;
    gn_value v;
    s->packets++;
    s->last_net_id = r->net_id;
    if (gn_read(r, &v) && v.type <= GN_S32) s->last_int = v.as.i;
}

static void reset(void)
{
    int i;
    memset(&sv, 0, sizeof sv);
    for (i = 0; i < NCLIENTS; i++) {
        gn_client *c = cl[i].c;
        memset(&cl[i], 0, sizeof cl[i]);
        cl[i].c = c;
    }
}

static void pump(void)
{
    int i;
    gn_server_poll(server);
    for (i = 0; i < NCLIENTS; i++) gn_client_poll(cl[i].c);
}

/* polls everything until cond or ~5 s */
#define WAIT(cond)                                         \
    do {                                                   \
        int t_;                                            \
        for (t_ = 0; t_ < 5000 && !(cond); t_++) {         \
            pump();                                        \
            sleep_ms(1);                                   \
        }                                                  \
    } while (0)

static int send_int(int i, unsigned net_id, int64_t v)
{
    unsigned char buf[64];
    gn_writer w;
    gn_writer_init(&w, buf, sizeof buf, net_id);
    gn_write_int(&w, v);
    return gn_client_send_packet(cl[i].c, &w);
}

static int connect_all(int n, const char *path)
{
    int i, ok = 1;
    for (i = 0; i < n; i++) {
        if (gn_client_connect(cl[i].c, "127.0.0.1", gn_server_port(server), path) != GN_OK) return 0;
    }
    WAIT(sv.connects == n);
    for (i = 0; i < n; i++) {
        WAIT(cl[i].connects == 1);
        ok = ok && cl[i].connects == 1;
    }
    return ok && sv.connects == n;
}

static void close_all_clients(void)
{
    int i;
    for (i = 0; i < NCLIENTS; i++) gn_client_close(cl[i].c);
    WAIT(gn_server_connection_count(server) == 0 && gn_client_get_state(cl[0].c) == GN_DISCONNECTED &&
         gn_client_get_state(cl[1].c) == GN_DISCONNECTED && gn_client_get_state(cl[2].c) == GN_DISCONNECTED);
}

static void ok(const char *name)
{
    printf("  pass: %s\n", name);
    passed++;
}

/* ---- tests ---- */

static void test_echo(void)
{
    const char *name = "echo, path, id and user data";
    reset();
    sv.echo = 1;
    CHECK(connect_all(1, "/room/7"), "connect");
    CHECK(strcmp(sv.last_path, "/room/7") == 0, "path");
    CHECK(gn_conn_id(sv.last_conn) >= 1, "id");
    CHECK(gn_conn_get_user(sv.last_conn) == (void *)(size_t)(100 + gn_conn_id(sv.last_conn)), "user data");
    CHECK(gn_server_connection_count(server) == 1, "count");
    CHECK(send_int(0, 9, -77777) == GN_OK, "send");
    WAIT(cl[0].packets == 1);
    CHECK(cl[0].packets == 1 && cl[0].last_net_id == 9 && cl[0].last_int == -77777, "echo");
    gn_client_close(cl[0].c);
    WAIT(sv.disconnects == 1 && cl[0].disconnects == 1);
    CHECK(sv.disconnects == 1 && sv.last_code == 1000, "server sees 1000");
    CHECK(cl[0].disconnects == 1 && cl[0].code == 1000, "client sees 1000");
    CHECK(gn_server_connection_count(server) == 0, "count after close");
    ok(name);
done:
    close_all_clients();
}

static void test_broadcast(void)
{
    const char *name = "broadcast to everyone but the sender";
    reset();
    sv.broadcast = 1;
    CHECK(connect_all(3, NULL), "connect");
    CHECK(send_int(1, 5, 42) == GN_OK, "send");
    WAIT(cl[0].packets == 1 && cl[2].packets == 1);
    pump();
    CHECK(cl[0].packets == 1 && cl[0].last_int == 42, "client 0");
    CHECK(cl[2].packets == 1 && cl[2].last_int == 42, "client 2");
    CHECK(cl[1].packets == 0, "not the sender");
    ok(name);
done:
    close_all_clients();
}

static void test_kick(void)
{
    const char *name = "server closes a client with a custom code";
    reset();
    sv.kick_code = 4001;
    CHECK(connect_all(1, NULL), "connect");
    send_int(0, 1, 1);
    WAIT(cl[0].disconnects == 1 && sv.disconnects == 1);
    CHECK(cl[0].code == 4001, "client sees 4001");
    CHECK(sv.disconnects == 1 && sv.last_code == 4001, "server reports 4001 once");
    ok(name);
done:
    close_all_clients();
}

static void test_lost_client(void)
{
    const char *name = "client vanishing without a close frame is 1006";
    gn_client_callbacks cb;
    gn_client *tmp;
    reset();
    memset(&cb, 0, sizeof cb);
    tmp = gn_client_create(&cb);
    CHECK(gn_client_connect(tmp, "127.0.0.1", gn_server_port(server), NULL) == GN_OK, "connect");
    {
        int t;
        for (t = 0; t < 5000 && sv.connects == 0; t++) {
            gn_server_poll(server);
            gn_client_poll(tmp);
            sleep_ms(1);
        }
    }
    CHECK(sv.connects == 1, "connected");
    gn_client_destroy(tmp); /* drops the socket */
    WAIT(sv.disconnects == 1);
    CHECK(sv.last_code == 1006, "1006");
    ok(name);
done:
    close_all_clients();
}

static void test_server_close(void)
{
    const char *name = "gn_server_close from a callback closes everyone with 1001";
    reset();
    sv.close_all = 1;
    CHECK(connect_all(3, NULL), "connect");
    send_int(0, 1, 1);
    WAIT(sv.disconnects == 3 && cl[0].disconnects && cl[1].disconnects && cl[2].disconnects);
    CHECK(sv.disconnects == 3 && sv.last_code == 1001, "server: three 1001s");
    CHECK(cl[0].code == 1001 && cl[1].code == 1001 && cl[2].code == 1001, "clients see 1001");
    CHECK(gn_server_port(server) == 0, "not listening");
    CHECK(gn_server_connection_count(server) == 0, "empty");
    /* listening again works (port 0: under wine, the old port is held by
     * TIME_WAIT without SO_REUSEADDR, which gn deliberately skips on Windows) */
    CHECK(gn_server_listen(server, "127.0.0.1", 0) == GN_OK && gn_server_port(server) != 0, "relisten");
    ok(name);
done:
    close_all_clients();
}

static void test_many_and_large(void)
{
    const char *name = "large message with many packets, both directions";
    static unsigned char msg[4 * GN_MAX_PACKET];
    static char big[50001];
    size_t total = 0;
    int i;
    reset();
    sv.echo = 1;
    CHECK(connect_all(1, NULL), "connect");
    memset(big, 'q', 50000);
    for (i = 0; i < 4; i++) {
        gn_writer w;
        gn_writer_init(&w, msg + total, GN_MAX_PACKET, 3);
        gn_write_int(&w, i);
        gn_write_string(&w, big);
        total += gn_writer_finish(&w);
    }
    CHECK(gn_client_send(cl[0].c, msg, total) == GN_OK, "send");
    WAIT(cl[0].packets == 4);
    CHECK(sv.packets == 4 && cl[0].packets == 4 && cl[0].last_int == 3, "4 packets each way");
    ok(name);
done:
    close_all_clients();
}

static void test_bad_calls(void)
{
    const char *name = "argument and state checks";
    gn_server *other;
    reset();
    other = gn_server_create(NULL);
    CHECK(gn_server_listen(server, NULL, 0) == GN_ERR_STATE, "listen twice");
    CHECK(gn_server_listen(other, "127.0.0.1", gn_server_port(server)) == GN_ERR_SOCKET, "port in use");
    CHECK(gn_server_listen(other, "127.0.0.1", 0) == GN_OK && gn_server_port(other) != 0, "port 0");
    CHECK(gn_server_broadcast(server, "x", 1, NULL) == GN_OK, "broadcast to nobody");
    CHECK(gn_conn_send(NULL, "x", 1) == GN_ERR_ARG, "null conn");
    ok(name);
done:
    gn_server_destroy(other);
}

int main(void)
{
    gn_server_callbacks scb;
    int i;
    memset(&scb, 0, sizeof scb);
    scb.on_connect = sv_connect;
    scb.on_packet = sv_packet;
    scb.on_disconnect = sv_disconnect;
    scb.on_error = sv_error;
    server = gn_server_create(&scb);
    if (!server || gn_server_listen(server, "127.0.0.1", 0) != GN_OK) {
        printf("cannot start server\n");
        return 1;
    }
    for (i = 0; i < NCLIENTS; i++) {
        gn_client_callbacks cb;
        memset(&cb, 0, sizeof cb);
        cb.on_connect = cl_connect;
        cb.on_disconnect = cl_disconnect;
        cb.on_packet = cl_packet;
        cb.user = &cl[i];
        cl[i].c = gn_client_create(&cb);
    }

    test_echo();
    test_broadcast();
    test_kick();
    test_lost_client();
    test_many_and_large();
    test_bad_calls();
    test_server_close();

    for (i = 0; i < NCLIENTS; i++) gn_client_destroy(cl[i].c);
    gn_server_destroy(server);
    printf("Server tests: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
