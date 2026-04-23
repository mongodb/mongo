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
 * follower_fetch_full_metadata --
 *     Fetch the full checkpoint metadata from the page log.
 */
static int
follower_fetch_full_metadata(WT_SESSION *session, WT_PAGE_LOG *page_log,
  const WT_ITEM *checkpoint_metadata, WT_ITEM *full_metadata)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    WT_PAGE_LOG_GET_ARGS get_args;
    WT_PAGE_LOG_HANDLE *plh;
    uint64_t metadata_lsn;
    uint32_t count;
    char *meta_str;

    meta_str = NULL;
    plh = NULL;
    memset(full_metadata, 0, sizeof(*full_metadata));

    /* Extract the checkpoint_metadata into a null-terminated string for config parsing. */
    WT_ERR(__wt_strndup(
      (WT_SESSION_IMPL *)session, checkpoint_metadata->data, checkpoint_metadata->size, &meta_str));

    /* Extract the metadata_lsn from the checkpoint_metadata. */
    WT_ERR(__wt_config_getones((WT_SESSION_IMPL *)session, meta_str, "metadata_lsn", &cval));
    metadata_lsn = (uint64_t)cval.val;

    /* Open a handle for the metadata table. */
    WT_ERR(page_log->pl_open_handle(page_log, session, WT_SPECIAL_PALI_TURTLE_FILE_ID, &plh));

    /* Read the metadata page at the specified LSN. */
    memset(&get_args, 0, sizeof(get_args));
    get_args.lsn = metadata_lsn;
    count = 1;
    WT_ERR(plh->plh_get(
      plh, session, WT_DISAGG_METADATA_MAIN_PAGE_ID, 0, &get_args, full_metadata, &count));

    if (count == 0) {
        ret = WT_NOTFOUND;
        goto err;
    }

err:
    if (plh != NULL)
        testutil_check(plh->plh_close(plh, session));
    __wt_free((WT_SESSION_IMPL *)session, meta_str);
    return (ret);
}

/*
 * follower_try_pickup_checkpoint --
 *     Attempt to pick up a checkpoint. Returns true if the checkpoint was picked up, false if
 *     skipped due to timestamp constraints (checkpoint's oldest timestamp > follower's
 *     pinned_timestamp).
 */
static bool
follower_try_pickup_checkpoint(WT_SESSION *session, WT_CONNECTION *conn, WT_PAGE_LOG *page_log,
  WT_ITEM *checkpoint_metadata, uint64_t checkpoint_ts)
{
    WT_DISAGG_METADATA metadata;
    WT_ITEM full_metadata;
    uint64_t pinned_ts;
    char config[1024];
    bool picked_up;

    picked_up = false;
    memset(&full_metadata, 0, sizeof(full_metadata));

    /*
     * Before picking up the checkpoint, compare the checkpoint's oldest timestamp with the
     * follower's current pinned timestamp. If the checkpoint's oldest timestamp is greater than the
     * pinned timestamp, we cannot safely pick up this checkpoint yet - skip it and wait for the
     * next attempt when timestamps have caught up.
     *
     * The checkpoint_metadata from pl_get_complete_checkpoint_ext() only contains pointer
     * information (metadata_lsn, etc.). We need to fetch the actual metadata page from the page log
     * to get the full checkpoint config with oldest_timestamp.
     */
    testutil_assert(g.transaction_timestamps_config);
    testutil_check(
      follower_fetch_full_metadata(session, page_log, checkpoint_metadata, &full_metadata));
    testutil_check(__wt_disagg_parse_meta((WT_SESSION_IMPL *)session, &full_metadata, &metadata));
    testutil_assert(metadata.oldest_timestamp != WT_TS_NONE);
    testutil_check(timestamp_query("get=pinned", &pinned_ts));
    if (pinned_ts != 0 && metadata.oldest_timestamp > pinned_ts) {
        printf("--- [Follower] Skipping checkpoint pickup: oldest_timestamp(hex)=%" PRIx64
               " > pinned_timestamp(hex)=%" PRIx64 " ---\n",
          metadata.oldest_timestamp, pinned_ts);
        goto done;
    }

    testutil_snprintf(config, sizeof(config), "disaggregated=(checkpoint_meta=\"%.*s\")",
      (int)checkpoint_metadata->size, (const char *)checkpoint_metadata->data);
    testutil_check(conn->reconfigure(conn, config));
    printf("--- [Follower] Picked up checkpoint (metadata=[%.*s],timestamp(hex)=%" PRIx64 ") ---\n",
      (int)checkpoint_metadata->size, (const char *)checkpoint_metadata->data, checkpoint_ts);
    picked_up = true;

done:
    free(full_metadata.mem);
    return (picked_up);
}

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
    WT_ITEM checkpoint_metadata;
    WT_PAGE_LOG *page_log;
    WT_SESSION *session;
    const char *disagg_page_log;
    uint64_t checkpoint_ts;

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
    if (ret != WT_NOTFOUND)
        (void)follower_try_pickup_checkpoint(
          session, conn, page_log, &checkpoint_metadata, checkpoint_ts);

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
                if (follower_try_pickup_checkpoint(
                      session, conn, page_log, &checkpoint_metadata, checkpoint_ts))
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
