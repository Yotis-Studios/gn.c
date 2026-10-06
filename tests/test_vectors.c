/* Runs gn.c against the shared protocol conformance vectors.
 * tests/vectors/protocol.json is copied from gn.js (test/vectors/protocol.json);
 * update it from there, never by hand.
 *
 * Usage: test_vectors [path/to/protocol.json] */
#include "gn.h"
#include "json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int passed, failed;
static char failure[512];

#define CHECK(cond, ...)                                          \
    do {                                                          \
        if (!(cond)) {                                            \
            snprintf(failure, sizeof failure, __VA_ARGS__);       \
            return 0;                                             \
        }                                                         \
    } while (0)

static void report(const char *section, const char *name, int ok)
{
    if (ok) {
        passed++;
    } else {
        failed++;
        printf("  FAIL %s: %s - %s\n", section, name, failure);
    }
}

static unsigned char *from_hex(const char *hex, size_t *len)
{
    size_t n = strlen(hex) / 2, i;
    unsigned char *out = (unsigned char *)malloc(n ? n : 1);
    for (i = 0; i < n; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (unsigned char)v;
    }
    *len = n;
    return out;
}

static void to_hex(const unsigned char *b, size_t len, char *out)
{
    size_t i;
    for (i = 0; i < len; i++) sprintf(out + 2 * i, "%02x", b[i]);
    out[2 * len] = 0;
}

static double number(const json_node *v)
{
    if (v->kind == J_STR) {
        if (strcmp(v->s, "NaN") == 0) return NAN;
        if (strcmp(v->s, "Infinity") == 0) return INFINITY;
        return -INFINITY;
    }
    return strtod(v->s, NULL);
}

static const char *str(const json_node *obj, const char *key)
{
    const json_node *n = json_get(obj, key);
    return n && n->kind == J_STR ? n->s : "";
}

/* encode-side value -> gn_write_* */
static void write_value(gn_writer *w, const json_node *x)
{
    const char *t = str(x, "t");
    const json_node *v = json_get(x, "v");
    if (strcmp(t, "int") == 0) {
        gn_write_int(w, strtoll(v->s, NULL, 10));
    } else if (strcmp(t, "float") == 0) {
        gn_write_double(w, number(v));
    } else if (strcmp(t, "bool") == 0) {
        gn_write_bool(w, v->b);
    } else if (strcmp(t, "string") == 0) {
        const json_node *rep = json_get(x, "repeat");
        if (rep) {
            size_t count = (size_t)strtoul(json_get(x, "count")->s, NULL, 10);
            size_t unit = strlen(rep->s), i;
            char *s = (char *)malloc(unit * count + 1);
            for (i = 0; i < count; i++) memcpy(s + i * unit, rep->s, unit);
            gn_write_stringn(w, s, unit * count);
            free(s);
        } else {
            gn_write_stringn(w, v->s, strlen(v->s));
        }
    } else if (strcmp(t, "buffer") == 0) {
        size_t len;
        unsigned char *b = from_hex(str(x, "hex"), &len);
        gn_write_buffer(w, b, len);
        free(b);
    } else {
        gn_write_undefined(w);
    }
}

static int build(const json_node *vec, unsigned char *out, size_t *len, int *err)
{
    gn_writer w;
    const json_node *v;
    gn_writer_init(&w, out, GN_MAX_PACKET, (uint32_t)strtoul(json_get(vec, "netId")->s, NULL, 10));
    for (v = json_get(vec, "values")->child; v; v = v->next) write_value(&w, v);
    *len = gn_writer_finish(&w);
    *err = w.err;
    return *len > 0;
}

/* compare one decoded value with its expected { type, v | hex } */
static int same_value(const gn_value *got, const json_node *want, int index)
{
    const char *type = str(want, "type");
    const json_node *v = json_get(want, "v");
    CHECK(strcmp(gn_type_name(got->type), type) == 0, "value %d: type %s, want %s", index, gn_type_name(got->type), type);
    switch (got->type) {
    case GN_U8: case GN_U16: case GN_U32: case GN_S8: case GN_S16: case GN_S32:
        CHECK(got->as.i == strtoll(v->s, NULL, 10), "value %d: %lld, want %s", index, (long long)got->as.i, v->s);
        break;
    case GN_F16: case GN_F32: case GN_F64: {
        double want_f = number(v);
        if (want_f != want_f) {
            CHECK(got->as.f != got->as.f, "value %d: %.17g, want NaN", index, got->as.f);
        } else {
            CHECK(got->as.f == want_f, "value %d: %.17g, want %.17g", index, got->as.f, want_f);
        }
        break;
    }
    case GN_STRING:
        CHECK(got->as.str.len == strlen(v->s) && memcmp(got->as.str.ptr, v->s, got->as.str.len) == 0,
              "value %d: string mismatch (%.*s)", index, (int)got->as.str.len, got->as.str.ptr);
        break;
    case GN_BUFFER: {
        char hex[600];
        to_hex(got->as.buf.ptr, got->as.buf.len, hex);
        CHECK(strcmp(hex, str(want, "hex")) == 0, "value %d: buffer %.200s, want %.200s", index, hex, str(want, "hex"));
        break;
    }
    default:
        break;
    }
    return 1;
}

