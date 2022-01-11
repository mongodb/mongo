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

void test_value(int64_t);
void test_spread(int64_t, int64_t, int64_t);

/*
 * test_value --
 *     TODO: Add a comment describing this function.
 */
void
test_value(int64_t val)
{
    const uint8_t *cp;
    uint8_t buf[WT_INTPACK64_MAXSIZE + 8]; /* -Werror=array-bounds */
    uint8_t *p;
    int64_t sinput, soutput;
    uint64_t uinput, uoutput;
    size_t used_len;

    memset(buf, 0xff, sizeof(buf)); /* -Werror=maybe-uninitialized */
    sinput = val;
    soutput = 0; /* -Werror=maybe-uninitialized */

    /*
     * Required on some systems to pull in parts of the library for which we have data references.
     */
    testutil_check(__wt_library_init());

    p = buf;
    testutil_check(__wt_vpack_int(&p, sizeof(buf), sinput));
    used_len = (size_t)(p - buf);
    testutil_assert(used_len <= WT_INTPACK64_MAXSIZE);
    cp = buf;
    testutil_check(__wt_vunpack_int(&cp, used_len, &soutput));
    /* Ensure we got the correct value back */
    if (sinput != soutput) {
        fprintf(stderr, "mismatch %" PRId64 ", %" PRId64 "\n", sinput, soutput);
        abort();
    }
    /* Ensure that decoding used the correct amount of buffer */
    if (cp != p) {
        fprintf(stderr,
          "Unpack consumed wrong size for %" PRId64 ", expected %" WT_SIZET_FMT
          ", got %" WT_SIZET_FMT "\n",
          sinput, used_len,
          cp > p ? used_len + (size_t)(cp - p) : /* More than buf used */
            used_len - (size_t)(p - cp));        /* Less than buf used */
        abort();
    }

    /* Test unsigned, convert negative into bigger positive values */
    uinput = (uint64_t)val;

    p = buf;
    testutil_check(__wt_vpack_uint(&p, sizeof(buf), uinput));
    used_len = (size_t)(p - buf);
    testutil_assert(used_len <= WT_INTPACK64_MAXSIZE);
    cp = buf;
    testutil_check(__wt_vunpack_uint(&cp, sizeof(buf), &uoutput));
    /* Ensure we got the correct value back */
    if (sinput != soutput) {
        fprintf(stderr, "mismatch %" PRId64 ", %" PRId64 "\n", sinput, soutput);
        abort();
    }
    /* Ensure that decoding used the correct amount of buffer */
    if (cp != p) {
        fprintf(stderr,
          "Unpack consumed wrong size for %" PRId64 ", expected %" WT_SIZET_FMT
          ", got %" WT_SIZET_FMT "\n",
          sinput, used_len, cp > p ? used_len + (size_t)(cp - p) : used_len - (size_t)(p - cp));
        abort();
    }
}

/*
 * test_spread --
 *     TODO: Add a comment describing this function.
 */
void
test_spread(int64_t start, int64_t before, int64_t after)
{
    int64_t i;

    printf("Testing range: %" PRId64 " to %" PRId64 ". Spread: % " PRId64 "\n", start - before,
      start + after, before + after);
    for (i = start - before; i < start + after; i++)
        test_value(i);
}

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(void)
{
    int64_t i, range, start_int64;

    range = 1025;
    start_int64 = INT64_MAX - range; /* reduce start point by range to avoid integer overflow */

    /*
     * Test all values in a range, to ensure pack/unpack of small numbers (which most actively use
     * different numbers of bits) works.
     */
    test_spread(0, 100000, 100000);
    test_spread(INT16_MAX, range, range);
    test_spread(INT32_MAX, range, range);
    test_spread(start_int64, range, range);
    /* Test bigger numbers */
    for (i = start_int64; i > 0; i = i / 2)
        test_spread(i, range, range);
    printf("\n");

    return (0);
}
