/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

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
    wt_timestamp_t pinned_timestamp, rollback_timestamp;
    char ts_string[2][WT_TS_INT_STRING_SIZE];
    bool dryrun;

    conn = S2C(session);
    txn_global = &conn->txn_global;
    dryrun = conn->rts->dryrun;

    /*
     * Rollback to stable should ignore tombstones in the history store since it needs to scan the
     * entire table sequentially.
     */
    F_SET(session, WT_SESSION_ROLLBACK_TO_STABLE);

    WT_ERR(__wt_rts_check(session));

    /*
     * Update the global time window state to have consistent view from global visibility rules for
     * the rollback to stable to bring back the database into a consistent state.
     *
     * As part of the below function call, the oldest transaction id and pinned timestamps are
     * updated.
     */
    WT_ERR(__wt_txn_update_oldest(session, WT_TXN_OLDEST_STRICT | WT_TXN_OLDEST_WAIT));

    WT_ASSERT_ALWAYS(session,
      (txn_global->has_pinned_timestamp || !txn_global->has_oldest_timestamp),
      "Database has no pinned timestamp but an oldest timestamp. Pinned timestamp is required to "
      "find out the global visibility/obsolete of an update.");

    /*
     * Copy the stable timestamp, otherwise we'd need to lock it each time it's accessed. Even
     * though the stable timestamp isn't supposed to be updated while rolling back, accessing it
     * without a lock would violate protocol.
     */
    WT_ORDERED_READ(rollback_timestamp, txn_global->stable_timestamp);
    WT_ORDERED_READ(pinned_timestamp, txn_global->pinned_timestamp);
    __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
      WT_RTS_VERB_TAG_INIT
      "start rollback to stable with stable_timestamp=%s and oldest_timestamp=%s",
      __wt_timestamp_to_string(rollback_timestamp, ts_string[0]),
      __wt_timestamp_to_string(txn_global->oldest_timestamp, ts_string[1]));

    if (F_ISSET(conn, WT_CONN_RECOVERING))
        __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
          WT_RTS_VERB_TAG_RECOVER_CKPT "recovered checkpoint snapshot_min=%" PRIu64
                                       ", snapshot_max=%" PRIu64 ", snapshot_count=%" PRIu32,
          conn->recovery_ckpt_snap_min, conn->recovery_ckpt_snap_max,
          conn->recovery_ckpt_snapshot_count);

    WT_ERR(__wt_rts_btree_apply_all(session, rollback_timestamp));

    /* Rollback the global durable timestamp to the stable timestamp. */
    if (!dryrun) {
        txn_global->has_durable_timestamp = txn_global->has_stable_timestamp;
        txn_global->durable_timestamp = txn_global->stable_timestamp;
    }
    __rts_assert_timestamps_unchanged(session, pinned_timestamp, rollback_timestamp);

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
    wt_timestamp_t pinned_timestamp, rollback_timestamp;
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

    WT_RET(__wt_metadata_search(session, uri, &config));

    /* Read the stable timestamp once, when we first start up. */
    WT_ORDERED_READ(rollback_timestamp, conn->txn_global.stable_timestamp);
    WT_ORDERED_READ(pinned_timestamp, conn->txn_global.pinned_timestamp);

    F_SET(session, WT_SESSION_QUIET_CORRUPT_FILE);
    ret = __wt_rts_btree_walk_btree_apply(session, uri, config, rollback_timestamp);
    F_CLR(session, WT_SESSION_QUIET_CORRUPT_FILE);

    __rts_assert_timestamps_unchanged(session, pinned_timestamp, rollback_timestamp);

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
    bool dryrun;

    /*
     * Explicit null-check because internal callers (startup/shutdown) do not enter via the API, and
     * don't get default values installed in the config string.
     */
    dryrun = false;
    if (cfg != NULL) {
        ret = __wt_config_gets(session, cfg, "dryrun", &cval);
        if (ret == 0)
            dryrun = cval.val != 0;
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

    WT_STAT_CONN_SET(session, txn_rollback_to_stable_running, 1);
    WT_WITH_CHECKPOINT_LOCK(
      session, WT_WITH_SCHEMA_LOCK(session, ret = __rollback_to_stable_int(session, no_ckpt)));

    __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
      WT_RTS_VERB_TAG_END "finished rollback to stable%s", dryrun ? " dryrun" : "");
    WT_STAT_CONN_SET(session, txn_rollback_to_stable_running, 0);

    __rollback_to_stable_finalize(S2C(session)->rts);

    WT_TRET(__wt_session_close_internal(session));

    return (ret);
}

/*
 * __wt_rollback_to_stable_init --
 *     Initialize the data structures for the rollback to stable subsystem
 */
void
__wt_rollback_to_stable_init(WT_CONNECTION_IMPL *conn)
{
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
}
