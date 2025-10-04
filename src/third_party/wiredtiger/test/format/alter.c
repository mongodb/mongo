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
 * alter --
 *     Periodically alter a table's metadata.
 */
WT_THREAD_RET
alter(void *arg)
{
    SAP sap;
    TABLE *table;
    WT_CONNECTION *conn;
    WT_DECL_RET;
    WT_SESSION *session;
    u_int counter, period;
    char buf[32];
    bool access_value;

    (void)(arg);

    conn = g.wts_conn;

    /*
     * Only alter the access pattern hint. If we alter the cache resident setting we may end up with
     * a setting that fills cache and doesn't allow it to be evicted.
     */
    access_value = false;

    /* Open a session */
    memset(&sap, 0, sizeof(sap));
    wt_wrap_open_session(conn, &sap, NULL, NULL, &session);
    counter = 0;

    while (!g.workers_finished) {
        period = mmrand(&g.extra_rnd, 1, 10);

        testutil_snprintf(
          buf, sizeof(buf), "access_pattern_hint=%s", access_value ? "random" : "none");
        access_value = !access_value;

        /* Alter can return EBUSY if concurrent with other operations. */
        table = table_select(NULL, false);
        trace_msg(session, "Alter #%u URI %s start %s", ++counter, table->uri, buf);

        while ((ret = session->alter(session, table->uri, buf)) != 0 && ret != EBUSY)
            testutil_die(ret, "session.alter");
        trace_msg(session, "Alter #%u URI %s stop, ret=%d", counter, table->uri, ret);
        while (period > 0 && !g.workers_finished) {
            --period;
            __wt_sleep(1, 0);
        }
    }

    wt_wrap_close_session(session);
    return (WT_THREAD_RET_VALUE);
}
