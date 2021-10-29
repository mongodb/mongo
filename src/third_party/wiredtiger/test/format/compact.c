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
 * compaction --
 *     Periodically do a compaction operation.
 */
WT_THREAD_RET
compact(void *arg)
{
    WT_CONNECTION *conn;
    WT_DECL_RET;
    WT_SESSION *session;
    u_int period;

    (void)(arg);

    /* Open a session. */
    conn = g.wts_conn;
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /*
     * Perform compaction at somewhere under 15 seconds (so we get at least one done), and then at
     * 23 second intervals.
     */
    for (period = mmrand(NULL, 1, 15);; period = 23) {
        /* Sleep for short periods so we don't make the run wait. */
        while (period > 0 && !g.workers_finished) {
            --period;
            __wt_sleep(1, 0);
        }
        if (g.workers_finished)
            break;

        /*
         * Compact can return EBUSY if concurrent with alter or if there is eviction pressure, or we
         * collide with checkpoints.
         *
         * Compact returns ETIMEDOUT if the compaction doesn't finish in in some number of seconds.
         * We don't configure a timeout and occasionally exceed the default of 1200 seconds.
         */
        ret = session->compact(session, g.uri, NULL);
        if (ret != 0 && ret != EBUSY && ret != ETIMEDOUT && ret != WT_ROLLBACK &&
          ret != WT_CACHE_FULL)
            testutil_die(ret, "session.compact");
    }

    testutil_check(session->close(session, NULL));

    return (WT_THREAD_RET_VALUE);
}
