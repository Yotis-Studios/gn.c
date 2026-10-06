/* libFuzzer: split arbitrary bytes into packets, decode every value, and
 * re-encode each one. Must never read out of bounds or crash. */
#include "gn.h"

#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static uint8_t out[GN_MAX_PACKET];
    gn_frames f;
    const uint8_t *payload;
    size_t len;
    gn_reader whole;
    gn_value v;

    /* the input as one payload */
    gn_reader_init(&whole, data, size);
    while (gn_read(&whole, &v)) {
    }

    /* the input as a WebSocket message */
    gn_frames_init(&f, data, size);
    while (gn_frames_next(&f, &payload, &len)) {
        gn_reader r;
        gn_writer w;
        gn_reader_init(&r, payload, len);
        gn_writer_init(&w, out, sizeof out, r.net_id);
        while (gn_read(&r, &v)) {
            if (v.type == GN_STRING && v.as.str.len > 0 && v.as.str.ptr[v.as.str.len - 1] == 0) {
                /* fine: just must not crash */
            }
            if (v.type != GN_F16) {
                gn_write_value(&w, &v);
            }
        }
        if (gn_writer_finish(&w) == 0 && w.err != GN_ERR_TOO_LARGE) {
            abort(); /* decoded values must always re-encode */
        }
    }
    return 0;
}
