/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_rts_check --
 *     Check to the extent possible that the rollback request is reasonable.
 */
int
__wt_rts_check(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_SESSION_IMPL *session_in_list;
    uint32_t i, session_cnt;
    bool cursor_active, txn_active;

    conn = S2C(session);
    cursor_active = txn_active = false;

    WT_STAT_CONN_INCR(session, txn_walk_sessions);

    /*
     * Help the user comply with the requirement there be no concurrent user operations. It is okay
     * to have a transaction in the prepared state.
     *
     * WT_TXN structures are allocated and freed as sessions are activated and closed. Lock the
     * session open/close to ensure we don't race. This call is a rarely used RTS-only function,
     * acquiring the lock shouldn't be an issue.
     */
    __wt_spin_lock(session, &conn->api_lock);

    WT_ORDERED_READ(session_cnt, conn->session_cnt);
    for (i = 0, session_in_list = conn->sessions; i < session_cnt; i++, session_in_list++) {

        /* Skip inactive or internal sessions. */
        if (!session_in_list->active || F_ISSET(session_in_list, WT_SESSION_INTERNAL))
            continue;

        /* Check if a user session has a running transaction. */
        if (F_ISSET(session_in_list->txn, WT_TXN_RUNNING)) {
            txn_active = true;
            break;
        }

        /* Check if a user session has an active file cursor. */
        if (session_in_list->ncursors != 0) {
            cursor_active = true;
            break;
        }
    }
    __wt_spin_unlock(session, &conn->api_lock);

    /*
     * A new cursor may be positioned or a transaction may start after we return from this call and
     * callers should be aware of this limitation.
     */
    if (cursor_active)
        WT_RET_MSG(session, EBUSY, "rollback_to_stable illegal with active file cursors");
    if (txn_active) {
        ret = EBUSY;
        WT_TRET(__wt_verbose_dump_txn(session));
        WT_RET_MSG(session, ret, "rollback_to_stable illegal with active transactions");
    }
    return (0);
}

/*
 * __rts_progress_msg --
 *     Log a verbose message about the progress of the current rollback to stable.
 */
static void
__rts_progress_msg(WT_SESSION_IMPL *session, struct timespec rollback_start,
  uint64_t rollback_count, uint64_t *rollback_msg_count)
{
    struct timespec cur_time;
    uint64_t time_diff;

    __wt_epoch(session, &cur_time);

    /* Time since the rollback started. */
    time_diff = WT_TIMEDIFF_SEC(cur_time, rollback_start);

    if ((time_diff / WT_PROGRESS_MSG_PERIOD) > *rollback_msg_count) {
        __wt_verbose(session, WT_VERB_RECOVERY_PROGRESS,
          "Rollback to stable has been running for %" PRIu64 " seconds and has inspected %" PRIu64
          " files. For more detailed logging, enable WT_VERB_RTS",
          time_diff, rollback_count);
        ++(*rollback_msg_count);
    }
}

/*
 * __wt_rts_btree_apply_all --
 *     Perform rollback to stable to all files listed in the metadata, apart from the metadata and
 *     history store files.
 */
int
__wt_rts_btree_apply_all(WT_SESSION_IMPL *session, wt_timestamp_t rollback_timestamp)
{
    struct timespec rollback_timer;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    uint64_t rollback_count, rollback_msg_count;
    const char *config, *uri;

    /* Initialize the verbose tracking timer. */
    __wt_epoch(session, &rollback_timer);
    rollback_count = 0;
    rollback_msg_count = 0;

    WT_RET(__wt_metadata_cursor(session, &cursor));
    while ((ret = cursor->next(cursor)) == 0) {
        /* Log a progress message. */
        __rts_progress_msg(session, rollback_timer, rollback_count, &rollback_msg_count);
        ++rollback_count;

        WT_ERR(cursor->get_key(cursor, &uri));
        WT_ERR(cursor->get_value(cursor, &config));

        F_SET(session, WT_SESSION_QUIET_CORRUPT_FILE);
        ret = __wt_rts_btree_walk_btree_apply(session, uri, config, rollback_timestamp);
        F_CLR(session, WT_SESSION_QUIET_CORRUPT_FILE);

        /*
         * Ignore rollback to stable failures on files that don't exist or files where corruption is
         * detected.
         */
        if (ret == ENOENT || (ret == WT_ERROR && F_ISSET(S2C(session), WT_CONN_DATA_CORRUPTION))) {
            __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
              WT_RTS_VERB_TAG_SKIP_DAMAGE
              "%s: skipped performing rollback to stable because the file %s",
              uri, ret == ENOENT ? "does not exist" : "is corrupted.");
            continue;
        }
        WT_ERR(ret);
    }
    WT_ERR_NOTFOUND_OK(ret, false);

    if (F_ISSET(S2C(session), WT_CONN_RECOVERING))
        WT_ERR(__wt_rts_history_final_pass(session, rollback_timestamp));

err:
    WT_TRET(__wt_metadata_cursor_release(session, &cursor));
    return (ret);
}
