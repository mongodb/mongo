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
 *    A fuzzing target for modifies.
 */
int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    WT_CURSOR *cursor;
    WT_DECL_ITEM(packed_modify);
    WT_ITEM buf;
    WT_MODIFY modify;
    WT_SESSION_IMPL *session_impl;

    /* We can't do anything sensible with small inputs. */
    if (size < 10)
        return (0);

    WT_CLEAR(cursor);
    WT_CLEAR(buf);
    WT_CLEAR(modify);

    fuzzutil_setup();
    session_impl = (WT_SESSION_IMPL *)fuzz_state.session;

    /* Choose some portion of the buffer for the underlying value. */
    buf.data = &data[0];
    buf.size = data[0] % size;

    /* The modify data takes the rest. */
    modify.data.data = &data[buf.size];
    modify.data.size = modify.size = size - buf.size;
    modify.offset = data[buf.size] % size;

    /* We're doing this in order to get a cursor since we need one to call the modify helper. */
    testutil_check(
      fuzz_state.session->open_cursor(fuzz_state.session, "metadata:", NULL, NULL, &cursor));
    testutil_check(__wt_modify_pack(cursor, &modify, 1, &packed_modify));
    testutil_check(__wt_modify_apply_item(session_impl, "u", &buf, packed_modify->data));

    testutil_check(cursor->close(cursor));
    __wt_scr_free(session_impl, &packed_modify);
    __wt_buf_free(session_impl, &buf);
    return (0);
}
