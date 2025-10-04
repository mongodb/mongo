/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __rts_check_callback --
 *     Check if a single session has an active transaction or open cursors. Callback from the
 *     session array walk.
 */
static int
__rts_check_callback(
  WT_SESSION_IMPL *session, WT_SESSION_IMPL *array_session, bool *exit_walkp, void *cookiep)
{
    WT_RTS_COOKIE *cookie;

    WT_UNUSED(session);
    cookie = (WT_RTS_COOKIE *)cookiep;

    /* Check if a user session has a running transaction. */
    if (F_ISSET(array_session->txn, WT_TXN_RUNNING)) {
        cookie->ret_txn_active = true;
        *exit_walkp = true;
    } else if (array_session->ncursors != 0) {
        /* Check if a user session has an active file cursor. */
        cookie->ret_cursor_active = true;
        *exit_walkp = true;
    }
    return (0);
}
/*
 * __rts_check --
 *     Check to the extent possible that the rollback request is reasonable.
 */
static int
__rts_check(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_RTS_COOKIE cookie;

    WT_CLEAR(cookie);
    conn = S2C(session);

    WT_STAT_CONN_INCR(session, txn_walk_sessions);

    /*
     * Help the user to comply with the requirement that there are no concurrent user operations.
     *
     * WT_TXN structures are allocated and freed as sessions are activated and closed. Lock the
     * session open/close to ensure we don't race. This call is a rarely used RTS-only function,
     * acquiring the lock shouldn't be an issue.
     */
    __wt_spin_lock(session, &conn->api_lock);
    WT_IGNORE_RET(__wt_session_array_walk(session, __rts_check_callback, true, &cookie));
    __wt_spin_unlock(session, &conn->api_lock);

    /*
     * A new cursor may be positioned or a transaction may start after we return from this call and
     * callers should be aware of this limitation.
     */
    if (cookie.ret_cursor_active) {
        ret = EBUSY;
        WT_TRET(__wt_verbose_dump_sessions(session, true));
        WT_RET_MSG(session, ret, "rollback_to_stable illegal with active file cursors");
    }
    if (cookie.ret_txn_active) {
        ret = EBUSY;
        WT_TRET(__wt_verbose_dump_sessions(session, false));
        WT_RET_MSG(session, ret, "rollback_to_stable illegal with active transactions");
    }
    return (0);
}
/*
 * __rts_assert_timestamps_unchanged --
 *     Wrapper for some diagnostic assertions related to global timestamps.
 */
static void
__rts_assert_timestamps_unchanged(
  WT_SESSION_IMPL *session, wt_timestamp_t old_pinned, wt_timestamp_t old_stable)
{
#ifdef HAVE_DIAGNOSTIC
    WT_ASSERT(session, S2C(session)->txn_global.pinned_timestamp == old_pinned);
    WT_ASSERT(session, S2C(session)->txn_global.stable_timestamp == old_stable);
#else
    WT_UNUSED(session);
    WT_UNUSED(old_pinned);
    WT_UNUSED(old_stable);
#endif
}

/*
 * __rollback_to_stable_int --
 *     Rollback all modifications with timestamps more recent than the passed in timestamp.
 */
