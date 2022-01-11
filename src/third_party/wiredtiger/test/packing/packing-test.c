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
 * check --
 *     TODO: Add a comment describing this function.
 */
static void
check(const char *fmt, ...)
{
    size_t len;
    char buf[200], *end, *p;
    va_list ap;

    len = 0; /* -Werror=maybe-uninitialized */

    va_start(ap, fmt);
    testutil_check(__wt_struct_sizev(NULL, &len, fmt, ap));
    va_end(ap);

    if (len < 1 || len >= sizeof(buf))
        testutil_die(EINVAL, "Unexpected length from __wt_struct_sizev");

    va_start(ap, fmt);
    testutil_check(__wt_struct_packv(NULL, buf, sizeof(buf), fmt, ap));
    va_end(ap);

    printf("%s ", fmt);
    for (p = buf, end = p + len; p < end; p++)
        printf("%02x", (u_char)*p & 0xff);
    printf("\n");
}

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(void)
{
    /*
     * Required on some systems to pull in parts of the library for which we have data references.
     */
    testutil_check(__wt_library_init());

    check("iii", 0, 101, -99);
    check("3i", 0, 101, -99);
    check("iS", 42, "forty two");
    check("s", "a big string");
#if 0
	/* TODO: need a WT_ITEM */
	check("u", r"\x42" * 20)
	check("uu", r"\x42" * 10, r"\x42" * 10)
#endif
    return (0);
}
