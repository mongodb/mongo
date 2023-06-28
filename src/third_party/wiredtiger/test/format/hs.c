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
 * hs_cursor --
 *     Run a cursor through the history store, depending on the library order checking code to
 *     detect problems.
 */
WT_THREAD_RET
hs_cursor(void *arg)
{
#if WIREDTIGER_VERSION_MAJOR < 10
    WT_UNUSED(arg);
#else
    SAP sap;
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_ITEM hs_key, hs_value;
    WT_SESSION *session;
    wt_timestamp_t hs_durable_timestamp, hs_start_ts, hs_stop_durable_ts;
    uint64_t hs_counter, hs_upd_type;
    uint32_t hs_btree_id, i;
    u_int period;
    bool next;

    (void)(arg); /* Unused parameter */

    conn = g.wts_conn;

    /*
     * Trigger the internal WiredTiger cursor order checking on the history-store file. Open a
     * cursor on the history-store file, retrieve some records, close cursor, repeat.
     *
     * Open a session.
     */
    memset(&sap, 0, sizeof(sap));
    wt_wrap_open_session(conn, &sap, NULL, &session);

    memset(&hs_key, 0, sizeof(hs_key));
    memset(&hs_value, 0, sizeof(hs_value));
    for (;;) {
        /* Open a HS cursor. */
        testutil_check(__wt_curhs_open((WT_SESSION_IMPL *)session, NULL, &cursor));
        F_SET(cursor, WT_CURSTD_HS_READ_COMMITTED);

        /*
         * Move the cursor through the table from the beginning or the end. We can't position the
         * cursor in the HS store because the semantics of search aren't quite the same as other
         * tables, and we can't correct for them in application code. We don't sleep with an open
         * cursor, so we should be able to traverse large chunks of the HS store quickly, without
         * blocking normal operations.
         */
        next = mmrand(&g.extra_rnd, 0, 1) == 1;
        for (i = mmrand(&g.extra_rnd, WT_THOUSAND, 100 * WT_THOUSAND); i > 0; --i) {
            if ((ret = (next ? cursor->next(cursor) : cursor->prev(cursor))) != 0) {
                testutil_assertfmt(ret == WT_NOTFOUND || ret == WT_CACHE_FULL || ret == WT_ROLLBACK,
                  "WT_CURSOR.%s failed: %d", next ? "next" : "prev", ret);
                break;
            }
            testutil_check(
              cursor->get_key(cursor, &hs_btree_id, &hs_key, &hs_start_ts, &hs_counter));
            testutil_check(cursor->get_value(
              cursor, &hs_stop_durable_ts, &hs_durable_timestamp, &hs_upd_type, &hs_value));
        }

        testutil_check(cursor->close(cursor));

        /* Sleep for some number of seconds, in short intervals so we don't make the run wait. */
        for (period = mmrand(&g.extra_rnd, 1, 10); period > 0 && !g.workers_finished; --period)
            __wt_sleep(1, 0);
        if (g.workers_finished)
            break;
    }

    wt_wrap_close_session(session);
#endif

    return (WT_THREAD_RET_VALUE);
}
