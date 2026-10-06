/* libFuzzer: feed arbitrary client bytes (HTTP upgrade + frames) to a
 * server connection. Build with -DGN_FUZZING. */
#include "gn.h"

void gn__fuzz_server_feed(const uint8_t *data, size_t len);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    gn__fuzz_server_feed(data, size);
    return 0;
}
