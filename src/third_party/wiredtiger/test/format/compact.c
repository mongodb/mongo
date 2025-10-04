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
 * background_compact --
 *     Periodically enable/disable the background compaction thread.
 */
WT_THREAD_RET
background_compact(void *arg)
{
    SAP sap;
    WT_CONNECTION *conn;
    WT_DECL_RET;
    WT_SESSION *session;
    u_int period;
    char config_buf[128];

    (void)(arg);

    conn = g.wts_conn;

    /* Open a session. */
    memset(&sap, 0, sizeof(sap));
    wt_wrap_open_session(conn, &sap, NULL, NULL, &session);

    /*
     * Start the background compaction server at somewhere under 5 seconds, and then enable/disable
     * it every 10 minutes.
     */
    for (period = mmrand(&g.extra_rnd, 1, 5);; period = 600) {
        /* Sleep for short periods so we don't make the run wait. */
        while (period > 0 && !g.workers_finished) {
            --period;
            __wt_sleep(1, 0);
        }
        if (g.workers_finished)
            break;

        /*
         * The API supports enabling or disabling the background compact server multiple times in a
         * row. Randomly pick whether we are enabling or disabling to cover all state changes.
         */
        if (mmrand(&g.extra_rnd, 0, 1))
            testutil_snprintf(config_buf, sizeof(config_buf),
              "background=true,free_space_target=%" PRIu32 "MB",
              GV(BACKGROUND_COMPACT_FREE_SPACE_TARGET));
        else
            testutil_snprintf(config_buf, sizeof(config_buf), "%s", "background=false");

        ret = session->compact(session, NULL, config_buf);
        if (ret != 0)
            testutil_assertfmt(ret == EBUSY, "WT_SESSION.compact failed: %d", ret);
    }

    /* Always disable the background compaction server. */
    ret = session->compact(session, NULL, "background=false");
    if (ret != 0)
        testutil_assertfmt(ret == EBUSY, "WT_SESSION.compact failed: %d", ret);

    wt_wrap_close_session(session);

    return (WT_THREAD_RET_VALUE);
}

/*
 * compact --
 *     Periodically do a compaction operation.
 */
WT_THREAD_RET
compact(void *arg)
{
    SAP sap;
    TABLE *table;
    WT_CONNECTION *conn;
    WT_DECL_RET;
    WT_SESSION *session;
    u_int period;
    char config_buf[128];

    (void)(arg);

    conn = g.wts_conn;

    /* Open a session. */
    memset(&sap, 0, sizeof(sap));
    wt_wrap_open_session(conn, &sap, NULL, NULL, &session);

    /*
     * Perform compaction at somewhere under 15 seconds (so we get at least one done), and then at
     * 23 second intervals.
     */
    for (period = mmrand(&g.extra_rnd, 1, 15);; period = 23) {
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
         * Compact returns ETIMEDOUT if the compaction doesn't finish in some number of seconds. We
         * don't configure a timeout and occasionally exceed the default of 1200 seconds.
         */
        table = table_select(NULL, false);
        testutil_snprintf(config_buf, sizeof(config_buf), "free_space_target=%" PRIu32 "MB",
          GV(COMPACT_FREE_SPACE_TARGET));
        ret = session->compact(session, table->uri, config_buf);
        testutil_assertfmt(ret == 0 || ret == EBUSY || ret == ETIMEDOUT || ret == WT_CACHE_FULL ||
            ret == WT_ROLLBACK,
          "WT_SESSION.compact failed: %s: %d", table->uri, ret);
    }

    wt_wrap_close_session(session);

    return (WT_THREAD_RET_VALUE);
}
