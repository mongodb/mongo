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
#include "fuzz_util.h"

int LLVMFuzzerTestOneInput(const uint8_t *, size_t);

/*
 * LLVMFuzzerTestOneInput --
 *    A fuzzing target for configuration parsing.
 */
int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FUZZ_SLICED_INPUT input;
    WT_CONFIG_ITEM cval;
    char *config, *key;
    static const uint8_t separator[] = {'|'};

    WT_CLEAR(input);
    config = key = NULL;

    fuzzutil_setup();
    if (!fuzzutil_sliced_input_init(&input, data, size, separator, sizeof(separator), 2))
        return (0);

    testutil_assert(input.num_slices == 2);
    key = fuzzutil_slice_to_cstring(input.slices[0], input.sizes[0]);
    if (key == NULL)
        testutil_die(ENOMEM, "Failed to allocate key");
    config = fuzzutil_slice_to_cstring(input.slices[1], input.sizes[1]);
    if (config == NULL)
        testutil_die(ENOMEM, "Failed to allocate config");

    (void)__wt_config_getones((WT_SESSION_IMPL *)fuzz_state.session, config, key, &cval);
    (void)cval;

    fuzzutil_sliced_input_free(&input);
    free(config);
    free(key);
    return (0);
}
