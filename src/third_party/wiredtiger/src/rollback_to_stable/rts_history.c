/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_rts_history_delete_hs --
 *     Delete the updates for a key in the history store until the first update (including) that is
 *     larger than or equal to the specified timestamp.
 */
int
__wt_rts_history_delete_hs(WT_SESSION_IMPL *session, WT_ITEM *key, wt_timestamp_t ts)
{
    WT_CURSOR *hs_cursor;
    WT_DECL_ITEM(hs_key);
    WT_DECL_RET;
    WT_TIME_WINDOW *hs_tw;
    bool dryrun;

    dryrun = S2C(session)->rts->dryrun;

    /* Open a history store table cursor. */
    WT_RET(__wt_curhs_open(session, NULL, &hs_cursor));
    /*
     * Rollback-to-stable operates exclusively (i.e., it is the only active operation in the system)
     * outside the constraints of transactions. Therefore, there is no need for snapshot based
     * visibility checks.
     */
    F_SET(hs_cursor, WT_CURSTD_HS_READ_COMMITTED);

    WT_ERR(__wt_scr_alloc(session, 0, &hs_key));

    /*
     * Scan the history store for the given btree and key with maximum start timestamp to let the
     * search point to the last version of the key and start traversing backwards to delete all the
     * records until the first update with the start timestamp larger than or equal to the specified
     * timestamp.
     */
    hs_cursor->set_key(hs_cursor, 4, S2BT(session)->id, key, WT_TS_MAX, UINT64_MAX);
    ret = __wt_curhs_search_near_before(session, hs_cursor);
    for (; ret == 0; ret = hs_cursor->prev(hs_cursor)) {
        /* Retrieve the time window from the history cursor. */
        __wt_hs_upd_time_window(hs_cursor, &hs_tw);

        /*
         * Remove all history store versions with a stop timestamp greater than the start/stop
         * timestamp of a stable update in the data store.
         */
        if (hs_tw->stop_ts <= ts)
            break;

        if (!dryrun)
            WT_ERR(hs_cursor->remove(hs_cursor));
        WT_RTS_STAT_CONN_DATA_INCR(session, txn_rts_hs_removed);

        /*
         * The globally visible start time windows are cleared during history store reconciliation.
         * Treat them also as a stable entry removal from the history store.
         */
        if (hs_tw->start_ts == ts || hs_tw->start_ts == WT_TS_NONE)
            WT_RTS_STAT_CONN_DATA_INCR(session, cache_hs_key_truncate_rts);
        else
            WT_RTS_STAT_CONN_DATA_INCR(session, cache_hs_key_truncate_rts_unstable);
    }
    WT_ERR_NOTFOUND_OK(ret, false);

err:
    __wt_scr_free(session, &hs_key);
    WT_TRET(hs_cursor->close(hs_cursor));
    return (ret);
}

/*
 * __wt_rts_history_btree_hs_truncate --
 *     Wipe all history store updates for the btree (non-timestamped tables)
 */
int
__wt_rts_history_btree_hs_truncate(WT_SESSION_IMPL *session, uint32_t btree_id)
{
    WT_CURSOR *hs_cursor_start, *hs_cursor_stop;
    WT_DECL_ITEM(hs_key);
    WT_DECL_RET;
    WT_SESSION *truncate_session;
    wt_timestamp_t hs_start_ts;
    uint64_t hs_counter;
    uint32_t hs_btree_id;
    bool dryrun;

    dryrun = S2C(session)->rts->dryrun;

    hs_cursor_start = hs_cursor_stop = NULL;
    hs_btree_id = 0;
    truncate_session = (WT_SESSION *)session;

    WT_RET(__wt_scr_alloc(session, 0, &hs_key));

    /* Open a history store start cursor. */
    WT_ERR(__wt_curhs_open(session, NULL, &hs_cursor_start));
    F_SET(hs_cursor_start, WT_CURSTD_HS_READ_COMMITTED);

    hs_cursor_start->set_key(hs_cursor_start, 1, btree_id);
    WT_ERR_NOTFOUND_OK(__wt_curhs_search_near_after(session, hs_cursor_start), true);
    if (ret == WT_NOTFOUND) {
        ret = 0;
        goto done;
    }

    /* Open a history store stop cursor. */
    WT_ERR(__wt_curhs_open(session, NULL, &hs_cursor_stop));
    F_SET(hs_cursor_stop, WT_CURSTD_HS_READ_COMMITTED | WT_CURSTD_HS_READ_ACROSS_BTREE);

    hs_cursor_stop->set_key(hs_cursor_stop, 1, btree_id + 1);
    WT_ERR_NOTFOUND_OK(__wt_curhs_search_near_after(session, hs_cursor_stop), true);

#ifdef HAVE_DIAGNOSTIC
    /* If we get not found, we are at the largest btree id in the history store. */
    if (ret == 0) {
        hs_cursor_stop->get_key(hs_cursor_stop, &hs_btree_id, hs_key, &hs_start_ts, &hs_counter);
        WT_ASSERT(session, hs_btree_id > btree_id);
    }
#endif

    do {
        WT_ASSERT(session, ret == WT_NOTFOUND || hs_btree_id > btree_id);

        WT_ERR_NOTFOUND_OK(hs_cursor_stop->prev(hs_cursor_stop), true);
        /* We can find the start point then we must be able to find the stop point. */
        if (ret == WT_NOTFOUND)
            WT_ERR_PANIC(
              session, ret, "cannot locate the stop point to truncate the history store.");
        hs_cursor_stop->get_key(hs_cursor_stop, &hs_btree_id, hs_key, &hs_start_ts, &hs_counter);
    } while (hs_btree_id != btree_id);

    if (!dryrun)
        WT_ERR(truncate_session->truncate(
          truncate_session, NULL, hs_cursor_start, hs_cursor_stop, NULL));

    WT_RTS_STAT_CONN_DATA_INCR(session, cache_hs_btree_truncate);

    __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
      WT_RTS_VERB_TAG_HS_TRUNCATED
      "Rollback to stable has truncated records for btree=%u from the history store",
      btree_id);

