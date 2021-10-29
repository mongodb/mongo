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

#include "format.h"

/*
 * random_kv --
 *     Do random cursor operations.
 */
WT_THREAD_RET
random_kv(void *arg)
{
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_ITEM key, value;
    WT_SESSION *session;
    uint32_t i;
    u_int period;
    const char *config;
    bool simple;

    (void)(arg); /* Unused parameter */

    conn = g.wts_conn;

    /* Random cursor ops are only supported on row-store. */
    if (g.type != ROW)
        return (WT_THREAD_RET_VALUE);

    /* Open a session. */
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    for (simple = false;;) {
        /* Alternate between simple random cursors and sample-size random cursors. */
        config = simple ? "next_random=true" : "next_random=true,next_random_sample_size=37";
        simple = !simple;

        /*
         * open_cursor can return EBUSY if concurrent with a metadata operation, retry in that case.
         */
        while ((ret = session->open_cursor(session, g.uri, NULL, config, &cursor)) == EBUSY)
            __wt_yield();
        testutil_check(ret);

        /* This is just a smoke-test, get some key/value pairs. */
        for (i = mmrand(NULL, 0, 1000); i > 0; --i) {
            switch (ret = cursor->next(cursor)) {
            case 0:
                break;
            case WT_NOTFOUND:
            case WT_ROLLBACK:
            case WT_CACHE_FULL:
            case WT_PREPARE_CONFLICT:
                continue;
            default:
                testutil_check(ret);
            }
            testutil_check(cursor->get_key(cursor, &key));
            testutil_check(cursor->get_value(cursor, &value));
        }

        testutil_check(cursor->close(cursor));

        /* Sleep for some number of seconds. */
        period = mmrand(NULL, 1, 10);

        /* Sleep for short periods so we don't make the run wait. */
        while (period > 0 && !g.workers_finished) {
            --period;
            __wt_sleep(1, 0);
        }
        if (g.workers_finished)
            break;
    }

    testutil_check(session->close(session, NULL));

    return (WT_THREAD_RET_VALUE);
}
