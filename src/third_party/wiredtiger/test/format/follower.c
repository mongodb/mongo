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
 * follower_read_latest_checkpoint --
 *     Read the latest checkpoint. Only followers should be able to do so.
 */
void
follower_read_latest_checkpoint(void)
{
    SAP sap;
    WT_CONNECTION *conn;
    WT_DECL_RET;
    WT_PAGE_LOG *page_log;
    WT_SESSION *session;
    const char *disagg_page_log;
    WT_ITEM checkpoint_metadata;
    uint64_t checkpoint_ts;
    char config[128];

    conn = g.wts_conn;
    disagg_page_log = (char *)GVS(DISAGG_PAGE_LOG);
    memset(&checkpoint_metadata, 0, sizeof(checkpoint_metadata));

    /* Only follower can pickup checkpoints. */
    testutil_assert(!g.disagg_leader);
    testutil_check(conn->get_page_log(conn, disagg_page_log, &page_log));

    wt_wrap_open_session(conn, &sap, NULL, NULL, &session);
    ret = page_log->pl_get_complete_checkpoint_ext(
      page_log, session, NULL, NULL, &checkpoint_ts, &checkpoint_metadata);
    testutil_check_error_ok(ret, WT_NOTFOUND);
    if (ret != WT_NOTFOUND) {
        testutil_snprintf(config, sizeof(config), "disaggregated=(checkpoint_meta=\"%.*s\")",
          (int)checkpoint_metadata.size, (const char *)checkpoint_metadata.data);
        testutil_check(conn->reconfigure(conn, config));
        printf("--- [Follower] Picked up checkpoint (metadata=[%.*s],timestamp(hex)=%" PRIx64
               ") ---\n",
          (int)checkpoint_metadata.size, (const char *)checkpoint_metadata.data, checkpoint_ts);
    }
    free(checkpoint_metadata.mem);
    wt_wrap_close_session(session);
    testutil_check(page_log->terminate(page_log, NULL));
}

/*
 * follower --
 *     Periodically check for a new checkpoint from the leader, and reconfigure to use it.
 */
WT_THREAD_RET
follower(void *arg)
{
    SAP sap;
    WT_CONNECTION *conn;
    WT_DECL_RET;
    WT_ITEM checkpoint_metadata;
    WT_PAGE_LOG *page_log;
    WT_SESSION *session;
    const char *disagg_page_log;
    char config[128];
    u_int period;
    uint64_t checkpoint_ts;

    (void)(arg); /* Unused parameter */
    conn = g.wts_conn;
    disagg_page_log = (char *)GVS(DISAGG_PAGE_LOG);
    memset(&sap, 0, sizeof(sap));
    memset(&checkpoint_metadata, 0, sizeof(checkpoint_metadata));

    wt_wrap_open_session(conn, &sap, NULL, NULL, &session);
    testutil_check(conn->get_page_log(conn, disagg_page_log, &page_log));

    while (!g.workers_finished) {
        /*
         * FIXME-WT-15788: Eventually have the leader send checkpoint metadata to the follower (via
         * shared memory or pipe) so it can be picked up. Required once we start running against the
         * library version of PALI, which doesn't implement pl_get_complete_checkpoint_ext().
         */
        ret = page_log->pl_get_complete_checkpoint_ext(
          page_log, session, NULL, NULL, &checkpoint_ts, &checkpoint_metadata);
        testutil_check_error_ok(ret, WT_NOTFOUND);
        /* Only reconfigure if there's a new checkpoint. */
        if (ret != WT_NOTFOUND) {
            if (g.checkpoint_metadata[0] == '\0' ||
              memcmp(g.checkpoint_metadata, (const char *)checkpoint_metadata.data,
                checkpoint_metadata.size) != 0) {
                testutil_snprintf(config, sizeof(config),
                  "disaggregated=(checkpoint_meta=\"%.*s\")", (int)checkpoint_metadata.size,
                  (const char *)checkpoint_metadata.data);
                testutil_check(conn->reconfigure(conn, config));
                printf(
                  "--- [Follower] Picked up checkpoint (metadata=[%.*s],timestamp(hex)=%" PRIx64
                  ") ---\n",
                  (int)checkpoint_metadata.size, (const char *)checkpoint_metadata.data,
                  checkpoint_ts);
                testutil_snprintf(g.checkpoint_metadata, sizeof(g.checkpoint_metadata), "%.*s",
                  (int)checkpoint_metadata.size, (const char *)checkpoint_metadata.data);
            }
        }
        period = mmrand(&g.extra_rnd, 1, 3);
        while (period > 0 && !g.workers_finished) {
            --period;
            __wt_sleep(1, 0);
        }
    }
    free(checkpoint_metadata.mem);
    wt_wrap_close_session(session);
    testutil_check(page_log->terminate(page_log, NULL));

    return (WT_THREAD_RET_VALUE);
}
