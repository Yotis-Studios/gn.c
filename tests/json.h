/* json.h - minimal JSON reader for the test suite (not part of the library).
 * Parses the whole document into a tree of json_node; numbers keep their
 * source text so integers and doubles are both exact. */
#ifndef GN_TEST_JSON_H
#define GN_TEST_JSON_H

#include <stdlib.h>
#include <string.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } json_kind;

typedef struct json_node {
    json_kind kind;
    int b;
    char *s;               /* J_STR: decoded UTF-8; J_NUM: number text */
    char *key;             /* set when this node is an object member */
    struct json_node *child;
    struct json_node *next;
} json_node;

typedef struct {
    const char *p;
    int err;
} json_parser;

static void json_ws(json_parser *jp)
{
    while (*jp->p == ' ' || *jp->p == '\n' || *jp->p == '\r' || *jp->p == '\t') jp->p++;
}

static json_node *json_new(json_kind k)
{
    json_node *n = (json_node *)calloc(1, sizeof *n);
    n->kind = k;
    return n;
}

static void json_utf8(char **out, unsigned cp)
{
    char *o = *out;
    if (cp < 0x80) {
        *o++ = (char)cp;
    } else if (cp < 0x800) {
        *o++ = (char)(0xc0 | (cp >> 6));
        *o++ = (char)(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        *o++ = (char)(0xe0 | (cp >> 12));
        *o++ = (char)(0x80 | ((cp >> 6) & 0x3f));
        *o++ = (char)(0x80 | (cp & 0x3f));
    } else {
        *o++ = (char)(0xf0 | (cp >> 18));
        *o++ = (char)(0x80 | ((cp >> 12) & 0x3f));
        *o++ = (char)(0x80 | ((cp >> 6) & 0x3f));
        *o++ = (char)(0x80 | (cp & 0x3f));
    }
    *out = o;
}

static char *json_string(json_parser *jp)
{
    const char *start = ++jp->p; /* skip opening quote */
    char *buf, *o;
    while (*jp->p && *jp->p != '"') {
        if (*jp->p == '\\' && jp->p[1]) jp->p++;
        jp->p++;
    }
    if (*jp->p != '"') {
        jp->err = 1;
        return NULL;
    }
    buf = (char *)malloc((size_t)(jp->p - start) * 2 + 1);
    o = buf;
    while (start < jp->p) {
        if (*start != '\\') {
            *o++ = *start++;
            continue;
        }
        start++;
        switch (*start) {
        case 'n': *o++ = '\n'; start++; break;
        case 't': *o++ = '\t'; start++; break;
        case 'r': *o++ = '\r'; start++; break;
        case 'b': *o++ = '\b'; start++; break;
        case 'f': *o++ = '\f'; start++; break;
        case 'u': {
            char hex[5];
            unsigned cp;
            memcpy(hex, start + 1, 4);
            hex[4] = 0;
            cp = (unsigned)strtoul(hex, NULL, 16);
            start += 5;
            if (cp >= 0xd800 && cp < 0xdc00 && start[0] == '\\' && start[1] == 'u') {
                unsigned lo;
                memcpy(hex, start + 2, 4);
                lo = (unsigned)strtoul(hex, NULL, 16);
                cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                start += 6;
            }
            json_utf8(&o, cp);
            break;
        }
        default: *o++ = *start++; break;
        }
    }
    *o = 0;
    jp->p++; /* closing quote */
    return buf;
}

static json_node *json_value(json_parser *jp)
{
    json_node *n = NULL;
    json_ws(jp);
    if (*jp->p == '{' || *jp->p == '[') {
        int obj = *jp->p == '{';
        json_node **tail;
        n = json_new(obj ? J_OBJ : J_ARR);
        tail = &n->child;
        jp->p++;
        json_ws(jp);
        if (*jp->p == (obj ? '}' : ']')) {
            jp->p++;
            return n;
        }
        for (;;) {
            char *key = NULL;
            json_node *item;
            json_ws(jp);
            if (obj) {
                if (*jp->p != '"') { jp->err = 1; return n; }
                key = json_string(jp);
                json_ws(jp);
                if (*jp->p != ':') { jp->err = 1; return n; }
                jp->p++;
            }
            item = json_value(jp);
            if (!item) { jp->err = 1; return n; }
            item->key = key;
            *tail = item;
            tail = &item->next;
            json_ws(jp);
            if (*jp->p == ',') { jp->p++; continue; }
            if (*jp->p == (obj ? '}' : ']')) { jp->p++; return n; }
            jp->err = 1;
            return n;
        }
    }
    if (*jp->p == '"') {
        n = json_new(J_STR);
        n->s = json_string(jp);
        return n;
    }
    if (strncmp(jp->p, "true", 4) == 0) { n = json_new(J_BOOL); n->b = 1; jp->p += 4; return n; }
    if (strncmp(jp->p, "false", 5) == 0) { n = json_new(J_BOOL); jp->p += 5; return n; }
    if (strncmp(jp->p, "null", 4) == 0) { jp->p += 4; return json_new(J_NULL); }
    if (*jp->p == '-' || (*jp->p >= '0' && *jp->p <= '9')) {
        const char *start = jp->p;
        size_t len;
        while (*jp->p && strchr("+-0123456789.eE", *jp->p)) jp->p++;
        len = (size_t)(jp->p - start);
        n = json_new(J_NUM);
        n->s = (char *)malloc(len + 1);
        memcpy(n->s, start, len);
        n->s[len] = 0;
        return n;
    }
    jp->err = 1;
    return NULL;
}

static json_node *json_parse(const char *text)
{
    json_parser jp;
    json_node *root;
    jp.p = text;
    jp.err = 0;
    root = json_value(&jp);
    return jp.err ? NULL : root;
}

static void json_free(json_node *n)
{
    while (n) {
        json_node *next = n->next;
        json_free(n->child);
        free(n->s);
        free(n->key);
        free(n);
        n = next;
    }
}

static json_node *json_get(const json_node *obj, const char *key)
{
    json_node *c;
    if (!obj || obj->kind != J_OBJ) return NULL;
    for (c = obj->child; c; c = c->next) {
        if (c->key && strcmp(c->key, key) == 0) return c;
    }
    return NULL;
}

#endif
