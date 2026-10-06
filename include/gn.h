/*
 * gn.c - C implementation of the gn binary protocol
 * https://github.com/Yotis-Studios/gn.js/blob/main/PROTOCOL.md
 *
 * Two independent parts:
 *
 *   Codec   gn_writer / gn_reader / gn_frames: build and parse packets in
 *           memory. No allocation, no I/O. Usable with any transport.
 *
 *   Client  gn_client: a non-blocking WebSocket (ws://) client. Call
 *           gn_client_poll() once per frame; callbacks fire from inside it,
 *           on the calling thread. No threads are created.
 *
 * C99. Builds as 32- or 64-bit on Windows (Winsock, link ws2_32), Linux and
 * macOS. Not thread-safe: use each gn_client from one thread.
 */
#ifndef GN_H
#define GN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================== */
/* Protocol                                                              */
/* ===================================================================== */

typedef enum gn_type {
    GN_U8 = 0,
    GN_U16 = 1,
    GN_U32 = 2,
    GN_S8 = 3,
    GN_S16 = 4,
    GN_S32 = 5,
    GN_F16 = 6,
    GN_F32 = 7,
    GN_F64 = 8,
    GN_STRING = 9,
    GN_BUFFER = 10,
    GN_UNDEFINED = 11
} gn_type;

#define GN_TYPE_COUNT 12

/* Largest possible packet on the wire: 2-byte size + 65535-byte payload. */
#define GN_MAX_PACKET 65537
/* Longest string/buffer payload a value can carry. */
#define GN_MAX_STRING 65534
#define GN_MAX_BUFFER 255

typedef enum gn_error {
    GN_OK = 0,
    GN_ERR_RANGE = 1,     /* integer or netId does not fit its field      */
    GN_ERR_TOO_LARGE = 2, /* string/buffer/payload exceeds its length field */
    GN_ERR_NO_SPACE = 3,  /* the caller's output buffer is full           */
    GN_ERR_MALFORMED = 4, /* truncated value, bad length or unknown type   */
    GN_ERR_ARG = 5,       /* NULL pointer or invalid argument             */
    GN_ERR_STATE = 6,     /* client is not in a state that allows this    */
    GN_ERR_SOCKET = 7,    /* socket/DNS failure                           */
    GN_ERR_HANDSHAKE = 8, /* server rejected the WebSocket upgrade        */
    GN_ERR_MEMORY = 9     /* allocation failed                            */
} gn_error;

const char *gn_error_string(int err);
const char *gn_type_name(gn_type type);

/* A decoded value. Strings and buffers point into the packet they were read
 * from and are only valid while that memory is. Strings are UTF-8 and NOT
 * NUL-terminated: use len. */
typedef struct gn_value {
    gn_type type;
    union {
        int64_t i;   /* GN_U8 .. GN_S32                  */
        double f;    /* GN_F16, GN_F32, GN_F64          */
        struct {
            const char *ptr;
            size_t len;
        } str;       /* GN_STRING                        */
        struct {
            const uint8_t *ptr;
            size_t len;
        } buf;       /* GN_BUFFER                        */
    } as;
} gn_value;

/* Helpers for reading a value as a C type. Each returns 1 on success, 0 if
 * the value is not of a compatible type (out is left unchanged). */
int gn_value_int(const gn_value *v, int64_t *out);   /* integer types only */
int gn_value_double(const gn_value *v, double *out); /* any numeric type   */
int gn_value_bool(const gn_value *v, int *out);      /* any numeric: != 0  */

/* ===================================================================== */
/* Writing packets                                                       */
/* ===================================================================== */

/* Builds one packet into a caller-supplied buffer. Errors are sticky: after
 * the first failure every call is a no-op and gn_writer_finish() returns 0,
 * so you can write all values and check once at the end. */
typedef struct gn_writer {
    uint8_t *data;
    size_t cap;
    size_t len;
    int err;
} gn_writer;

/* cap of GN_MAX_PACKET always suffices. net_id must be 0-65535 (it is wider
 * than 16 bits so an out-of-range id is an error, GN_ERR_RANGE, rather than a
 * silent truncation). */
void gn_writer_init(gn_writer *w, void *buffer, size_t cap, uint32_t net_id);

/* Writers that pick the smallest wire type, exactly like gn.js. */
int gn_write_int(gn_writer *w, int64_t v);   /* u8..u32 / s8..s32 or GN_ERR_RANGE */
int gn_write_uint(gn_writer *w, uint64_t v);
int gn_write_double(gn_writer *w, double v); /* f32 if |v| <= 2^24, else f64 */
int gn_write_bool(gn_writer *w, int v);      /* u8 1/0 */
int gn_write_string(gn_writer *w, const char *s);              /* NUL-terminated */
int gn_write_stringn(gn_writer *w, const char *s, size_t len); /* exact bytes    */
int gn_write_buffer(gn_writer *w, const void *data, size_t len);
int gn_write_undefined(gn_writer *w);