done:
err:
    __wt_scr_free(session, &hs_key);
    if (hs_cursor_start != NULL)
        WT_TRET(hs_cursor_start->close(hs_cursor_start));
    if (hs_cursor_stop != NULL)
        WT_TRET(hs_cursor_stop->close(hs_cursor_stop));

    return (ret);
}

/*
 * __wt_rts_history_final_pass --
 *     Perform rollback to stable on the history store to remove any entries newer than the stable
 *     timestamp.
 */
int
__wt_rts_history_final_pass(WT_SESSION_IMPL *session, wt_timestamp_t rollback_timestamp)
{
    WT_CONFIG ckptconf;
    WT_CONFIG_ITEM cval, durableval, key;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    wt_timestamp_t max_durable_ts, newest_stop_durable_ts, newest_stop_ts;
    size_t i;
    char *config;
    char ts_string[2][WT_TS_INT_STRING_SIZE];

    config = NULL;
    conn = S2C(session);

    WT_RET(__wt_metadata_search(session, WT_HS_URI, &config));

    /*
     * Find out the max durable timestamp of the history store from checkpoint. Most of the history
     * store updates have stop timestamp either greater or equal to the start timestamp except for
     * the updates written for the prepared updates on the data store. To abort the updates with no
     * stop timestamp, we must include the newest stop timestamp also into the calculation of
     * maximum timestamp of the history store.
     */
    newest_stop_durable_ts = newest_stop_ts = WT_TS_NONE;
    WT_ERR(__wt_config_getones(session, config, "checkpoint", &cval));
    __wt_config_subinit(session, &ckptconf, &cval);
    for (; __wt_config_next(&ckptconf, &key, &cval) == 0;) {
        ret = __wt_config_subgets(session, &cval, "newest_stop_durable_ts", &durableval);
        if (ret == 0)
            newest_stop_durable_ts = WT_MAX(newest_stop_durable_ts, (wt_timestamp_t)durableval.val);
        WT_ERR_NOTFOUND_OK(ret, false);
        ret = __wt_config_subgets(session, &cval, "newest_stop_ts", &durableval);
        if (ret == 0)
            newest_stop_ts = WT_MAX(newest_stop_ts, (wt_timestamp_t)durableval.val);
        WT_ERR_NOTFOUND_OK(ret, false);
    }
    max_durable_ts = WT_MAX(newest_stop_ts, newest_stop_durable_ts);
    WT_ERR(__wt_session_get_dhandle(session, WT_HS_URI, NULL, NULL, 0));

    /*
     * The rollback operation should be performed on the history store file when the checkpoint
     * durable start/stop timestamp is greater than the rollback timestamp. But skip if there is no
     * stable timestamp.
     *
     * Note that the corresponding code for RTS btree apply also checks whether there _are_
     * timestamped updates by checking max_durable_ts; that check is redundant here for several
     * reasons, the most immediate being that max_durable_ts cannot be none (zero) because it's
     * greater than rollback_timestamp, which is itself greater than zero.
     */
    if (max_durable_ts > rollback_timestamp && rollback_timestamp != WT_TS_NONE) {
        __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
          WT_RTS_VERB_TAG_HS_TREE_ROLLBACK "tree rolled back with durable_timestamp=%s",
          __wt_timestamp_to_string(max_durable_ts, ts_string[0]));
        WT_TRET(__wt_rts_btree_walk_btree(session, rollback_timestamp));
    } else
        __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
          WT_RTS_VERB_TAG_HS_TREE_SKIP
          "tree skipped with durable_timestamp=%s and stable_timestamp=%s",
          __wt_timestamp_to_string(max_durable_ts, ts_string[0]),
          __wt_timestamp_to_string(rollback_timestamp, ts_string[1]));

    /*
     * Truncate history store entries from the partial backup remove list. The list holds all of the
     * btree ids that do not exist as part of the database anymore due to performing a selective
     * restore from backup.
     */
    if (F_ISSET(conn, WT_CONN_BACKUP_PARTIAL_RESTORE) && conn->partial_backup_remove_ids != NULL)
        for (i = 0; conn->partial_backup_remove_ids[i] != 0; ++i)
            WT_ERR(__wt_rts_history_btree_hs_truncate(session, conn->partial_backup_remove_ids[i]));
err:
    if (session->dhandle != NULL)
        WT_TRET(__wt_session_release_dhandle(session));
    __wt_free(session, config);
    return (ret);
}
