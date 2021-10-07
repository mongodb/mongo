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
 * testutil_modify_apply --
 *     Implement a modify using a completely separate algorithm as a check on the internal library
 *     algorithms.
 */
void
testutil_modify_apply(WT_ITEM *value, WT_ITEM *workspace, WT_MODIFY *entries, int nentries)
{
    WT_ITEM *ta, *tb, *tmp, _tmp;
    size_t len, size;
    int i;

    /*
     * Passed a value and array of modifications, plus a temporary buffer for an additional work
     * space.
     *
     * Process the entries to figure out the largest possible buffer we need. This is pessimistic
     * because we're ignoring replacement bytes, but it's a simpler calculation.
     */
    for (size = value->size, i = 0; i < nentries; ++i) {
        if (entries[i].offset >= size)
            size = entries[i].offset;
        size += entries[i].data.size;
    }

    /* Grow the buffers. */
    testutil_check(__wt_buf_grow(NULL, value, size));
    testutil_check(__wt_buf_grow(NULL, workspace, size));

    /*
     * Overwrite anything not initialized in the original buffer, and overwrite the entire workspace
     * buffer.
     */
    if ((value->memsize - value->size) > 0)
        memset((uint8_t *)value->mem + value->size, 0xff, value->memsize - value->size);
    if (workspace->memsize > 0)
        memset((uint8_t *)workspace->mem, 0xff, workspace->memsize);

    ta = value;
    tb = workspace;

    /*
     * From the starting buffer, create a new buffer b based on changes in the entries array. We're
     * doing a brute force solution here to test the faster solution implemented in the library.
     */
    for (i = 0; i < nentries; ++i) {
        /* Take leading bytes from the original, plus any gap bytes. */
        if (entries[i].offset >= ta->size) {
            memcpy(tb->mem, ta->mem, ta->size);
            if (entries[i].offset > ta->size)
                memset((uint8_t *)tb->mem + ta->size, '\0', entries[i].offset - ta->size);
        } else if (entries[i].offset > 0)
            memcpy(tb->mem, ta->mem, entries[i].offset);
        tb->size = entries[i].offset;

        /* Take replacement bytes. */
        if (entries[i].data.size > 0) {
            memcpy((uint8_t *)tb->mem + tb->size, entries[i].data.data, entries[i].data.size);
            tb->size += entries[i].data.size;
        }

        /* Take trailing bytes from the original. */
        len = entries[i].offset + entries[i].size;
        if (ta->size > len) {
            memcpy((uint8_t *)tb->mem + tb->size, (uint8_t *)ta->mem + len, ta->size - len);
            tb->size += ta->size - len;
        }
        testutil_assert(tb->size <= size);

        /* Swap the buffers and do it again. */
        tmp = ta;
        ta = tb;
        tb = tmp;
    }
    ta->data = ta->mem;
    tb->data = tb->mem;

    /*
     * The final results may not be in the original buffer, in which case we swap them back around.
     */
    if (ta != value) {
        _tmp = *ta;
        *ta = *tb;
        *tb = _tmp;
    }
}
