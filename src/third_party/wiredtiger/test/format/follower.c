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
int
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
  WT_ITEM *checkpoint_metadata, wt_timestamp_t checkpoint_ts, bool set_connection_timestamps)
{
    WT_DISAGG_METADATA metadata;
    WT_ITEM full_metadata;
    wt_timestamp_t pinned_ts, replayed_ts;
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
     * The checkpoint_metadata from pl_get_complete_checkpoint() only contains pointer information
     * (metadata_lsn, etc.). We need to fetch the actual metadata page from the page log to get the
     * full checkpoint config with oldest_timestamp.
     */
    testutil_assert(g.transaction_timestamps_config);
    testutil_check(
      follower_fetch_full_metadata(session, page_log, checkpoint_metadata, &full_metadata));
    testutil_check(__wt_disagg_parse_meta((WT_SESSION_IMPL *)session, &full_metadata, &metadata));
    testutil_assert(metadata.oldest_timestamp != WT_TS_NONE);
    /*
     * Checkpoint metadata may only be delivered once its content has been replayed, so wait for
     * replay to reach the checkpoint's timestamp; the stable timestamp is no substitute, advancing
     * on the replay schedule rather than with application. The watermark only exists under
     * predictable replay.
     */
    if (!set_connection_timestamps && GV(RUNS_PREDICTABLE_REPLAY)) {
        replayed_ts = replay_maximum_committed();
        if (replayed_ts < checkpoint_ts) {
            printf("--- [Follower] Skipping checkpoint pickup: checkpoint_timestamp(hex)=%" PRIx64
                   " > replayed_timestamp(hex)=%" PRIx64 " ---\n",
              checkpoint_ts, replayed_ts);
            goto done;
        }
    }

    testutil_check(timestamp_query("get=pinned", &pinned_ts));
    if (pinned_ts != WT_TS_NONE && metadata.oldest_timestamp > pinned_ts) {
        printf("--- [Follower] Skipping checkpoint pickup: oldest_timestamp(hex)=%" PRIx64
               " > pinned_timestamp(hex)=%" PRIx64 " ---\n",
          metadata.oldest_timestamp, pinned_ts);
        goto done;
    }

    testutil_snprintf(config, sizeof(config), "disaggregated=(checkpoint_meta=\"%.*s\")",
      (int)checkpoint_metadata->size, (const char *)checkpoint_metadata->data);
    testutil_check(conn->reconfigure(conn, config));
    printf("--- [Follower] Picked up checkpoint (metadata=[%.*s],timestamp=%#" PRIx64 ") ---\n",
      (int)checkpoint_metadata->size, (const char *)checkpoint_metadata->data, checkpoint_ts);
    if (set_connection_timestamps) {
        testutil_snprintf(config, sizeof(config),
          "oldest_timestamp=%" PRIx64 ",stable_timestamp=%" PRIx64, metadata.oldest_timestamp,
          checkpoint_ts);
        testutil_check(conn->set_timestamp(conn, config));
    }
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
    WT_PAGE_LOG *page_log;
    WT_PAGE_LOG_GET_COMPLETE_CHECKPOINT_ARGS args;
    WT_SESSION *session;
    const char *disagg_page_log;

    conn = g.wts_conn;
    disagg_page_log = (char *)GVS(DISAGG_PAGE_LOG);
    memset(&sap, 0, sizeof(sap));
    memset(&args, 0, sizeof(args));

    /* Only follower can pickup checkpoints. */
    testutil_assert(!g.disagg_leader);
    testutil_check(conn->get_page_log(conn, disagg_page_log, &page_log));

    wt_wrap_open_session(conn, &sap, NULL, NULL, &session);
    ret = page_log->pl_get_complete_checkpoint(page_log, session, &args);
    testutil_check_error_ok(ret, WT_NOTFOUND);
    if (ret != WT_NOTFOUND)
        (void)follower_try_pickup_checkpoint(session, conn, page_log, &args.checkpoint_metadata,
          args.checkpoint_timestamp, g.reopen && disagg_is_multi_node());

    free(args.checkpoint_metadata.mem);
    wt_wrap_close_session(session);
    testutil_check(page_log->terminate(page_log, NULL));
}

/*
 * follower_read_no_ts --
 *     Repeatedly run transactional snapshot reads with no read timestamp on the follower, racing
 *     checkpoint pickups: scan a table's first rows twice within one transaction, through freshly
 *     opened cursors, and fail on any difference. The snapshot must observe exactly one state for
 *     its lifetime; a refused read (rollback) is an acceptable outcome and retried.
 */
