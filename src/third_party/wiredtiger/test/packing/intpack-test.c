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

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(void)
{
    uint64_t ncalls, r, r2, s;
    uint8_t *p;
    uint8_t buf[WT_INTPACK64_MAXSIZE + 8]; /* -Werror=array-bounds */
    const uint8_t *cp;
    size_t used_len;
    int i;

    memset(buf, 0xff, sizeof(buf)); /* -Werror=maybe-uninitialized */

    /*
     * Required on some systems to pull in parts of the library for which we have data references.
     */
    testutil_check(__wt_library_init());

    for (ncalls = 0, i = 0; i < 10 * WT_MILLION; i++) {
        for (s = 0; s < 50; s += 5) {
            ++ncalls;
            r = 1ULL << s;

#if 1
            p = buf;
            testutil_check(__wt_vpack_uint(&p, sizeof(buf), r));
            used_len = (size_t)(p - buf);
            testutil_assert(used_len <= WT_INTPACK64_MAXSIZE);
            cp = buf;
            testutil_check(__wt_vunpack_uint(&cp, sizeof(buf), &r2));
#else
            /*
             * Note: use memmove for comparison because GCC does aggressive optimization of memcpy
             * and it's difficult to measure anything.
             */
            p = buf;
            memmove(p, &r, sizeof(r));
            cp = buf;
            memmove(&r2, cp, sizeof(r2));
#endif
            testutil_assert(r == r2);
        }
    }

    printf("Number of calls: %" PRIu64 "\n", ncalls);

    return (0);
}
