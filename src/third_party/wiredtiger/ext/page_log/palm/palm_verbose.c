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

#include <stdlib.h>
#include <wiredtiger.h>

#include "palm_verbose.h"

#define N_VERBOSE_SLOTS 16 /* must be a power of 2 */
#define N_VERBOSE_MASK (N_VERBOSE_SLOTS - 1)

/*
 * Return a single hex character.
 */
static char
verbose_hex_char(uint8_t x)
{
    if (x < 10)
        return ((char)('0' + x));
    else
        return ((char)('a' + (x - 10)));
}

/*
 * Return a string for the buffer. Not strictly reentrant, but returns entries from a circular
 * buffer in a round-robin fashion.
 */
const char *
palm_verbose_item(const WT_ITEM *buf)
{
    static uint8_t slot_count = 0;
    static char return_slot[N_VERBOSE_SLOTS][1024];
    uint8_t slot;
    const uint8_t *p;
    size_t n;
    char *end, *s;

    /*
     * Select the next slot in a circle of buffers.
     */
    slot = (__atomic_add_fetch(&slot_count, 1, __ATOMIC_SEQ_CST)) & N_VERBOSE_MASK;

    /*
     * Get the beginning and end of the buffer. Leave plenty of room at the end for the final entry
     * plus the possibility of a space, newline, and a final "overflow" message.
     */
    s = &return_slot[slot][0];
    end = &return_slot[slot][sizeof(return_slot[0])] - 30;

    for (n = 0, p = buf->data; n < buf->size && s < end; ++n, ++p) {
        if (n != 0 && n % 32 == 0)
            *s++ = '\n';
        if (n % 4 == 0)
            *s++ = ' ';
        *s++ = verbose_hex_char((*p & 0xf0) >> 4);
        *s++ = verbose_hex_char(*p & 0xf);
    }
    if (n < buf->size)
        snprintf(s, 25, "...[%d total]", (int)buf->size);
    else
        *s = '\0';

    return &return_slot[slot][0];
}
