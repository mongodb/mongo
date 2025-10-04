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
 * JIRA ticket reference: WT-4117 Test case description: Smoke-test the CRC32C external API.
 */

/*
 * check --
 *     TODO: Add a comment describing this function.
 */
static inline void
check(uint32_t crc32c, uint32_t expected, size_t len, const char *msg)
{
    testutil_checkfmt(crc32c == expected ? 0 : 1,
      "%s checksum mismatch of %" WT_SIZET_FMT " bytes: %#08x != %#08x\n", msg, len, crc32c,
      expected);
}

/*
 * run --
 *     TODO: Add a comment describing this function.
 */
static void
run(void)
{
    size_t len;
    uint32_t crc32c, (*func)(const void *, size_t);
    uint8_t *data;

    /* Allocate aligned memory for the data. */
    data = dcalloc(100, sizeof(uint8_t));

    /* Get a pointer to the CRC32C function. */
    func = wiredtiger_crc32c_func();

    /*
     * Some simple known checksums.
     */
    len = 1;
    crc32c = func(data, len);
    check(crc32c, (uint32_t)0x527d5351, len, "nul x1");

    len = 2;
    crc32c = func(data, len);
    check(crc32c, (uint32_t)0xf16177d2, len, "nul x2");

    len = 3;
    crc32c = func(data, len);
    check(crc32c, (uint32_t)0x6064a37a, len, "nul x3");

    len = 4;
    crc32c = func(data, len);
    check(crc32c, (uint32_t)0x48674bc7, len, "nul x4");

    len = strlen("123456789");
    memcpy(data, "123456789", len);
    crc32c = func(data, len);
    check(crc32c, (uint32_t)0xe3069283, len, "known string #1");

    len = strlen("The quick brown fox jumps over the lazy dog");
    memcpy(data, "The quick brown fox jumps over the lazy dog", len);
    crc32c = func(data, len);
    check(crc32c, (uint32_t)0x22620404, len, "known string #2");

    free(data);
}

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(void)
{
    run();

    return (EXIT_SUCCESS);
}