static int
__rollback_to_stable_int(WT_SESSION_IMPL *session, bool no_ckpt)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_TXN_GLOBAL *txn_global;
    wt_timestamp_t pinned_timestamp, rollback_timestamp, stable_timestamp;
    uint32_t threads;
    char ts_string[2][WT_TS_INT_STRING_SIZE];
    bool dryrun;

    conn = S2C(session);
    txn_global = &conn->txn_global;
    dryrun = conn->rts->dryrun;
    threads = conn->rts->threads_num;

    WT_ASSERT_SPINLOCK_OWNED(session, &conn->checkpoint_lock);
    WT_ASSERT_SPINLOCK_OWNED(session, &conn->schema_lock);

    /*
     * Rollback to stable should ignore tombstones in the history store since it needs to scan the
     * entire table sequentially.
     */
    F_SET(session, WT_SESSION_ROLLBACK_TO_STABLE);

    WT_ERR(__rts_check(session));

    /*
     * Update the global time window state to have consistent view from global visibility rules for
     * the rollback to stable to bring back the database into a consistent state.
     *
     * As part of the below function call, the oldest transaction id and pinned timestamps are
     * updated.
     */
    WT_ERR(__wt_txn_update_oldest(session, WT_TXN_OLDEST_STRICT | WT_TXN_OLDEST_WAIT));

    WT_ASSERT_ALWAYS(session,
      (txn_global->has_pinned_timestamp ||
        !__wt_atomic_loadbool(&txn_global->has_oldest_timestamp)),
      "Database has no pinned timestamp but an oldest timestamp. Pinned timestamp is required to "
      "find out the global visibility/obsolete of an update.");

    /*
     * Copy the stable timestamp, otherwise we'd need to lock it each time it's accessed. Even
     * though the stable timestamp isn't supposed to be updated while rolling back, accessing it
     * without a lock would violate protocol.
     */
    WT_ACQUIRE_READ_WITH_BARRIER(stable_timestamp, txn_global->stable_timestamp);
    WT_ACQUIRE_READ_WITH_BARRIER(pinned_timestamp, txn_global->pinned_timestamp);
    __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
      WT_RTS_VERB_TAG_INIT
      "start rollback to stable with stable_timestamp=%s and oldest_timestamp=%s using %u worker "
      "threads",
      __wt_timestamp_to_string(stable_timestamp, ts_string[0]),
      __wt_timestamp_to_string(txn_global->oldest_timestamp, ts_string[1]), threads);

    /* If the stable timestamp is not set, do not roll back based on it. */
    if (stable_timestamp != WT_TS_NONE)
        rollback_timestamp = stable_timestamp;
    else {
        rollback_timestamp = WT_TS_MAX;
        __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session), WT_RTS_VERB_TAG_NO_STABLE "%s",
          "the stable timestamp is not set; set the rollback timestamp to the maximum timestamp");
    }

    if (F_ISSET(conn, WT_CONN_RECOVERING))
        __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
          WT_RTS_VERB_TAG_RECOVER_CKPT "recovered checkpoint snapshot_min=%" PRIu64
                                       ", snapshot_max=%" PRIu64 ", snapshot_count=%" PRIu32,
          conn->recovery_ckpt_snap_min, conn->recovery_ckpt_snap_max,
          conn->recovery_ckpt_snapshot_count);

    WT_ERR(__wti_rts_btree_apply_all(session, rollback_timestamp));

    /* Rollback the global durable timestamp to the stable timestamp. */
    if (!dryrun) {
        txn_global->has_durable_timestamp = txn_global->has_stable_timestamp;
        txn_global->durable_timestamp = txn_global->stable_timestamp;
    }
    __rts_assert_timestamps_unchanged(session, pinned_timestamp, stable_timestamp);

    /*
     * If the configuration is not in-memory, forcibly log a checkpoint after rollback to stable to
     * ensure that both in-memory and on-disk versions are the same unless caller requested for no
     * checkpoint.
     */
    if (!F_ISSET(conn, WT_CONN_IN_MEMORY) && !no_ckpt && !dryrun)
        WT_ERR(session->iface.checkpoint(&session->iface, "force=1"));

err:
    F_CLR(session, WT_SESSION_ROLLBACK_TO_STABLE);
    return (ret);
}

/*
 * __rollback_to_stable_one --
 *     Perform rollback to stable on a single object.
 */
static int
__rollback_to_stable_one(WT_SESSION_IMPL *session, const char *uri, bool *skipp)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_TIMER timer;
    wt_timestamp_t pinned_timestamp, rollback_timestamp, stable_timestamp;
    uint64_t time_diff_ms;
    char *config;

    conn = S2C(session);

    /*
     * This is confusing: the caller's boolean argument "skip" stops the schema-worker loop from
     * processing this object and any underlying objects it may have (for example, a table with
     * multiple underlying file objects). We rollback-to-stable all of the file objects an object
     * may contain, so set the caller's skip argument to true on all file objects, else set the
     * caller's skip argument to false so our caller continues down the tree of objects.
     */
    *skipp = WT_BTREE_PREFIX(uri);
    if (!*skipp)
        return (0);

    __wt_timer_start(session, &timer);
    WT_RET(__wt_metadata_search(session, uri, &config));

    __wt_verbose_multi(
      session, WT_VERB_RECOVERY_RTS(session), "starting rollback to stable on uri %s", uri);

    /* Read the stable timestamp once, when we first start up. */
    WT_ACQUIRE_READ_WITH_BARRIER(stable_timestamp, conn->txn_global.stable_timestamp);
    WT_ACQUIRE_READ_WITH_BARRIER(pinned_timestamp, conn->txn_global.pinned_timestamp);

    /* If the stable timestamp is not set, do not roll back based on it. */
    if (stable_timestamp != WT_TS_NONE)
        rollback_timestamp = stable_timestamp;
    else {
        rollback_timestamp = WT_TS_MAX;
        __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session), WT_RTS_VERB_TAG_NO_STABLE "%s",
          "the stable timestamp is not set; set the rollback timestamp to the maximum timestamp");
    }

    F_SET(session, WT_SESSION_QUIET_CORRUPT_FILE);
    ret = __wti_rts_btree_walk_btree_apply(session, uri, config, rollback_timestamp);
    F_CLR(session, WT_SESSION_QUIET_CORRUPT_FILE);

    __rts_assert_timestamps_unchanged(session, pinned_timestamp, stable_timestamp);
    __wt_timer_evaluate_ms(session, &timer, &time_diff_ms);
    __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
      "finished rollback to stable on uri %s and has ran for %" PRIu64 " milliseconds", uri,
      time_diff_ms);

    __wt_free(session, config);
    return (ret);
}