/* Write a value with exactly the type it carries (e.g. force a u32, or send
 * a value you decoded). Integers must fit the type; GN_F16 is not supported
 * for writing (GN_ERR_ARG). */
int gn_write_value(gn_writer *w, const gn_value *v);

/* Fills in the size header. Returns the packet length in bytes (send
 * w->data[0..len)) or 0 if any write failed; see w->err. */
size_t gn_writer_finish(gn_writer *w);

/* ===================================================================== */
/* Reading packets                                                       */
/* ===================================================================== */

typedef struct gn_reader {
    const uint8_t *data; /* payload: the bytes after the size field */
    size_t len;
    size_t pos;
    int has_net_id;      /* 0 if the payload is shorter than 2 bytes */
    uint16_t net_id;
    int err;             /* GN_ERR_MALFORMED if reading stopped early */
} gn_reader;

void gn_reader_init(gn_reader *r, const void *payload, size_t len);

/* Reads the next value. Returns 1 and fills *out, or 0 at the end of the
 * packet. Malformed data also returns 0, with r->err = GN_ERR_MALFORMED;
 * values read before it remain valid. */
int gn_read(gn_reader *r, gn_value *out);

/* ===================================================================== */
/* Splitting WebSocket messages into packets                             */
/* ===================================================================== */

typedef struct gn_frames {
    const uint8_t *data;
    size_t len;
    size_t pos;
} gn_frames;

void gn_frames_init(gn_frames *f, const void *message, size_t len);

/* Returns 1 and the next packet's payload, or 0 when no complete packet
 * remains (end of message, zero size, or size past the end). */
int gn_frames_next(gn_frames *f, const uint8_t **payload, size_t *len);

/* ===================================================================== */
/* WebSocket client                                                      */
/* ===================================================================== */

typedef enum gn_client_state {
    GN_DISCONNECTED = 0,
    GN_CONNECTING = 1, /* TCP connect or WebSocket handshake in progress */
    GN_OPEN = 2,
    GN_CLOSING = 3     /* close requested, waiting for the server */
} gn_client_state;

typedef struct gn_client gn_client;

typedef struct gn_client_callbacks {
    /* Handshake completed; gn_client_send() may be used. */
    void (*on_connect)(void *user);
    /* One packet. The reader (and strings/buffers read from it) is only
     * valid during the callback. */
    void (*on_packet)(void *user, gn_reader *packet);
    /* The connection ended, or a connection attempt failed. Fires exactly
     * once for each gn_client_connect() that returned GN_OK (one that fails
     * immediately fires nothing). code is the WebSocket close code
     * (1000 = normal), or 1006 if the connection was lost without one. It is
     * safe to call gn_client_connect() again from inside this callback. */
    void (*on_disconnect)(void *user, int code);
    /* Something went wrong. err is a gn_error, message is human-readable.
     * Fatal errors are followed by on_disconnect. */
    void (*on_error)(void *user, int err, const char *message);
    void *user;
} gn_client_callbacks;

/* callbacks may be NULL; individual function pointers may be NULL. */
gn_client *gn_client_create(const gn_client_callbacks *callbacks);

/* Not from inside a callback. Closes the socket without a close handshake
 * if still connected (no callbacks fire). */
void gn_client_destroy(gn_client *c);

/* Starts connecting to ws://host:port/path (path NULL = "/"). Returns
 * immediately; progress happens in gn_client_poll(). Name resolution is
 * blocking: pass an IP address to avoid any stall. */
int gn_client_connect(gn_client *c, const char *host, uint16_t port, const char *path);

/* Does all pending I/O and dispatches callbacks. Never blocks. Call it
 * regularly (e.g. once per frame) while not GN_DISCONNECTED. */
void gn_client_poll(gn_client *c);

/* Queues one WebSocket message containing packet bytes (one or more packets
 * from gn_writer_finish, concatenated). Sent as soon as the socket allows,
 * including from within this call. Only while GN_OPEN. */
int gn_client_send(gn_client *c, const void *data, size_t len);

/* Convenience: finish the writer and send its packet. */
int gn_client_send_packet(gn_client *c, gn_writer *w);

/* Starts the close handshake. on_disconnect fires from a later poll once the
 * server answers (or after a timeout). */
void gn_client_close(gn_client *c);

gn_client_state gn_client_get_state(const gn_client *c);

#ifdef __cplusplus
}
#endif

#endif /* GN_H */
