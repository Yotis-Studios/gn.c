/* A gn.c server for interop tests (tests/interop.js, the gn.hml client).
 * Listens on 127.0.0.1 (port from argv, 0 = any), prints "READY <port>", then
 * logs events to stdout. Behaviour by netId of the received packet:
 *   1 echo   2 broadcast to the others   3 close the sender with 4002 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200112L
#endif
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
    ts.tv_sec = 0;
    ts.tv_nsec = (long)ms * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

static gn_server *server;

static void on_connect(void *u, gn_conn *c)
{
    (void)u;
    printf("CONNECT %u %s\n", gn_conn_id(c), gn_conn_path(c));
    fflush(stdout);
}

static void on_packet(void *u, gn_conn *c, gn_reader *r)
{
    static unsigned char buf[GN_MAX_PACKET];
    gn_writer w;
    gn_value v;
    (void)u;
    gn_writer_init(&w, buf, sizeof buf, r->net_id);
    while (gn_read(r, &v)) {
        gn_write_value(&w, &v);
    }
    if (r->net_id == 1) gn_conn_send_packet(c, &w);
    if (r->net_id == 2) gn_server_broadcast_packet(server, &w, c);
    if (r->net_id == 3) gn_conn_close(c, 4002);
}

static void on_disconnect(void *u, gn_conn *c, int code)
{
    (void)u;
    printf("DISCONNECT %u %d\n", gn_conn_id(c), code);
    fflush(stdout);
}

static void on_error(void *u, gn_conn *c, int err, const char *m)
{
    (void)u;
    printf("ERROR %u %d %s\n", c ? gn_conn_id(c) : 0, err, m);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    gn_server_callbacks cb;
    memset(&cb, 0, sizeof cb);
    cb.on_connect = on_connect;
    cb.on_packet = on_packet;
    cb.on_disconnect = on_disconnect;
    cb.on_error = on_error;
    server = gn_server_create(&cb);
    if (!server || gn_server_listen(server, "127.0.0.1", (uint16_t)(argc > 1 ? atoi(argv[1]) : 0)) != GN_OK) {
        printf("FAILED to listen\n");
        return 1;
    }
    printf("READY %u\n", gn_server_port(server));
    fflush(stdout);
    for (;;) {
        gn_server_poll(server);
        sleep_ms(1);
    }
}