/*
 * __rollback_to_stable_finalize --
 *     Reset a connection's RTS structure in preparation for the next call.
 */
static void
__rollback_to_stable_finalize(WT_ROLLBACK_TO_STABLE *rts)
{
    rts->dryrun = false;
    rts->threads_num = 0;
}

/*
 * __rollback_to_stable --
 *     Rollback the database to the stable timestamp.
 */
static int
__rollback_to_stable(WT_SESSION_IMPL *session, const char *cfg[], bool no_ckpt)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    WT_TIMER timer;
    uint64_t time_diff_ms;
    uint32_t threads;
    bool dryrun;

    /*
     * Explicit null-check because internal callers (startup/shutdown) do not enter via the API, and
     * don't get default values installed in the config string.
     */
    dryrun = false;
    threads = 0;
    if (cfg != NULL) {
        ret = __wt_config_gets(session, cfg, "dryrun", &cval);
        if (ret == 0)
            dryrun = cval.val != 0;
        WT_RET_NOTFOUND_OK(ret);
        ret = __wt_config_gets(session, cfg, "threads", &cval);
        if (ret == 0)
            threads = (uint32_t)cval.val;
        WT_RET_NOTFOUND_OK(ret);
    }

    /*
     * Don't use the connection's default session: we are working on data handles and (a) don't want
     * to cache all of them forever, plus (b) can't guarantee that no other method will be called
     * concurrently. Copy parent session no logging option to the internal session to make sure that
     * rollback to stable doesn't generate log records.
     */
    WT_RET(
      __wt_open_internal_session(S2C(session), "txn rollback_to_stable", true, 0, 0, &session));

    S2C(session)->rts->dryrun = dryrun;
    S2C(session)->rts->threads_num = threads;

    __wt_timer_start(session, &timer);

    WT_STAT_CONN_SET(session, txn_rollback_to_stable_running, 1);
    WT_WITH_CHECKPOINT_LOCK(
      session, WT_WITH_SCHEMA_LOCK(session, ret = __rollback_to_stable_int(session, no_ckpt)));

    /* Time since the RTS started. */
    __wt_timer_evaluate_ms(session, &timer, &time_diff_ms);
    __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
      WT_RTS_VERB_TAG_END "finished rollback to stable%s and has ran for %" PRIu64 " milliseconds",
      dryrun ? " dryrun" : "", time_diff_ms);
    WT_STAT_CONN_SET(session, txn_rollback_to_stable_running, 0);

    /* Reset the RTS configuration to default. */
    __rollback_to_stable_finalize(S2C(session)->rts);

    WT_TRET(__wt_session_close_internal(session));

    return (ret);
}

/*
 * __wt_rollback_to_stable_init --
 *     Initialize the data structures for the rollback to stable subsystem
 */
int
__wt_rollback_to_stable_init(WT_SESSION_IMPL *session, const char **cfg)
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    conn = S2C(session);

    /*
     * Setup the pointer so the data structure can be accessed easily while avoiding the need to do
     * explicit memory management.
     */
    conn->rts = &conn->_rts;

    /* Setup function pointers. */
    conn->rts->rollback_to_stable = __rollback_to_stable;
    conn->rts->rollback_to_stable_one = __rollback_to_stable_one;

    /* Setup variables. */
    conn->rts->dryrun = false;

    /* Setup worker threads at connection level. */
    if ((ret = __wt_config_gets(session, cfg, "rollback_to_stable.threads", &cval)) == 0)
        conn->rts->cfg_threads_num = (uint32_t)cval.val;
    WT_RET_NOTFOUND_OK(ret);

    return (0);
}

/*
 * __wt_rollback_to_stable_reconfig --
 *     Re-configure the RTS worker threads.
 */
int
__wt_rollback_to_stable_reconfig(WT_SESSION_IMPL *session, const char **cfg)
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    conn = S2C(session);

    /* Re-configure the RTS worker threads at connection level. */
    if ((ret = __wt_config_gets(session, cfg, "rollback_to_stable.threads", &cval)) == 0)
        conn->rts->cfg_threads_num = (uint32_t)cval.val;
    WT_RET_NOTFOUND_OK(ret);

    return (0);
}
