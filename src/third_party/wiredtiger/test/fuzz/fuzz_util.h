/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#include "test_util.h"

typedef struct {
    WT_CONNECTION *conn;
    WT_SESSION *session;
} FUZZ_GLOBAL_STATE;

extern FUZZ_GLOBAL_STATE fuzz_state;

void fuzzutil_setup(void);

/* ![fuzzutil sliced input api] */
typedef struct {
    const uint8_t **slices;
    size_t *sizes;
    size_t num_slices;
} FUZZ_SLICED_INPUT;

bool fuzzutil_sliced_input_init(FUZZ_SLICED_INPUT *input, const uint8_t *data, size_t size,
  const uint8_t *sep, size_t sep_size, size_t req_slices);
void fuzzutil_sliced_input_free(FUZZ_SLICED_INPUT *input);
/* ![fuzzutil sliced input api] */

char *fuzzutil_slice_to_cstring(const uint8_t *data, size_t size);