WT_THREAD_RET
follower_read_no_ts(void *arg)
{
    SAP sap;
    WT_CONNECTION *conn;
    WT_DECL_RET;
    WT_ITEM keys[FOLLOWER_READ_ROWS], values[FOLLOWER_READ_ROWS];
    WT_SESSION *session;
    uint64_t iterations;
    u_int i;

    (void)(arg); /* Unused parameter */
    conn = g.wts_conn;
    memset(keys, 0, sizeof(keys));
    memset(values, 0, sizeof(values));

    /* Restrict to row-store, like the random cursor reader. */
    for (i = 0; i <= ntables; ++i)
        if (tables[i] != NULL && tables[i]->type == ROW)
            break;
    if (i > ntables)
        return (WT_THREAD_RET_VALUE);

    WT_CLEAR(sap);
    wt_wrap_open_session(conn, &sap, NULL, NULL, &session);

    printf("--- [Follower] snapshot read stress running ---\n");
    for (iterations = 0; !g.workers_finished; ++iterations) {
        TABLE *table = table_select_type(ROW, false);
        WT_ITEM start_key;
        u_int count = 0;
        bool failed = false;

        if (table == NULL)
            break;
        testutil_check(session->begin_transaction(session, "isolation=snapshot"));

        /* Scan from a random position, so updates anywhere in the table are candidates. */
        key_gen_init(&start_key);
        key_gen(table, &start_key, mmrand(&g.extra_rnd, 1, table->rows_current));

        for (u_int pass = 0; pass < FOLLOWER_READ_PASSES && !failed && !g.workers_finished;
          ++pass) {
            WT_CURSOR *cursor;
            int exact;

            /*
             * Hold the snapshot across pickups: the first pass records the baseline and the later
             * passes re-read it every few hundred milliseconds, so most transactions span an
             * adoption and every scanned row is a candidate to catch a leaked change.
             */
            if (pass > 0)
                __wt_sleep(0, mmrand(&g.extra_rnd, 200, 400) * WT_THOUSAND);
            wt_wrap_open_cursor(session, table->uri, NULL, &cursor);
            cursor->set_key(cursor, &start_key);
            if ((ret = cursor->search_near(cursor, &exact)) != 0) {
                testutil_assertfmt(ret == WT_NOTFOUND || ret == WT_ROLLBACK ||
                    ret == WT_PREPARE_CONFLICT || ret == WT_CACHE_FULL,
                  "follower_read_no_ts: search_near: %d", ret);
                failed = true;
                testutil_check(cursor->close(cursor));
                break;
            }
            /*
             * A near search may legally position on either neighbor of the search key, and the side
             * it picks can change when the underlying trees are reshaped without any visible
             * change, such as by a checkpoint pickup. Anchor every pass on the first visible key at
             * or after the start position so the passes are comparable.
             */
            if (exact < 0 && (ret = cursor->next(cursor)) != 0) {
                testutil_assertfmt(ret == WT_NOTFOUND || ret == WT_ROLLBACK ||
                    ret == WT_PREPARE_CONFLICT || ret == WT_CACHE_FULL,
                  "follower_read_no_ts: next: %d", ret);
                if (ret == WT_NOTFOUND) {
                    /* No rows at or after the start position: every pass must agree on that. */
                    testutil_assertfmt(pass == 0 || count == 0,
                      "follower_read_no_ts: snapshot row count changed within a transaction "
                      "(0 != %u)",
                      count);
                } else
                    failed = true;
                testutil_check(cursor->close(cursor));
                continue;
            }
            for (i = 0; i < FOLLOWER_READ_ROWS; ++i) {
                if (i > 0 && (ret = cursor->next(cursor)) != 0) {
                    testutil_assertfmt(ret == WT_NOTFOUND || ret == WT_ROLLBACK ||
                        ret == WT_PREPARE_CONFLICT || ret == WT_CACHE_FULL,
                      "follower_read_no_ts: next: %d", ret);
                    /* A refusal or conflict abandons the transaction; it is not a failure. */
                    failed = ret != WT_NOTFOUND;
                    if (ret == WT_NOTFOUND && pass > 0)
                        testutil_assertfmt(i == count,
                          "follower_read_no_ts: snapshot row count changed within a transaction "
                          "(%u != %u)",
                          i, count);
                    break;
                }
                WT_ITEM key, value;

                testutil_check(cursor->get_key(cursor, &key));
                testutil_check(cursor->get_value(cursor, &value));
                if (pass == 0) {
                    testutil_check(
                      __wt_buf_set((WT_SESSION_IMPL *)session, &keys[i], key.data, key.size));
                    testutil_check(
                      __wt_buf_set((WT_SESSION_IMPL *)session, &values[i], value.data, value.size));
                    count = i + 1;
                } else {
                    /* The second pass must observe exactly the first pass's rows. */
                    testutil_assertfmt(i < count,
                      "follower_read_no_ts: snapshot row count changed within a transaction (more "
                      "than %u rows)",
                      count);
                    testutil_assertfmt(
                      key.size == keys[i].size && memcmp(key.data, keys[i].data, key.size) == 0,
                      "follower_read_no_ts: snapshot key changed within a transaction (row %u)", i);
                    testutil_assertfmt(value.size == values[i].size &&
                        memcmp(value.data, values[i].data, value.size) == 0,
                      "follower_read_no_ts: snapshot value changed within a transaction (row %u)",
                      i);
                }
            }
            testutil_check(cursor->close(cursor));
        }

        /*
         * Repeat the reads as point lookups: within the same snapshot, searching each key seen by
         * the scan must return the identical value. Point reads and scans position differently, so
         * both paths are verified.
         */
        if (!failed && count > 0 && !g.workers_finished) {
            WT_CURSOR *cursor;
            WT_ITEM value;

            wt_wrap_open_cursor(session, table->uri, NULL, &cursor);
            for (i = 0; i < count; ++i) {
                cursor->set_key(cursor, &keys[i]);
                if ((ret = cursor->search(cursor)) != 0) {
                    testutil_assertfmt(
                      ret == WT_ROLLBACK || ret == WT_PREPARE_CONFLICT || ret == WT_CACHE_FULL,
                      "follower_read_no_ts: a key seen by the snapshot disappeared on search (row "
                      "%u): "
                      "%d",
                      i, ret);
                    break;
                }
                testutil_check(cursor->get_value(cursor, &value));
                testutil_assertfmt(value.size == values[i].size &&
                    memcmp(value.data, values[i].data, value.size) == 0,
                  "follower_read_no_ts: snapshot value changed between a scan and a search (row "
                  "%u)",
                  i);
            }
            testutil_check(cursor->close(cursor));
        }
        key_gen_teardown(&start_key);
        testutil_check(session->rollback_transaction(session, NULL));
    }
    /*
     * Report whether the intended race ran: how many pick-ups the readers deferred and how many
     * were adopted. Configurations with long checkpoint intervals or short timers may legitimately
     * record zero, so this is diagnostic rather than asserted.
     */
    {
        WT_CURSOR *stat_cursor;
        int64_t adopted, deferred;

        wt_wrap_open_cursor(session, "statistics:", NULL, &stat_cursor);
        stat_cursor->set_key(stat_cursor, WT_STAT_CONN_DISAGG_CHECKPOINT_DEFER);
        testutil_check(stat_cursor->search(stat_cursor));
        testutil_check(stat_cursor->get_value(stat_cursor, NULL, NULL, &deferred));
        stat_cursor->set_key(stat_cursor, WT_STAT_CONN_DISAGG_CHECKPOINT_META_LSN);
        testutil_check(stat_cursor->search(stat_cursor));
        testutil_check(stat_cursor->get_value(stat_cursor, NULL, NULL, &adopted));
        testutil_check(stat_cursor->close(stat_cursor));
        printf("--- [Follower] snapshot read stress: %" PRIu64 " transactions, %" PRId64
               " pick-ups deferred, adopted LSN %" PRId64 " ---\n",
          iterations, deferred, adopted);
    }

    for (i = 0; i < FOLLOWER_READ_ROWS; ++i) {
        __wt_buf_free((WT_SESSION_IMPL *)session, &keys[i]);
        __wt_buf_free((WT_SESSION_IMPL *)session, &values[i]);
    }
    wt_wrap_close_session(session);
    return (WT_THREAD_RET_VALUE);
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
    WT_PAGE_LOG *page_log;
    WT_PAGE_LOG_GET_COMPLETE_CHECKPOINT_ARGS args;
    WT_SESSION *session;
    const char *disagg_page_log;
    u_int period;

    (void)(arg); /* Unused parameter */
    conn = g.wts_conn;
    disagg_page_log = (char *)GVS(DISAGG_PAGE_LOG);
    memset(&sap, 0, sizeof(sap));
    memset(&args, 0, sizeof(args));

    wt_wrap_open_session(conn, &sap, NULL, NULL, &session);
    testutil_check(conn->get_page_log(conn, disagg_page_log, &page_log));

    while (!g.workers_finished) {
        /*
         * FIXME-WT-15788: Eventually have the leader send checkpoint metadata to the follower (via
         * shared memory or pipe) so it can be picked up. Required once we start running against the
         * library version of PALI, which doesn't implement pl_get_complete_checkpoint().
         */
        free(args.checkpoint_metadata.mem);
        memset(&args, 0, sizeof(args));
        ret = page_log->pl_get_complete_checkpoint(page_log, session, &args);
        testutil_check_error_ok(ret, WT_NOTFOUND);
        /* Only reconfigure if there's a new checkpoint. */
        if (ret != WT_NOTFOUND) {
            if (g.checkpoint_metadata[0] == '\0' ||
              memcmp(g.checkpoint_metadata, (const char *)args.checkpoint_metadata.data,
                args.checkpoint_metadata.size) != 0) {
                if (follower_try_pickup_checkpoint(session, conn, page_log,
                      &args.checkpoint_metadata, args.checkpoint_timestamp, false))
                    testutil_snprintf(g.checkpoint_metadata, sizeof(g.checkpoint_metadata), "%.*s",
                      (int)args.checkpoint_metadata.size,
                      (const char *)args.checkpoint_metadata.data);
            }
        }
        period = mmrand(&g.extra_rnd, 1, 3);
        while (period > 0 && !g.workers_finished) {
            --period;
            __wt_sleep(1, 0);
        }
    }
    free(args.checkpoint_metadata.mem);
    wt_wrap_close_session(session);
    testutil_check(page_log->terminate(page_log, NULL));

    return (WT_THREAD_RET_VALUE);
}