static int check_packet(const unsigned char *payload, size_t len, const json_node *net_id, const json_node *values)
{
    gn_reader r;
    gn_value got;
    const json_node *want;
    int i = 0;
    gn_reader_init(&r, payload, len);
    if (net_id->kind == J_NULL) {
        CHECK(!r.has_net_id, "netId %u, want none", (unsigned)r.net_id);
    } else {
        CHECK(r.has_net_id && r.net_id == strtoul(net_id->s, NULL, 10), "netId %u, want %s", (unsigned)r.net_id, net_id->s);
    }
    for (want = values->child; want; want = want->next, i++) {
        CHECK(gn_read(&r, &got), "only %d values, want more", i);
        if (!same_value(&got, want, i)) return 0;
    }
    CHECK(!gn_read(&r, &got), "extra value after %d", i);
    return 1;
}

static int test_encode(const json_node *vec)
{
    static unsigned char out[GN_MAX_PACKET];
    static char hex[2 * GN_MAX_PACKET + 1];
    size_t len;
    int err;
    CHECK(build(vec, out, &len, &err), "build failed: %s", gn_error_string(err));
    to_hex(out, len, hex);
    CHECK(strcmp(hex, str(vec, "hex")) == 0, "got %.400s", hex);
    return 1;
}

static int test_encode_error(const json_node *vec)
{
    static unsigned char out[GN_MAX_PACKET];
    size_t len;
    int err;
    CHECK(!build(vec, out, &len, &err), "built %u bytes, should have refused", (unsigned)len);
    return 1;
}

static int test_decode(const json_node *vec)
{
    size_t len;
    unsigned char *payload = from_hex(str(vec, "payload"), &len);
    int ok = check_packet(payload, len, json_get(vec, "netId"), json_get(vec, "values"));
    free(payload);
    return ok;
}

static int test_frames(const json_node *vec)
{
    size_t len, plen;
    unsigned char *msg = from_hex(str(vec, "hex"), &len);
    const uint8_t *payload;
    const json_node *want;
    gn_frames f;
    int i = 0, ok = 1;
    gn_frames_init(&f, msg, len);
    for (want = json_get(vec, "packets")->child; want && ok; want = want->next, i++) {
        if (!gn_frames_next(&f, &payload, &plen)) {
            snprintf(failure, sizeof failure, "only %d packets", i);
            ok = 0;
            break;
        }
        ok = check_packet(payload, plen, json_get(want, "netId"), json_get(want, "values"));
    }
    if (ok && gn_frames_next(&f, &payload, &plen)) {
        snprintf(failure, sizeof failure, "extra packet after %d", i);
        ok = 0;
    }
    free(msg);
    return ok;
}

static void run(const json_node *root, const char *section, int (*fn)(const json_node *))
{
    const json_node *vec;
    for (vec = json_get(root, section)->child; vec; vec = vec->next) {
        failure[0] = 0;
        report(section, str(vec, "name"), fn(vec));
    }
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tests/vectors/protocol.json";
    FILE *fp = fopen(path, "rb");
    long size;
    char *text;
    json_node *root;
    if (!fp) {
        printf("cannot open %s\n", path);
        return 1;
    }
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    text = (char *)malloc((size_t)size + 1);
    text[fread(text, 1, (size_t)size, fp)] = 0;
    fclose(fp);
    root = json_parse(text);
    free(text);
    if (!root) {
        printf("cannot parse %s\n", path);
        return 1;
    }

    run(root, "encode", test_encode);
    run(root, "encode_errors", test_encode_error);
    run(root, "decode", test_decode);
    run(root, "frames", test_frames);
    json_free(root);

    printf("Vector tests: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
