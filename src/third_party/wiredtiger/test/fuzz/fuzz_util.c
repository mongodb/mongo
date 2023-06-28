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

#include <stdint.h>
#include <stddef.h>

FUZZ_GLOBAL_STATE fuzz_state = {.conn = NULL, .session = NULL};

#define HOME_BUF_SIZE 100

/*
 * fuzzutil_generate_home_name --
 *     Create a unique home directory per worker that LibFuzzer creates.
 */
static void
fuzzutil_generate_home_name(char *buf, int buf_len)
{
    pid_t pid;

    /*
     * Once we're exercising the fuzz target, we don't know which worker we are. An easy way to make
     * sure that workers don't clobber each other's home directories is to tack the process ID on
     * the end of the name.
     */
    pid = getpid();
    testutil_check(__wt_snprintf(buf, buf_len, "WT_TEST_%d", pid));
}

/*
 * fuzzutil_setup --
 *     Initialize the connection and session the first time LibFuzzer executes the target.
 */
void
fuzzutil_setup(void)
{
    char home[HOME_BUF_SIZE];

    if (fuzz_state.conn != NULL) {
        testutil_assert(fuzz_state.session != NULL);
        return;
    }

    WT_CLEAR(home);
    fuzzutil_generate_home_name(home, HOME_BUF_SIZE);
    testutil_make_work_dir(home);
    testutil_check(wiredtiger_open(home, NULL,
      "create,cache_size=5MB,statistics=(all),statistics_log=(json,on_close,wait=1)",
      &fuzz_state.conn));
    testutil_check(fuzz_state.conn->open_session(fuzz_state.conn, NULL, NULL, &fuzz_state.session));
}

/*
 * fuzzutil_sliced_input_init --
 *     Often, our fuzz target requires multiple inputs. For example, for configuration parsing we'd
 *     need a configuration string and a key to search for. We can do this by requiring the fuzzer
 *     to provide data with a number of arbitrary multi-byte separators. If you have a sentinel
 *     character, you can use that, otherwise patterns like 0xdeadbeef work fine too but make the
 *     corpus less readable. If the fuzzer doesn't supply data in that format, we can return out of
 *     the fuzz target. While our fuzz target will reject lots of input to begin with, the fuzzer
 *     will figure out that inputs with these separators yield better coverage and will craft more
 *     sensible inputs over time. This is what the sliced input component is designed for. It takes
 *     the data input, a separator and the number of slices that it should expect and populates a
 *     heap allocated array of data pointers to each separate input and their respective size.
 */
bool
fuzzutil_sliced_input_init(FUZZ_SLICED_INPUT *input, const uint8_t *data, size_t size,
  const uint8_t *sep, size_t sep_size, size_t req_slices)
{
    size_t *sizes;
    const uint8_t *begin, *end, *pos, **slices;
    u_int i;

    pos = NULL;
    i = 0;
    begin = data;
    end = data + size;

    /*
     * It might be better to do an initial pass to check that we have the right number of separators
     * before actually storing them. Currently, we're dynamically allocating even in the case of
     * invalid input.
     */
    slices = malloc(sizeof(uint8_t *) * req_slices);
    sizes = malloc(sizeof(size_t) * req_slices);
    if (slices == NULL || sizes == NULL)
        goto err;

    /*
     * Store pointers and sizes for each slice of the input. This code is implementing the idea
     * described at:
     * https://github.com/google/fuzzing/blob/master/docs/split-inputs.md#magic-separator.
     */
    while ((pos = memmem(begin, (size_t)(end - begin), sep, sep_size)) != NULL) {
        if (i >= req_slices)
            goto err;
        slices[i] = begin;
        sizes[i] = (size_t)(pos - begin);
        begin = pos + sep_size;
        ++i;
    }
    if (begin < end) {
        if (i >= req_slices)
            goto err;
        slices[i] = begin;
        sizes[i] = (size_t)(end - begin);
        ++i;
    }
    if (i != req_slices)
        goto err;
    input->slices = slices;
    input->sizes = sizes;
    input->num_slices = req_slices;
    return (true);

err:
    free(slices);
    free(sizes);
    return (false);
}

/*
 * fuzzutil_sliced_input_free --
 *     Free any resources on the sliced input.
 */
void
fuzzutil_sliced_input_free(FUZZ_SLICED_INPUT *input)
{
    free(input->slices);
    free(input->sizes);
    input->slices = NULL;
    input->sizes = NULL;
}

/*
 * fuzzutil_slice_to_cstring --
 *     A conversion function to help convert from a data, size pair to a cstring.
 */
char *
fuzzutil_slice_to_cstring(const uint8_t *data, size_t size)
{
    char *str;

    str = malloc(size + 1);
    if (str == NULL)
        return NULL;
    memcpy(str, data, size);
    str[size] = '\0';

    return (str);
}
