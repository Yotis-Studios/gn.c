/* libFuzzer: feed arbitrary server bytes (handshake + frames) to the client
 * parser. Build with -DGN_FUZZING. */
#include "gn.h"

void gn__fuzz_client_feed(const uint8_t *data, size_t len);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    gn__fuzz_client_feed(data, size);
    return 0;
}
