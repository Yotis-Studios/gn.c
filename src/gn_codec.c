/* gn_codec.c - packet encoding/decoding (PROTOCOL.md sections 2-6) */
#include "gn.h"

#include <string.h>

/* ---------------------------------------------------------------------- */
/* Byte order: explicit shifts, so the host's endianness never matters.   */

static void put_u16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static uint32_t get_u16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t f32_bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

static float f32_from_bits(uint32_t u)
{
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

static uint64_t f64_bits(double d)
{
    uint64_t u;
    if (d != d) {
        return (uint64_t)0x7ff8000000000000ULL; /* canonical quiet NaN */
    }
    memcpy(&u, &d, sizeof u);
    return u;
}

static double f64_from_bits(uint64_t u)
{
    double d;
    memcpy(&d, &u, sizeof d);
    return d;
}

/* IEEE 754 binary16 -> double, via the f32 bit layout (exact). */
static double half_to_double(uint32_t h)
{
    uint32_t sign = (h >> 15) & 1u;
    uint32_t exp = (h >> 10) & 0x1fu;
    uint32_t frac = h & 0x3ffu;
    double v;
    if (exp == 0) {
        v = (double)frac / 16777216.0; /* subnormal: frac * 2^-24 */
    } else if (exp == 31) {
        v = (double)f32_from_bits(0x7f800000u | (frac << 13)); /* inf / NaN */
    } else {
        v = (double)f32_from_bits(((exp + 112u) << 23) | (frac << 13)); /* rebias 15 -> 127 */
    }
    return sign ? -v : v;
}

/* ---------------------------------------------------------------------- */

const char *gn_error_string(int err)
{
    switch (err) {
    case GN_OK: return "ok";
    case GN_ERR_RANGE: return "value out of range for its field";
    case GN_ERR_TOO_LARGE: return "value or packet too large for its length field";
    case GN_ERR_NO_SPACE: return "output buffer too small";
    case GN_ERR_MALFORMED: return "malformed packet data";
    case GN_ERR_ARG: return "invalid argument";
    case GN_ERR_STATE: return "invalid state for this operation";
    case GN_ERR_SOCKET: return "socket error";
    case GN_ERR_HANDSHAKE: return "WebSocket handshake failed";
    case GN_ERR_MEMORY: return "out of memory";
    default: return "unknown error";
    }
}

const char *gn_type_name(gn_type type)
{
    static const char *const names[GN_TYPE_COUNT] = {
        "u8", "u16", "u32", "s8", "s16", "s32", "f16", "f32", "f64", "string", "buffer", "undefined"
    };
    if ((int)type < 0 || (int)type >= GN_TYPE_COUNT) {
        return "invalid";
    }
    return names[type];
}

int gn_value_int(const gn_value *v, int64_t *out)
{
    if (!v || !out || v->type > GN_S32) {
        return 0;
    }
    *out = v->as.i;
    return 1;
}

int gn_value_double(const gn_value *v, double *out)
{
    if (!v || !out) {
        return 0;
    }
    if (v->type <= GN_S32) {
        *out = (double)v->as.i;
        return 1;
    }
    if (v->type == GN_F16 || v->type == GN_F32 || v->type == GN_F64) {
        *out = v->as.f;
        return 1;
    }
    return 0;
}

int gn_value_bool(const gn_value *v, int *out)
{
    double d;
    if (!out || !gn_value_double(v, &d)) {
        return 0;
    }
    *out = d != 0.0;
    return 1;
}

/* ====================================================================== */
/* Writer                                                                 */
/* ====================================================================== */

void gn_writer_init(gn_writer *w, void *buffer, size_t cap, uint32_t net_id)
{
    if (!w) {
        return;
    }
    w->data = (uint8_t *)buffer;
    w->cap = cap;
    w->len = 0;
    w->err = GN_OK;
    if (!buffer) {
        w->err = GN_ERR_ARG;
        return;
    }
    if (net_id > 65535) {
        w->err = GN_ERR_RANGE;
        return;
    }
    if (cap < 4) {
        w->err = GN_ERR_NO_SPACE;
        return;
    }
    put_u16(w->data, 2); /* fixed up by gn_writer_finish */
    put_u16(w->data + 2, net_id);
    w->len = 4;
}

/* Reserves n bytes (type byte included) and returns where to write them. */
static uint8_t *reserve(gn_writer *w, size_t n)
{
    uint8_t *p;
    if (!w) {
        return NULL;
    }
    if (w->err) {
        return NULL;
    }
    if (w->len - 2 + n > 65535) {
        w->err = GN_ERR_TOO_LARGE;
        return NULL;
    }
    if (w->len + n > w->cap) {
        w->err = GN_ERR_NO_SPACE;
        return NULL;
    }
    p = w->data + w->len;
    w->len += n;
    return p;
}

static int fail(gn_writer *w, int err)
{
    if (w && !w->err) {
        w->err = err;
    }
    return w ? w->err : GN_ERR_ARG;
}

static int write_fixed(gn_writer *w, gn_type type, uint32_t bits, size_t size)
{
    uint8_t *p = reserve(w, 1 + size);
    if (!p) {
        return w ? w->err : GN_ERR_ARG;
    }
    p[0] = (uint8_t)type;
    if (size == 1) {
        p[1] = (uint8_t)bits;
    } else if (size == 2) {
        put_u16(p + 1, bits);
    } else {
        put_u32(p + 1, bits);
    }
    return GN_OK;
}

static int write_f64(gn_writer *w, double v)
{
    uint64_t bits = f64_bits(v);
    uint8_t *p = reserve(w, 9);
    if (!p) {
        return w ? w->err : GN_ERR_ARG;
    }
    p[0] = GN_F64;
    put_u32(p + 1, (uint32_t)(bits & 0xffffffffu));
    put_u32(p + 5, (uint32_t)(bits >> 32));
    return GN_OK;
}

int gn_write_int(gn_writer *w, int64_t v)
{
    if (v >= 0) {
        if (v < 256) return write_fixed(w, GN_U8, (uint32_t)v, 1);
        if (v < 65536) return write_fixed(w, GN_U16, (uint32_t)v, 2);
        if (v <= 4294967295LL) return write_fixed(w, GN_U32, (uint32_t)v, 4);
    } else {
        if (v > -129) return write_fixed(w, GN_S8, (uint32_t)(int32_t)v, 1);
        if (v > -32769) return write_fixed(w, GN_S16, (uint32_t)(int32_t)v, 2);
        if (v >= -2147483647LL - 1) return write_fixed(w, GN_S32, (uint32_t)(int32_t)v, 4);
    }
    return fail(w, GN_ERR_RANGE);
}

int gn_write_uint(gn_writer *w, uint64_t v)
{
    if (v > 4294967295ULL) {
        return fail(w, GN_ERR_RANGE);
    }
    return gn_write_int(w, (int64_t)v);
}

int gn_write_double(gn_writer *w, double v)
{
    double mag = v < 0 ? -v : v;
    if (mag <= 16777216.0) { /* false for NaN and infinities */
        return write_fixed(w, GN_F32, f32_bits((float)v), 4);
    }
    return write_f64(w, v);
}

int gn_write_bool(gn_writer *w, int v)
{
    return write_fixed(w, GN_U8, v ? 1u : 0u, 1);
}

int gn_write_string(gn_writer *w, const char *s)
{
    if (!s) {
        return fail(w, GN_ERR_ARG);
    }
    return gn_write_stringn(w, s, strlen(s));
}

int gn_write_stringn(gn_writer *w, const char *s, size_t len)
{
    /* Encoders append a NUL counted in len; like gn.js, a string that already
     * ends in NUL is not given a second one. */
    size_t wire_len;
    uint8_t *p;
    if (!s && len > 0) {
        return fail(w, GN_ERR_ARG);
    }
    wire_len = (len > 0 && s[len - 1] == '\0') ? len : len + 1;
    if (wire_len > 65535) {
        return fail(w, GN_ERR_TOO_LARGE);
    }
    p = reserve(w, 3 + wire_len);
    if (!p) {
        return w ? w->err : GN_ERR_ARG;
    }
    p[0] = GN_STRING;
    put_u16(p + 1, (uint32_t)wire_len);
    if (len > 0) {
        memcpy(p + 3, s, len);
    }
    p[3 + wire_len - 1] = 0;
    return GN_OK;
}

int gn_write_buffer(gn_writer *w, const void *data, size_t len)
{
    uint8_t *p;
    if (!data && len > 0) {
        return fail(w, GN_ERR_ARG);
    }
    if (len > GN_MAX_BUFFER) {
        return fail(w, GN_ERR_TOO_LARGE);
    }
    p = reserve(w, 2 + len);
    if (!p) {
        return w ? w->err : GN_ERR_ARG;
    }
    p[0] = GN_BUFFER;
    p[1] = (uint8_t)len;
    if (len > 0) {
        memcpy(p + 2, data, len);
    }
    return GN_OK;
}

int gn_write_undefined(gn_writer *w)
{
    uint8_t *p = reserve(w, 1);
    if (!p) {
        return w ? w->err : GN_ERR_ARG;
    }
    p[0] = GN_UNDEFINED;
    return GN_OK;
}

int gn_write_value(gn_writer *w, const gn_value *v)
{
    int64_t i;
    if (!v) {
        return fail(w, GN_ERR_ARG);
    }
    i = v->as.i;
    switch (v->type) {
    case GN_U8:
        if (i < 0 || i > 255) return fail(w, GN_ERR_RANGE);
        return write_fixed(w, GN_U8, (uint32_t)i, 1);
    case GN_U16:
        if (i < 0 || i > 65535) return fail(w, GN_ERR_RANGE);
        return write_fixed(w, GN_U16, (uint32_t)i, 2);
    case GN_U32:
        if (i < 0 || i > 4294967295LL) return fail(w, GN_ERR_RANGE);
        return write_fixed(w, GN_U32, (uint32_t)i, 4);
    case GN_S8:
        if (i < -128 || i > 127) return fail(w, GN_ERR_RANGE);
        return write_fixed(w, GN_S8, (uint32_t)(int32_t)i, 1);
    case GN_S16:
        if (i < -32768 || i > 32767) return fail(w, GN_ERR_RANGE);
        return write_fixed(w, GN_S16, (uint32_t)(int32_t)i, 2);
    case GN_S32:
        if (i < -2147483647LL - 1 || i > 2147483647LL) return fail(w, GN_ERR_RANGE);
        return write_fixed(w, GN_S32, (uint32_t)(int32_t)i, 4);
    case GN_F32:
        return write_fixed(w, GN_F32, f32_bits((float)v->as.f), 4);
    case GN_F64:
        return write_f64(w, v->as.f);
    case GN_STRING:
        return gn_write_stringn(w, v->as.str.ptr, v->as.str.len);
    case GN_BUFFER:
        return gn_write_buffer(w, v->as.buf.ptr, v->as.buf.len);
    case GN_UNDEFINED:
        return gn_write_undefined(w);
    default:
        return fail(w, GN_ERR_ARG); /* GN_F16 or invalid */
    }
}

size_t gn_writer_finish(gn_writer *w)
{
    if (!w || w->err) {
        return 0;
    }
    put_u16(w->data, (uint32_t)(w->len - 2));
    return w->len;
}

/* ====================================================================== */
/* Reader                                                                 */
/* ====================================================================== */

void gn_reader_init(gn_reader *r, const void *payload, size_t len)
{
    if (!r) {
        return;
    }
    r->data = (const uint8_t *)payload;
    r->len = payload ? len : 0;
    r->err = GN_OK;
    r->net_id = 0;
    r->has_net_id = 0;
    r->pos = r->len;
    if (r->len >= 2) {
        r->net_id = (uint16_t)get_u16(r->data);
        r->has_net_id = 1;
        r->pos = 2;
    }
}

static int malformed(gn_reader *r)
{
    r->err = GN_ERR_MALFORMED;
    r->pos = r->len;
    return 0;
}

int gn_read(gn_reader *r, gn_value *out)
{
    static const uint8_t sizes[GN_TYPE_COUNT] = { 1, 2, 4, 1, 2, 4, 2, 4, 8, 0, 0, 0 };
    const uint8_t *p;
    size_t remaining;
    gn_type type;
    if (!r || !out || r->pos >= r->len) {
        return 0;
    }
    type = (gn_type)r->data[r->pos];
    if ((int)type >= GN_TYPE_COUNT) {
        return malformed(r);
    }
    p = r->data + r->pos + 1;
    remaining = r->len - r->pos - 1;
    out->type = type;

    if (type == GN_STRING) {
        size_t len;
        if (remaining < 2) return malformed(r);
        len = get_u16(p);
        if (remaining < 2 + len) return malformed(r);
        out->as.str.ptr = (const char *)(p + 2);
        /* strip the NUL terminator if present */
        out->as.str.len = (len > 0 && p[2 + len - 1] == 0) ? len - 1 : len;
        r->pos += 1 + 2 + len;
        return 1;
    }
    if (type == GN_BUFFER) {
        size_t len;
        if (remaining < 1) return malformed(r);
        len = p[0];
        if (remaining < 1 + len) return malformed(r);
        out->as.buf.ptr = p + 1;
        out->as.buf.len = len;
        r->pos += 1 + 1 + len;
        return 1;
    }

    if (remaining < sizes[type]) {
        return malformed(r);
    }
    switch (type) {
    case GN_U8: out->as.i = p[0]; break;
    case GN_U16: out->as.i = get_u16(p); break;
    case GN_U32: out->as.i = get_u32(p); break;
    case GN_S8: out->as.i = (int8_t)p[0]; break;
    case GN_S16: out->as.i = (int16_t)get_u16(p); break;
    case GN_S32: out->as.i = (int32_t)get_u32(p); break;
    case GN_F16: out->as.f = half_to_double(get_u16(p)); break;
    case GN_F32: out->as.f = (double)f32_from_bits(get_u32(p)); break;
    case GN_F64: out->as.f = f64_from_bits((uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32)); break;
    default: break; /* GN_UNDEFINED: no data */
    }
    r->pos += 1 + sizes[type];
    return 1;
}

/* ====================================================================== */
/* Frames                                                                 */
/* ====================================================================== */

void gn_frames_init(gn_frames *f, const void *message, size_t len)
{
    if (!f) {
        return;
    }
    f->data = (const uint8_t *)message;
    f->len = message ? len : 0;
    f->pos = 0;
}

int gn_frames_next(gn_frames *f, const uint8_t **payload, size_t *len)
{
    size_t size;
    if (!f || !payload || !len || f->pos + 2 > f->len) {
        return 0;
    }
    size = get_u16(f->data + f->pos);
    if (size == 0 || f->pos + 2 + size > f->len) {
        f->pos = f->len; /* stop: the rest of the message is discarded */
        return 0;
    }
    *payload = f->data + f->pos + 2;
    *len = size;
    f->pos += 2 + size;
    return 1;
}
