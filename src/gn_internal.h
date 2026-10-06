/* gn_internal.h - shared WebSocket plumbing for gn_client.c and gn_server.c.
 * Not part of the public API. */
#ifndef GN_INTERNAL_H
#define GN_INTERNAL_H

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

/* Largest WebSocket message accepted (a message may hold many packets). */
#define GN_MAX_MESSAGE (16u * 1024u * 1024u)
/* Largest unsent backlog per connection before sends are refused. */
#define GN_MAX_BACKLOG (16u * 1024u * 1024u)
#define GN_MAX_HANDSHAKE 8192u
#define GN_CLOSE_TIMEOUT_MS 2000u
#define GN_CONNECT_TIMEOUT_MS 10000u

/* ---- platform ---- */

int gn__net_init(void); /* WSAStartup on Windows; 1 on success */
void gn__net_done(void);
uint32_t gn__now_ms(void);
int gn__past(uint32_t deadline_ms);
int gn__would_block(void);
int gn__set_nonblocking(gn_sock fd);
void gn__tune_socket(gn_sock fd);

/* ---- helpers ---- */

typedef struct gn_bytes {
    uint8_t *data;
    size_t len;
    size_t cap;
} gn_bytes;

int gn__bytes_reserve(gn_bytes *b, size_t extra, size_t limit);
void gn__bytes_consume(gn_bytes *b, size_t n);
void gn__bytes_free(gn_bytes *b);

uint64_t gn__seed(const void *salt);
uint64_t gn__random(uint64_t *state);
int gn__contains_ci(const char *s, size_t len, const char *needle);
void gn__append_str(char **p, const char *s);
void gn__append_uint(char **p, unsigned v);
void gn__base64(const uint8_t *in, size_t len, char *out);
/* Sec-WebSocket-Accept for a Sec-WebSocket-Key (key_len bytes). out: >= 29 */
void gn__accept_key(const char *key, size_t key_len, char *out);
/* Finds the end of an HTTP header block; 0 if not complete yet. */
size_t gn__header_end(const uint8_t *data, size_t len);
/* Value of header `name` (lowercase, with ':'), trimmed; NULL if absent. */
const char *gn__header_value(const char *headers, size_t len, const char *name, size_t *value_len);

/* ---- one WebSocket connection (either side) ---- */

typedef struct gn_ws {
    gn_sock fd;
    int is_client;      /* clients mask what they send; servers require masking */
    gn_bytes out;       /* bytes waiting to be sent */
    size_t out_pos;
    gn_bytes in;        /* bytes received, not yet parsed */
    gn_bytes msg;       /* fragmented message being assembled */
    int msg_opcode;     /* 0 when not inside a fragmented message */
    uint64_t rng;
    int close_sent;
    int peer_close_code; /* from the peer's close frame */
    int error;           /* gn_error after GN_WS_ERROR */
    const char *error_message;
} gn_ws;

void gn__ws_init(gn_ws *ws, gn_sock fd, int is_client, uint64_t seed);
/* Closes the socket and frees buffers. */
void gn__ws_reset(gn_ws *ws);

int gn__ws_queue(gn_ws *ws, const void *data, size_t len);
int gn__ws_queue_frame(gn_ws *ws, int opcode, const uint8_t *payload, size_t len);
void gn__ws_queue_close(gn_ws *ws, int code);
/* Writes as much of the backlog as the socket takes. 0 on a fatal error. */
int gn__ws_flush(gn_ws *ws);
/* Reads everything available. 1 = ok, 0 = peer closed TCP, -1 = error. */
int gn__ws_receive(gn_ws *ws);

/* Frame size for a payload, and writing one into dst (unmasked: server). */
size_t gn__ws_frame_size(size_t len, int masked);
void gn__ws_write_frame(uint8_t *dst, int opcode, const uint8_t *payload, size_t len, const uint8_t *mask);

typedef struct gn_ws_events {
    /* Return 0 if the connection ended inside the callback: parsing stops and
     * the gn_ws must not be touched again. */
    int (*on_binary)(void *ctx, const uint8_t *data, size_t len);
    int (*on_text)(void *ctx);
} gn_ws_events;

enum {
    GN_WS_OK = 0,      /* parsed everything available */
    GN_WS_STOPPED = 1, /* a callback ended the connection */
    GN_WS_CLOSED = 2,  /* peer sent close (peer_close_code); reply queued */
    GN_WS_ERROR = 3    /* protocol violation (error, error_message); close queued */
};

int gn__ws_parse(gn_ws *ws, const gn_ws_events *ev, void *ctx);

#endif
