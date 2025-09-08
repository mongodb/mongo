/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __txn_parse_hex_raw --
 *     Decodes and sets a hex value. Don't do any checking.
 */
WT_INLINE static int
__txn_parse_hex_raw(
  WT_SESSION_IMPL *session, const char *name, uint64_t *valuep, WT_CONFIG_ITEM *cval)
{
    static const int8_t hextable[] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, -1, -1, -1, -1, -1, -1,
      -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 10, 11, 12, 13, 14, 15, -1};
    uint64_t value;
    size_t len;
    int hex_val;
    const char *hex_itr;

    *valuep = 0;

    if (cval->len == 0)
        return (0);

    /* Protect against unexpectedly long hex strings. */
    if (cval->len > 2 * sizeof(uint64_t))
        WT_RET_MSG(session, EINVAL, "%s too long '%.*s'", name, (int)cval->len, cval->str);

    for (value = 0, hex_itr = cval->str, len = cval->len; len > 0; --len) {
        if ((size_t)*hex_itr < WT_ELEMENTS(hextable))
            hex_val = hextable[(size_t)*hex_itr++];
        else
            hex_val = -1;
        if (hex_val < 0)
            WT_RET_MSG(
              session, EINVAL, "Failed to parse %s '%.*s'", name, (int)cval->len, cval->str);
        value = (value << 4) | (uint64_t)hex_val;
    }
    *valuep = value;

    return (0);
}

/*
 * __wt_txn_parse_timestamp_raw --
 *     Decodes and sets a timestamp. Don't do any checking.
 */
int
__wt_txn_parse_timestamp_raw(
  WT_SESSION_IMPL *session, const char *name, wt_timestamp_t *timestamp, WT_CONFIG_ITEM *cval)
{
    return (__txn_parse_hex_raw(session, name, timestamp, cval));
}

/*
 * __wt_txn_parse_timestamp --
 *     Decodes and sets a timestamp checking it is non-zero.
 */
int
__wt_txn_parse_timestamp(
  WT_SESSION_IMPL *session, const char *name, wt_timestamp_t *timestamp, WT_CONFIG_ITEM *cval)
{
    WT_RET(__wt_txn_parse_timestamp_raw(session, name, timestamp, cval));
    if (cval->len != 0 && *timestamp == WT_TS_NONE)
        WT_RET_MSG(session, EINVAL, "illegal %s '%.*s': zero not permitted", name, (int)cval->len,
          cval->str);

    return (0);
}

/*
 * __wt_txn_parse_prepared_id --
 *     Decodes and sets a prepared id checking it is non-zero.
 */
int
__wt_txn_parse_prepared_id(WT_SESSION_IMPL *session, uint64_t *prepared_id, WT_CONFIG_ITEM *cval)
{
    WT_RET(__txn_parse_hex_raw(session, "prepare id", prepared_id, cval));
    if (cval->len != 0 && *prepared_id == WT_PREPARED_ID_NONE)
        WT_RET_MSG(session, EINVAL, "illegal prepared id '%.*s': zero not permitted",
          (int)cval->len, cval->str);

    return (0);
}

/*
 * __txn_get_read_timestamp --
 *     Get the read timestamp from the transaction.
 */
static void
__txn_get_read_timestamp(WT_TXN_SHARED *txn_shared, wt_timestamp_t *read_timestampp)
{
    WT_ACQUIRE_READ_WITH_BARRIER(*read_timestampp, txn_shared->read_timestamp);
}

/*
 * __wti_txn_get_pinned_timestamp --
 *     Calculate the current pinned timestamp.
 */
void
__wti_txn_get_pinned_timestamp(WT_SESSION_IMPL *session, wt_timestamp_t *tsp, uint32_t flags)
{
    WT_CONNECTION_IMPL *conn;
    WT_TXN_GLOBAL *txn_global;
    WT_TXN_SHARED *s;
    wt_timestamp_t tmp_read_ts, tmp_ts;
    uint32_t i, session_cnt;
    bool include_oldest, txn_has_write_lock;

    conn = S2C(session);
    txn_global = &conn->txn_global;
    include_oldest = LF_ISSET(WT_TXN_TS_INCLUDE_OLDEST);
    txn_has_write_lock = LF_ISSET(WT_TXN_TS_ALREADY_LOCKED);

    /* If including oldest and there's none set, we're done, nothing else matters. */
    if (include_oldest && !__wt_atomic_loadbool(&txn_global->has_oldest_timestamp)) {
        *tsp = 0;
        return;
    }

    if (!txn_has_write_lock)
        __wt_readlock(session, &txn_global->rwlock);

    tmp_ts = include_oldest ? txn_global->oldest_timestamp : WT_TS_NONE;

    /* Check for a running checkpoint */
    if (LF_ISSET(WT_TXN_TS_INCLUDE_CKPT) && txn_global->checkpoint_timestamp != WT_TS_NONE &&
      (tmp_ts == WT_TS_NONE || txn_global->checkpoint_timestamp < tmp_ts))
        tmp_ts = txn_global->checkpoint_timestamp;

    /* Walk the array of concurrent transactions. */
    WT_ACQUIRE_READ_WITH_BARRIER(session_cnt, conn->session_array.cnt);
    for (i = 0, s = txn_global->txn_shared_list; i < session_cnt; i++, s++) {
        __txn_get_read_timestamp(s, &tmp_read_ts);
        /*
         * A zero timestamp is possible here only when the oldest timestamp is not accounted for.
         */
        if (tmp_ts == WT_TS_NONE || (tmp_read_ts != WT_TS_NONE && tmp_read_ts < tmp_ts))
            tmp_ts = tmp_read_ts;
    }

    if (!txn_has_write_lock)
        __wt_readunlock(session, &txn_global->rwlock);

    WT_STAT_CONN_INCR(session, txn_walk_sessions);
    WT_STAT_CONN_INCRV(session, txn_sessions_walked, i);

    *tsp = tmp_ts;
}

/*
 * __txn_get_durable_timestamp --
 *     Get the durable timestamp from the transaction.
 */
static void
__txn_get_durable_timestamp(WT_TXN_SHARED *txn_shared, wt_timestamp_t *durable_timestampp)
{
    WT_ACQUIRE_READ_WITH_BARRIER(*durable_timestampp, txn_shared->pinned_durable_timestamp);
}

/*
 * __txn_global_query_timestamp --
 *     Query a timestamp on the global transaction.
 */
static int
__txn_global_query_timestamp(WT_SESSION_IMPL *session, wt_timestamp_t *tsp, const char *cfg[])
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_TXN_GLOBAL *txn_global;
    WT_TXN_SHARED *s;
    wt_timestamp_t ts, tmpts;
    uint32_t i, session_cnt;

    conn = S2C(session);
    txn_global = &conn->txn_global;

    WT_STAT_CONN_INCR(session, txn_query_ts);
    WT_RET(__wt_config_gets(session, cfg, "get", &cval));
    if (WT_CONFIG_LIT_MATCH("all_durable", cval)) {
        /*
         * If there is no durable timestamp set, there is nothing to return. No need to walk the
         * concurrent transactions.
         */
        if (!txn_global->has_durable_timestamp) {
            *tsp = WT_TS_NONE;
            return (0);
        }

        __wt_readlock(session, &txn_global->rwlock);

        ts = txn_global->durable_timestamp;

        /* Walk the array of concurrent transactions. */
        WT_ACQUIRE_READ_WITH_BARRIER(session_cnt, conn->session_array.cnt);
        for (i = 0, s = txn_global->txn_shared_list; i < session_cnt; i++, s++) {
            __txn_get_durable_timestamp(s, &tmpts);
            if (tmpts != 0 && (ts == 0 || --tmpts < ts))
                ts = tmpts;
        }

        __wt_readunlock(session, &txn_global->rwlock);

        WT_STAT_CONN_INCR(session, txn_walk_sessions);
        WT_STAT_CONN_INCRV(session, txn_sessions_walked, i);
    } else if (WT_CONFIG_LIT_MATCH("backup_checkpoint", cval)) {
        /* This code will return set a timestamp only if a backup cursor is open. */
        ts = WT_TS_NONE;
        WT_WITH_HOTBACKUP_READ_LOCK_BACKUP(session, ts = conn->hot_backup_timestamp, NULL);
    } else if (WT_CONFIG_LIT_MATCH("last_checkpoint", cval)) {
        /* Read-only value forever. Make sure we don't used a cached version. */
        WT_COMPILER_BARRIER();
        ts = txn_global->last_ckpt_timestamp;
    } else if (WT_CONFIG_LIT_MATCH("oldest_timestamp", cval) ||
      WT_CONFIG_LIT_MATCH("oldest", cval)) {
        ts = __wt_atomic_loadbool(&txn_global->has_oldest_timestamp) ?
          txn_global->oldest_timestamp :
          0;
    } else if (WT_CONFIG_LIT_MATCH("oldest_reader", cval))
        __wti_txn_get_pinned_timestamp(session, &ts, WT_TXN_TS_INCLUDE_CKPT);
    else if (WT_CONFIG_LIT_MATCH("pinned", cval))
        __wti_txn_get_pinned_timestamp(
          session, &ts, WT_TXN_TS_INCLUDE_CKPT | WT_TXN_TS_INCLUDE_OLDEST);
    else if (WT_CONFIG_LIT_MATCH("recovery", cval))
        /* Read-only value forever. No lock needed. */
        ts = txn_global->recovery_timestamp;
    else if (WT_CONFIG_LIT_MATCH("stable_timestamp", cval) || WT_CONFIG_LIT_MATCH("stable", cval)) {
        ts = txn_global->has_stable_timestamp ? txn_global->stable_timestamp : 0;
    } else
        WT_RET_MSG(session, EINVAL, "unknown timestamp query %.*s", (int)cval.len, cval.str);

    *tsp = ts;
    return (0);
}

/*
 * __txn_query_timestamp --
 *     Query a timestamp within this session's transaction.
 */
static int
__txn_query_timestamp(WT_SESSION_IMPL *session, wt_timestamp_t *tsp, const char *cfg[])
{
    WT_CONFIG_ITEM cval;
    WT_TXN *txn;
    WT_TXN_SHARED *txn_shared;

    txn = session->txn;
    txn_shared = WT_SESSION_TXN_SHARED(session);

    WT_STAT_CONN_INCR(session, session_query_ts);

    WT_RET(__wt_config_gets(session, cfg, "get", &cval));
    if (WT_CONFIG_LIT_MATCH("commit", cval))
        *tsp = txn->commit_timestamp;
    else if (WT_CONFIG_LIT_MATCH("first_commit", cval))
        *tsp = txn->first_commit_timestamp;
    else if (WT_CONFIG_LIT_MATCH("prepare", cval))
        *tsp = txn->prepare_timestamp;
    else if (WT_CONFIG_LIT_MATCH("read", cval))
        *tsp = txn_shared->read_timestamp;
    else
        WT_RET_MSG(session, EINVAL, "unknown timestamp query %.*s", (int)cval.len, cval.str);

    return (0);
}

/*
 * __wt_txn_query_timestamp --
 *     Query a timestamp. The caller may query the global transaction or the session's transaction.
 */
int
__wt_txn_query_timestamp(
  WT_SESSION_IMPL *session, char *hex_timestamp, const char *cfg[], bool global_txn)
{
    wt_timestamp_t ts;

    if (global_txn)
        WT_RET(__txn_global_query_timestamp(session, &ts, cfg));
    else
        WT_RET(__txn_query_timestamp(session, &ts, cfg));

    __wt_timestamp_to_hex_string(ts, hex_timestamp);
    return (0);
}

/*
 * __wti_txn_update_pinned_timestamp --
 *     Update the pinned timestamp (the oldest timestamp that has to be maintained for current or
 *     future readers).
 */
void
__wti_txn_update_pinned_timestamp(WT_SESSION_IMPL *session, bool force)
{
    WT_TXN_GLOBAL *txn_global;
    wt_timestamp_t last_pinned_timestamp, pinned_timestamp;

    txn_global = &S2C(session)->txn_global;

    /* Skip locking and scanning when the oldest timestamp is pinned. */
    if (txn_global->oldest_is_pinned)
        return;

    /* Scan to find the global pinned timestamp. */
    __wti_txn_get_pinned_timestamp(session, &pinned_timestamp, WT_TXN_TS_INCLUDE_OLDEST);
    if (pinned_timestamp == 0)
        return;

    if (txn_global->has_pinned_timestamp && !force) {
        last_pinned_timestamp = txn_global->pinned_timestamp;

        if (pinned_timestamp <= last_pinned_timestamp)
            return;
    }

    __wt_writelock(session, &txn_global->rwlock);
    /*
     * Scan the global pinned timestamp again, it's possible that it got changed after the previous
     * scan.
     */
    __wti_txn_get_pinned_timestamp(
      session, &pinned_timestamp, WT_TXN_TS_ALREADY_LOCKED | WT_TXN_TS_INCLUDE_OLDEST);

    if (pinned_timestamp != WT_TS_NONE &&
      (!txn_global->has_pinned_timestamp || force ||
        txn_global->pinned_timestamp < pinned_timestamp)) {
        WT_RELEASE_WRITE(txn_global->pinned_timestamp, pinned_timestamp);
        /*
         * Release write requires the data and destination have exactly the same size. stdbool.h
         * only defines true as `#define true 1` so we need a bool cast to provide proper type
         * information.
         */
        WT_RELEASE_WRITE(txn_global->has_pinned_timestamp, (bool)true);
        txn_global->oldest_is_pinned = txn_global->pinned_timestamp == txn_global->oldest_timestamp;
        txn_global->stable_is_pinned = txn_global->pinned_timestamp == txn_global->stable_timestamp;
        __wt_verbose_timestamp(session, pinned_timestamp, "Updated pinned timestamp");
    }
    __wt_writeunlock(session, &txn_global->rwlock);
}

/*
 * __wt_txn_global_set_timestamp --
 *     Set a global transaction timestamp.
 */
int
__wt_txn_global_set_timestamp(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CONFIG_ITEM cval;
    WT_CONFIG_ITEM durable_cval, oldest_cval, stable_cval;
    WT_TXN_GLOBAL *txn_global;
    wt_timestamp_t durable_ts, oldest_ts, stable_ts;
    wt_timestamp_t last_oldest_ts, last_stable_ts;
    char ts_string[2][WT_TS_INT_STRING_SIZE];
    bool force, has_durable, has_oldest, has_stable;

    txn_global = &S2C(session)->txn_global;

    WT_STAT_CONN_INCR(session, txn_set_ts);

    WT_RET(__wt_config_gets_def(session, cfg, "durable_timestamp", 0, &durable_cval));
    has_durable = durable_cval.len != 0;
    if (has_durable)
        WT_STAT_CONN_INCR(session, txn_set_ts_durable);

    WT_RET(__wt_config_gets_def(session, cfg, "oldest_timestamp", 0, &oldest_cval));
    has_oldest = oldest_cval.len != 0;
    if (has_oldest)
        WT_STAT_CONN_INCR(session, txn_set_ts_oldest);

    WT_RET(__wt_config_gets_def(session, cfg, "stable_timestamp", 0, &stable_cval));
    has_stable = stable_cval.len != 0;
    if (has_stable)
        WT_STAT_CONN_INCR(session, txn_set_ts_stable);

    /* If no timestamp was supplied, there's nothing to do. */
    if (!has_durable && !has_oldest && !has_stable)
        return (0);

    /*
     * Parsing will initialize the timestamp to zero even if it is not configured.
     */
    WT_RET(__wt_txn_parse_timestamp(session, "durable timestamp", &durable_ts, &durable_cval));
    WT_RET(__wt_txn_parse_timestamp(session, "oldest timestamp", &oldest_ts, &oldest_cval));
    WT_RET(__wt_txn_parse_timestamp(session, "stable timestamp", &stable_ts, &stable_cval));

    WT_RET(__wt_config_gets_def(session, cfg, "force", 0, &cval));
    force = cval.val != 0;

    if (force) {
        WT_STAT_CONN_INCR(session, txn_set_ts_force);
        goto set;
    }

    __wt_readlock(session, &txn_global->rwlock);

    last_oldest_ts = txn_global->oldest_timestamp;
    last_stable_ts = txn_global->stable_timestamp;

    /* It is an invalid call to set the oldest or stable timestamps behind the current values. */
    if (has_oldest && __wt_atomic_loadbool(&txn_global->has_oldest_timestamp) &&
      oldest_ts < last_oldest_ts) {
        __wt_readunlock(session, &txn_global->rwlock);
        WT_RET_MSG(session, EINVAL,
          "set_timestamp: oldest timestamp %s must not be older than current oldest timestamp %s",
          __wt_timestamp_to_string(oldest_ts, ts_string[0]),
          __wt_timestamp_to_string(last_oldest_ts, ts_string[1]));
    }

    if (has_stable && txn_global->has_stable_timestamp && stable_ts < last_stable_ts) {
        __wt_readunlock(session, &txn_global->rwlock);
        WT_RET_MSG(session, EINVAL,
          "set_timestamp: stable timestamp %s must not be older than current stable timestamp %s",
          __wt_timestamp_to_string(stable_ts, ts_string[0]),
          __wt_timestamp_to_string(last_stable_ts, ts_string[1]));
    }

    /*
     * First do error checking on the timestamp values. The oldest timestamp must always be less
     * than or equal to the stable timestamp. If we're only setting one then compare against the
     * system timestamp. If we're setting both then compare the passed in values.
     */
    if (!has_oldest && __wt_atomic_loadbool(&txn_global->has_oldest_timestamp))
        oldest_ts = last_oldest_ts;
    if (!has_stable && txn_global->has_stable_timestamp)
        stable_ts = last_stable_ts;

    /* The oldest and stable timestamps must always satisfy the condition that oldest <= stable. */
    if ((has_oldest || has_stable) &&
      (has_oldest || __wt_atomic_loadbool(&txn_global->has_oldest_timestamp)) &&
      (has_stable || txn_global->has_stable_timestamp) && oldest_ts > stable_ts) {
        __wt_readunlock(session, &txn_global->rwlock);
        WT_RET_MSG(session, EINVAL,
          "set_timestamp: oldest timestamp %s must not be later than stable timestamp %s",
          __wt_timestamp_to_string(oldest_ts, ts_string[0]),
          __wt_timestamp_to_string(stable_ts, ts_string[1]));
    }

    __wt_readunlock(session, &txn_global->rwlock);

    /* Check if we are actually updating anything. */
    if (!has_durable && !has_oldest && !has_stable)
        return (0);

set:
    __wt_writelock(session, &txn_global->rwlock);
    /*
     * This method can be called from multiple threads, check that we are moving the global
     * timestamps forwards.
     *
     * The exception is the durable timestamp, where the application can move it backwards (in fact,
     * it only really makes sense to explicitly move it backwards because it otherwise tracks the
     * largest durable_timestamp so it moves forward whenever transactions are assigned timestamps).
     */
    if (has_durable) {
        txn_global->durable_timestamp = durable_ts;
        txn_global->has_durable_timestamp = true;
        WT_STAT_CONN_INCR(session, txn_set_ts_durable_upd);
        __wt_verbose_timestamp(session, durable_ts, "Updated global durable timestamp");
    }

    if (has_oldest &&
      (!__wt_atomic_loadbool(&txn_global->has_oldest_timestamp) || force ||
        oldest_ts > txn_global->oldest_timestamp)) {
        txn_global->oldest_timestamp = oldest_ts;
        WT_STAT_CONN_INCR(session, txn_set_ts_oldest_upd);
        __wt_atomic_storebool(&txn_global->has_oldest_timestamp, true);
        txn_global->oldest_is_pinned = false;
        __wt_verbose_timestamp(session, oldest_ts, "Updated global oldest timestamp");
    }

    if (has_stable &&
      (!txn_global->has_stable_timestamp || force || stable_ts > txn_global->stable_timestamp)) {
        WT_RELEASE_WRITE(txn_global->stable_timestamp, stable_ts);
        WT_STAT_CONN_INCR(session, txn_set_ts_stable_upd);
        /*
         * Release write requires the data and destination have exactly the same size. stdbool.h
         * only defines true as `#define true 1` so we need a bool cast to provide proper type
         * information.
         */
        WT_RELEASE_WRITE(txn_global->has_stable_timestamp, (bool)true);
        txn_global->stable_is_pinned = false;
        __wt_verbose_timestamp(session, stable_ts, "Updated global stable timestamp");
    }

    /*
     * Even if the timestamps have been forcibly set, they must always satisfy the condition that
     * oldest <= stable. Don't fail as MongoDB violates this rule in very specific scenarios.
     */
    if (txn_global->has_stable_timestamp &&
      __wt_atomic_loadbool(&txn_global->has_oldest_timestamp) &&
      txn_global->oldest_timestamp > txn_global->stable_timestamp) {
        WT_STAT_CONN_INCR(session, txn_set_ts_out_of_order);
        __wt_verbose_debug1(session, WT_VERB_TIMESTAMP,
          "set_timestamp: oldest timestamp %s must not be later than stable timestamp %s",
          __wt_timestamp_to_string(txn_global->oldest_timestamp, ts_string[0]),
          __wt_timestamp_to_string(txn_global->stable_timestamp, ts_string[1]));
    }

    __wt_writeunlock(session, &txn_global->rwlock);

    if (has_oldest || has_stable)
        __wti_txn_update_pinned_timestamp(session, force);

    return (0);
}

/*
 * __txn_assert_after_reads --
 *     Assert that commit and prepare timestamps are greater than the latest active read timestamp,
 *     if any.
 */
static void
__txn_assert_after_reads(WT_SESSION_IMPL *session, const char *op, wt_timestamp_t ts)
{
    WT_TXN_GLOBAL *txn_global;
    WT_TXN_SHARED *s;
    wt_timestamp_t tmp_timestamp;
    uint32_t i, session_cnt;
    char ts_string[2][WT_TS_INT_STRING_SIZE];

    if (EXTRA_DIAGNOSTICS_ENABLED(session, WT_DIAGNOSTIC_TXN_VISIBILITY)) {
        txn_global = &S2C(session)->txn_global;
        WT_ACQUIRE_READ_WITH_BARRIER(session_cnt, S2C(session)->session_array.cnt);
        WT_STAT_CONN_INCR(session, txn_walk_sessions);
        WT_STAT_CONN_INCRV(session, txn_sessions_walked, session_cnt);

        __wt_readlock(session, &txn_global->rwlock);

        /* Walk the array of concurrent transactions. */
        for (i = 0, s = txn_global->txn_shared_list; i < session_cnt; i++, s++) {
            __txn_get_read_timestamp(s, &tmp_timestamp);
            if (tmp_timestamp != WT_TS_NONE && tmp_timestamp >= ts) {
                __wt_err(session, EINVAL,
                  "%s timestamp %s must be after all active read timestamps %s", op,
                  __wt_timestamp_to_string(ts, ts_string[0]),
                  __wt_timestamp_to_string(tmp_timestamp, ts_string[1]));

                __wt_abort(session);
                /* NOTREACHED */
            }
        }
        __wt_readunlock(session, &txn_global->rwlock);
    } else {
        WT_UNUSED(session);
        WT_UNUSED(op);
        WT_UNUSED(ts);
        WT_UNUSED(txn_global);
    }
}

/*
 * __txn_validate_commit_timestamp --
 *     Validate the commit timestamp of a transaction.
 */
static int
__txn_validate_commit_timestamp(WT_SESSION_IMPL *session, wt_timestamp_t *commit_tsp)
{
    WT_TXN *txn;
    WT_TXN_GLOBAL *txn_global;
    wt_timestamp_t commit_ts, oldest_ts, stable_ts;
    char ts_string[2][WT_TS_INT_STRING_SIZE];
    bool has_oldest_ts, has_stable_ts;

    txn = session->txn;
    txn_global = &S2C(session)->txn_global;
    commit_ts = *commit_tsp;

    /* Added this redundant initialization to circumvent build failure. */
    oldest_ts = stable_ts = WT_TS_NONE;

    /*
     * Compare against the oldest and the stable timestamp. Return an error if the given timestamp
     * is less than oldest and/or stable timestamp.
     */
    has_oldest_ts = __wt_atomic_loadbool(&txn_global->has_oldest_timestamp);
    if (has_oldest_ts)
        oldest_ts = txn_global->oldest_timestamp;
    has_stable_ts = txn_global->has_stable_timestamp;
    if (has_stable_ts)
        stable_ts = txn_global->stable_timestamp;

    if (!F_ISSET(txn, WT_TXN_HAS_TS_PREPARE)) {
        /* Compare against the first commit timestamp of the current transaction. */
        if (F_ISSET(txn, WT_TXN_HAS_TS_COMMIT)) {
            if (commit_ts < txn->first_commit_timestamp)
                WT_RET_MSG(session, EINVAL,
                  "commit timestamp %s older than the first commit timestamp %s for this "
                  "transaction",
                  __wt_timestamp_to_string(commit_ts, ts_string[0]),
                  __wt_timestamp_to_string(txn->first_commit_timestamp, ts_string[1]));
            commit_ts = txn->first_commit_timestamp;
        }

        /*
         * For a non-prepared transactions the commit timestamp should not be less or equal to the
         * stable timestamp.
         */
        if (has_oldest_ts && commit_ts < oldest_ts)
            WT_RET_MSG(session, EINVAL, "commit timestamp %s is less than the oldest timestamp %s",
              __wt_timestamp_to_string(commit_ts, ts_string[0]),
              __wt_timestamp_to_string(oldest_ts, ts_string[1]));

        if (has_stable_ts && commit_ts <= stable_ts)
            WT_RET_MSG(session, EINVAL, "commit timestamp %s must be after the stable timestamp %s",
              __wt_timestamp_to_string(commit_ts, ts_string[0]),
              __wt_timestamp_to_string(stable_ts, ts_string[1]));

        __txn_assert_after_reads(session, "commit", commit_ts);
    } else {
        /*
         * For a prepared transaction, the commit timestamp should not be less than the prepare
         * timestamp. Also, the commit timestamp cannot be set before the transaction has actually
         * been prepared.
         *
         * If the commit timestamp is less than the oldest timestamp and transaction is configured
         * to roundup timestamps of a prepared transaction, then we will roundup the commit
         * timestamp to the prepare timestamp of the transaction.
         */
        if (txn->prepare_timestamp > commit_ts) {
            if (!F_ISSET(txn, WT_TXN_TS_ROUND_PREPARED))
                WT_RET_MSG(session, EINVAL,
                  "commit timestamp %s is less than the prepare timestamp %s for this transaction",
                  __wt_timestamp_to_string(commit_ts, ts_string[0]),
                  __wt_timestamp_to_string(txn->prepare_timestamp, ts_string[1]));

            /* Update the caller's value. */
            *commit_tsp = txn->prepare_timestamp;
        }
        if (!F_ISSET(txn, WT_TXN_PREPARE))
            WT_RET_MSG(
              session, EINVAL, "commit timestamp must not be set before transaction is prepared");
        if (F_ISSET(txn, WT_TXN_HAS_TS_ROLLBACK))
            WT_RET_MSG(session, EINVAL,
              "rollback timestamp and commit timestamp should not be set together");
    }

    return (0);
}

/*
 * __txn_set_commit_timestamp --
 *     Set the commit timestamp of a transaction.
 */
static int
__txn_set_commit_timestamp(WT_SESSION_IMPL *session, wt_timestamp_t commit_ts)
{
    WT_TXN *txn;
    WT_TXN_GLOBAL *txn_global;
    wt_timestamp_t newest_commit_ts;

    txn = session->txn;
    txn_global = &S2C(session)->txn_global;

    if (txn->isolation != WT_ISO_SNAPSHOT)
        WT_RET_MSG(session, EINVAL,
          "setting a commit_timestamp requires a transaction running at snapshot isolation");

    /*
     * In scenarios where the prepare timestamp is greater than the provided commit timestamp, the
     * validate function returns the new commit timestamp based on the configuration.
     */
    WT_RET(__txn_validate_commit_timestamp(session, &commit_ts));
    txn->commit_timestamp = commit_ts;

    /*
     * First time copy the commit timestamp to the first commit timestamp.
     */
    if (!F_ISSET(txn, WT_TXN_HAS_TS_COMMIT))
        txn->first_commit_timestamp = commit_ts;

    /*
     * Only mirror the commit timestamp if there isn't already an explicit durable timestamp. This
     * might happen if we set a commit timestamp, set a durable timestamp and then subsequently set
     * the commit timestamp again.
     */
    if (!F_ISSET(txn, WT_TXN_HAS_TS_DURABLE))
        txn->durable_timestamp = commit_ts;

/* Used to define the granularity at which the shared global recent commit timestamp is updated. */
#define WT_COMMIT_TS_UPDATE_THRESHOLD 10
    /* Don't be overly greedy about updating the commit timestamp, it's shared */
    WT_ACQUIRE_READ(newest_commit_ts, txn_global->newest_seen_timestamp);
    if (commit_ts > newest_commit_ts + WT_COMMIT_TS_UPDATE_THRESHOLD) {
        /* If our update failed, someone beat us to it - no problem. */
        __wt_atomic_cas64(&txn_global->newest_seen_timestamp, newest_commit_ts, commit_ts);
    }

    F_SET(txn, WT_TXN_HAS_TS_COMMIT);
    return (0);
}

/*
 * __txn_validate_durable_timestamp --
 *     Validate the durable timestamp of a transaction.
 */
static int
__txn_validate_durable_timestamp(WT_SESSION_IMPL *session, wt_timestamp_t durable_ts)
{
    WT_TXN *txn;
    WT_TXN_GLOBAL *txn_global;
    wt_timestamp_t oldest_ts, stable_ts;
    char ts_string[2][WT_TS_INT_STRING_SIZE];
    bool has_oldest_ts, has_stable_ts;

    txn = session->txn;
    txn_global = &S2C(session)->txn_global;

    /* Added this redundant initialization to circumvent build failure. */
    oldest_ts = stable_ts = 0;

    /*
     * Compare against the oldest and the stable timestamp. Return an error if the given timestamp
     * is less than oldest and/or stable timestamp.
     */
    has_oldest_ts = __wt_atomic_loadbool(&txn_global->has_oldest_timestamp);
    if (has_oldest_ts)
        oldest_ts = txn_global->oldest_timestamp;
    has_stable_ts = txn_global->has_stable_timestamp;
    if (has_stable_ts)
        stable_ts = txn_global->stable_timestamp;

    if (has_oldest_ts && durable_ts < oldest_ts)
        WT_RET_MSG(session, EINVAL, "durable timestamp %s is less than the oldest timestamp %s",
          __wt_timestamp_to_string(durable_ts, ts_string[0]),
          __wt_timestamp_to_string(oldest_ts, ts_string[1]));

    if (has_stable_ts && durable_ts <= stable_ts)
        WT_RET_MSG(session, EINVAL, "durable timestamp %s must be after the stable timestamp %s",
          __wt_timestamp_to_string(durable_ts, ts_string[0]),
          __wt_timestamp_to_string(stable_ts, ts_string[1]));

    /* Check if the durable timestamp is less than the commit timestamp. */
    if (durable_ts < txn->commit_timestamp)
        WT_RET_MSG(session, EINVAL,
          "durable timestamp %s is less than the commit timestamp %s for this transaction",
          __wt_timestamp_to_string(durable_ts, ts_string[0]),
          __wt_timestamp_to_string(txn->commit_timestamp, ts_string[1]));

    return (0);
}

/*
 * __txn_publish_durable_timestamp --
 *     Publish a transaction's durable timestamp.
 */
static void
__txn_publish_durable_timestamp(WT_SESSION_IMPL *session)
{
    WT_TXN *txn;
    WT_TXN_SHARED *txn_shared;
    wt_timestamp_t ts;

    txn = session->txn;
    txn_shared = WT_SESSION_TXN_SHARED(session);

    if (F_ISSET(txn, WT_TXN_SHARED_TS_DURABLE))
        return;

    if (F_ISSET(txn, WT_TXN_HAS_TS_DURABLE))
        ts = txn->durable_timestamp;
    else if (F_ISSET(txn, WT_TXN_HAS_TS_COMMIT)) {
        /*
         * If we know for a fact that this is a prepared transaction and we only have a commit
         * timestamp, don't add to the durable queue. If we poll all_durable after setting the
         * commit timestamp of a prepared transaction, that prepared transaction should NOT be
         * visible.
         */
        if (F_ISSET(txn, WT_TXN_PREPARE))
            return;
        ts = txn->first_commit_timestamp;
    } else
        return;

    txn_shared->pinned_durable_timestamp = ts;
    F_SET(txn, WT_TXN_SHARED_TS_DURABLE);
}

/*
 * __txn_set_durable_timestamp --
 *     Set the durable timestamp of a transaction.
 */
static int
__txn_set_durable_timestamp(WT_SESSION_IMPL *session, wt_timestamp_t durable_ts)
{
    WT_TXN *txn;

    txn = session->txn;

    if (!F_ISSET(txn, WT_TXN_PREPARE))
        WT_RET_MSG(session, EINVAL,
          "durable timestamp should not be specified for non-prepared transaction");

    if (!F_ISSET(txn, WT_TXN_HAS_TS_COMMIT))
        WT_RET_MSG(
          session, EINVAL, "a commit timestamp is required before setting a durable timestamp");

    WT_RET(__txn_validate_durable_timestamp(session, durable_ts));
    txn->durable_timestamp = durable_ts;
    F_SET(txn, WT_TXN_HAS_TS_DURABLE);

    return (0);
}

/*
 * __txn_set_prepare_timestamp --
 *     Validate and set the prepare timestamp of a transaction.
 */
static int
__txn_set_prepare_timestamp(WT_SESSION_IMPL *session, wt_timestamp_t prepare_ts)
{
    WT_TXN *txn;
    WT_TXN_GLOBAL *txn_global;
    wt_timestamp_t oldest_ts, stable_ts;
    char ts_string[2][WT_TS_INT_STRING_SIZE];

    txn = session->txn;
    txn_global = &S2C(session)->txn_global;

    WT_RET(__wt_txn_context_prepare_check(session));

    if (F_ISSET(txn, WT_TXN_HAS_TS_PREPARE))
        WT_RET_MSG(session, EINVAL, "prepare timestamp is already set");

    if (F_ISSET(txn, WT_TXN_HAS_TS_COMMIT))
        WT_RET_MSG(session, EINVAL,
          "commit timestamp should not have been set before the prepare timestamp");

    __txn_assert_after_reads(session, "prepare", prepare_ts);

    /*
     * Check whether the prepare timestamp is less than the stable timestamp.
     */
    stable_ts = txn_global->stable_timestamp;
    if (prepare_ts <= stable_ts) {
        /*
         * Check whether the application is using the "prepared" roundup mode. This rounds up to
         * _oldest_, not stable, and permits preparing before stable, because it is meant to be used
         * during application recovery to replay a transaction that was successfully prepared (and
         * possibly committed) before a crash but had not yet become durable. In general it is
         * important to replay such transactions at the same time they had before the crash; in a
         * distributed setting they might have already committed in the network, in which case
         * replaying them at a different time is very likely to be inconsistent. Meanwhile, once a
         * transaction prepares we allow stable to move forward past it, so replaying may require
         * preparing and even committing prior to stable.
         *
         * Such a replay is safe provided that it happens during application-level recovery before
         * resuming ordinary operations: between the time the transaction prepares and the crash,
         * operations intersecting with the prepared transaction fail with WT_PREPARE_CONFLICT, and
         * after the crash, the replay recreates this state before any ordinary operations can
         * intersect with it. Application recovery code is responsible for making sure that any
         * other operations it does before the replay that might intersect with the prepared
         * transaction are consistent with it.
         *
         * (There is a slight extra wrinkle at the moment, because it is possible for a transaction
         * to prepare and commit and be interacted with before it becomes durable. Currently such
         * transactions _must_ be replayed identically by the application to avoid inconsistency,
         * or avoided.
         *
         * Under other circumstances, that is, not during application-level recovery when ordinary
         * operations are excluded, use of "roundup=prepared" (for replaying transactions or
         * otherwise) is not safe and can cause data inconsistency. There is currently no roundup
         * mode for commit timestamps that is suitable for use during ordinary operation.
         */
        if (F_ISSET(txn, WT_TXN_TS_ROUND_PREPARED)) {
            oldest_ts = txn_global->oldest_timestamp;
            if (prepare_ts < oldest_ts) {
                __wt_verbose(session, WT_VERB_TIMESTAMP,
                  "prepare timestamp %s rounded to oldest timestamp %s",
                  __wt_timestamp_to_string(prepare_ts, ts_string[0]),
                  __wt_timestamp_to_string(oldest_ts, ts_string[1]));
                prepare_ts = oldest_ts;
            }
        } else
            WT_RET_MSG(session, EINVAL,
              "prepare timestamp %s is not newer than the stable timestamp %s",
              __wt_timestamp_to_string(prepare_ts, ts_string[0]),
              __wt_timestamp_to_string(stable_ts, ts_string[1]));
    }
    txn->prepare_timestamp = prepare_ts;
    F_SET(txn, WT_TXN_HAS_TS_PREPARE);

    return (0);
}

/*
 * __wti_txn_set_read_timestamp --
 *     Parse a request to set a transaction's read_timestamp.
 */
int
__wti_txn_set_read_timestamp(WT_SESSION_IMPL *session, wt_timestamp_t read_ts)
{
    WT_TXN *txn;
    WT_TXN_GLOBAL *txn_global;
    WT_TXN_SHARED *txn_shared;
    wt_timestamp_t ts_oldest;
    char ts_string[2][WT_TS_INT_STRING_SIZE];
    bool did_roundup_to_oldest;

    txn = session->txn;
    txn_global = &S2C(session)->txn_global;
    txn_shared = WT_SESSION_TXN_SHARED(session);

    /*
     * Silently ignore attempts to set the read timestamp after a transaction is prepared (if we
     * error, the system will panic because an operation on a prepared transaction cannot fail).
     */
    if (F_ISSET(session->txn, WT_TXN_PREPARE)) {
        __wt_errx(session,
          "attempt to set the read timestamp after the transaction is prepared silently ignored");
        return (0);
    }

    /* Read timestamps imply / require snapshot isolation. */
    if (!F_ISSET(txn, WT_TXN_RUNNING))
        txn->isolation = WT_ISO_SNAPSHOT;
    else if (txn->isolation != WT_ISO_SNAPSHOT)
        WT_RET_MSG(session, EINVAL,
          "setting a read_timestamp requires a transaction running at snapshot isolation");

    /* Read timestamps can't change once set. */
    if (F_ISSET(txn, WT_TXN_SHARED_TS_READ))
        WT_RET_MSG(session, EINVAL, "a read_timestamp may only be set once per transaction");

    /*
     * This code is not using the timestamp validate function to avoid a race between checking and
     * setting transaction timestamp.
     */
    __wt_readlock(session, &txn_global->rwlock);

    ts_oldest = txn_global->oldest_timestamp;
    did_roundup_to_oldest = false;
    if (read_ts < ts_oldest) {
        /*
         * If given read timestamp is earlier than oldest timestamp then round the read timestamp to
         * oldest timestamp.
         */
        if (F_ISSET(txn, WT_TXN_TS_ROUND_READ)) {
            txn_shared->read_timestamp = ts_oldest;
            did_roundup_to_oldest = true;
        } else {
            __wt_readunlock(session, &txn_global->rwlock);

#if !defined(WT_STANDALONE_BUILD)
            /*
             * In some cases, MongoDB sets a read timestamp older than the oldest timestamp, relying
             * on WiredTiger's concurrency to detect and fail the set. In other cases it's a bug and
             * MongoDB wants error context to make it easier to find those problems. Don't output an
             * error message because that logs a MongoDB error, use an informational message to
             * provide the context instead. Don't output this message for standalone builds, it's
             * too noisy for applications that don't track the read timestamp against the oldest
             * timestamp and simply expect the set to fail.
             */
            __wt_verbose_notice(session, WT_VERB_TIMESTAMP,
              "read timestamp %s less than the oldest timestamp %s",
              __wt_timestamp_to_string(read_ts, ts_string[0]),
              __wt_timestamp_to_string(ts_oldest, ts_string[1]));
#endif
            return (EINVAL);
        }
    } else
        txn_shared->read_timestamp = read_ts;

    F_SET(txn, WT_TXN_SHARED_TS_READ);
    __wt_readunlock(session, &txn_global->rwlock);

    /*
     * This message is generated here to reduce the span of critical section.
     */
    if (did_roundup_to_oldest)
        __wt_verbose(session, WT_VERB_TIMESTAMP,
          "read timestamp %s : rounded to oldest timestamp %s",
          __wt_timestamp_to_string(read_ts, ts_string[0]),
          __wt_timestamp_to_string(ts_oldest, ts_string[1]));

    /*
     * If we already have a snapshot, it may be too early to match the timestamp (including the one
     * we just read, if rounding to oldest). Get a new one.
     */
    if (F_ISSET(txn, WT_TXN_RUNNING))
        __wt_txn_get_snapshot(session);

    return (0);
}

/*
 * __txn_set_rollback_timestamp --
 *     Validate and set the rollback timestamp of a transaction.
 */
static int
__txn_set_rollback_timestamp(WT_SESSION_IMPL *session, wt_timestamp_t rollback_ts)
{
    WT_TXN *txn;
    WT_TXN_GLOBAL *txn_global;
    wt_timestamp_t stable_ts;
    char ts_string[2][WT_TS_INT_STRING_SIZE];

    txn = session->txn;
    txn_global = &S2C(session)->txn_global;

    if (!F_ISSET(txn, WT_TXN_PREPARE))
        WT_RET_MSG(session, EINVAL, "rollback timestamp is set for an non-prepared transaction");

    if (F_ISSET(txn, WT_TXN_HAS_TS_ROLLBACK))
        WT_RET_MSG(session, EINVAL, "rollback timestamp is already set");

    if (F_ISSET(txn, WT_TXN_HAS_TS_COMMIT))
        WT_RET_MSG(
          session, EINVAL, "commit timestamp and rollback timestamp should not be set together");

    if (F_ISSET(txn, WT_TXN_HAS_TS_DURABLE))
        WT_RET_MSG(
          session, EINVAL, "durable timestamp and rollback timestamp should not be set together");

    __txn_assert_after_reads(session, "rollback", rollback_ts);

    /* Check whether the rollback timestamp is less than the stable timestamp. */
    stable_ts = txn_global->stable_timestamp;
    if (rollback_ts <= stable_ts) {
        WT_RET_MSG(session, EINVAL,
          "rollback timestamp %s is not newer than the stable timestamp %s",
          __wt_timestamp_to_string(rollback_ts, ts_string[0]),
          __wt_timestamp_to_string(stable_ts, ts_string[1]));
    }
    txn->rollback_timestamp = rollback_ts;
    F_SET(txn, WT_TXN_HAS_TS_ROLLBACK);

    return (0);
}

/*
 * __txn_set_prepared_id --
 *     Validate and set the prepared_id.
 */
static int
__txn_set_prepared_id(WT_SESSION_IMPL *session, uint64_t prepared_id)
{
    WT_TXN *txn;

    txn = session->txn;

    if (F_ISSET(txn, WT_TXN_HAS_PREPARED_ID))
        WT_RET_MSG(session, EINVAL, "prepared id is already set");

    txn->prepared_id = prepared_id;
    F_SET(txn, WT_TXN_HAS_PREPARED_ID);

    return (0);
}

/*
 * __wt_txn_set_timestamp --
 *     Parse a request to set a timestamp in a transaction.
 */
int
__wt_txn_set_timestamp(WT_SESSION_IMPL *session, const char *cfg[], bool commit)
{
    WT_CONFIG cparser;
    WT_CONFIG_ITEM ckey, cval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_TXN *txn;
    wt_timestamp_t commit_ts, durable_ts, prepare_ts, read_ts, rollback_ts;
    bool set_ts;

    conn = S2C(session);
    txn = session->txn;
    set_ts = false;

    WT_RET(__wt_txn_context_check(session, true));

    /*
     * If no commit or durable timestamp is set here, set to any previously set values and validate
     * them, the stable timestamp might have moved forward since they were successfully set.
     */
    commit_ts = durable_ts = prepare_ts = read_ts = rollback_ts = WT_TS_NONE;
    if (commit && F_ISSET(txn, WT_TXN_HAS_TS_COMMIT))
        commit_ts = txn->commit_timestamp;
    if (commit && F_ISSET(txn, WT_TXN_HAS_TS_DURABLE))
        durable_ts = txn->durable_timestamp;

    /*
     * If the API received no configuration string, or we just have the base configuration, there
     * are no strings to parse. Additionally, take a shortcut in parsing that works because we're
     * only given a base configuration and a user configuration.
     */
    if (cfg != NULL && cfg[0] != NULL && cfg[1] != NULL) {
        WT_ASSERT(session, cfg[2] == NULL);
        __wt_config_init(session, &cparser, cfg[1]);
        while ((ret = __wt_config_next(&cparser, &ckey, &cval)) == 0) {
            WT_ASSERT(session, ckey.str != NULL);
            if (WT_CONFIG_LIT_MATCH("commit_timestamp", ckey)) {
                WT_RET(__wt_txn_parse_timestamp(session, "commit timestamp", &commit_ts, &cval));
                set_ts = true;
            } else if (WT_CONFIG_LIT_MATCH("durable_timestamp", ckey)) {
                WT_RET(__wt_txn_parse_timestamp(session, "durable timestamp", &durable_ts, &cval));
                set_ts = true;
            } else if (WT_CONFIG_LIT_MATCH("prepare_timestamp", ckey)) {
                WT_RET(__wt_txn_parse_timestamp(session, "prepare timestamp", &prepare_ts, &cval));
                set_ts = true;
            } else if (WT_CONFIG_LIT_MATCH("read_timestamp", ckey)) {
                WT_RET(__wt_txn_parse_timestamp(session, "read timestamp", &read_ts, &cval));
                set_ts = true;
            } else if (WT_CONFIG_LIT_MATCH("rollback_timestamp", ckey)) {
                WT_RET(
                  __wt_txn_parse_timestamp(session, "rollback timestamp", &rollback_ts, &cval));
                set_ts = true;
            }
        }
        WT_RET_NOTFOUND_OK(ret);
    }

    if (commit)
        if (rollback_ts != WT_TS_NONE)
            WT_RET_MSG(session, EINVAL, "rollback timestamp is set for commit");

    /* Look for a commit timestamp. */
    if (commit_ts != WT_TS_NONE)
        WT_RET(__txn_set_commit_timestamp(session, commit_ts));

    /*
     * Look for a durable timestamp. Durable timestamp should be set only after setting the commit
     * timestamp.
     */
    if (durable_ts != WT_TS_NONE)
        WT_RET(__txn_set_durable_timestamp(session, durable_ts));
    __txn_publish_durable_timestamp(session);

    /* Look for a read timestamp. */
    if (read_ts != WT_TS_NONE)
        WT_RET(__wti_txn_set_read_timestamp(session, read_ts));

    /* Look for a prepare timestamp. */
    if (prepare_ts != WT_TS_NONE)
        WT_RET(__txn_set_prepare_timestamp(session, prepare_ts));

    if (rollback_ts != WT_TS_NONE)
        WT_RET(__txn_set_rollback_timestamp(session, rollback_ts));

    /* Timestamps are only logged in debugging mode. */
    if (set_ts && FLD_ISSET(conn->debug_flags, WT_CONN_DEBUG_TABLE_LOGGING) &&
      F_ISSET(&conn->log_mgr, WT_LOG_ENABLED) && !F_ISSET(conn, WT_CONN_RECOVERING))
        WT_RET(__wti_txn_ts_log(session));

    return (0);
}

/*
 * __wt_txn_set_timestamp_uint --
 *     Directly set the commit timestamp in a transaction, bypassing parsing logic. Prefer this to
 *     __wt_txn_set_timestamp when string parsing is a performance bottleneck.
 */
int
__wt_txn_set_timestamp_uint(WT_SESSION_IMPL *session, WT_TS_TXN_TYPE which, wt_timestamp_t ts)
{
    WT_CONNECTION_IMPL *conn;
    const char *name;

    WT_RET(__wt_txn_context_check(session, true));

    conn = S2C(session);

    if (ts == WT_TS_NONE) {
        /* Quiet warnings from both gcc and clang about this variable. */
        WT_NOT_READ(name, "unknown");
        switch (which) {
        case WT_TS_TXN_TYPE_COMMIT:
            name = "commit";
            break;
        case WT_TS_TXN_TYPE_DURABLE:
            name = "durable";
            break;
        case WT_TS_TXN_TYPE_PREPARE:
            name = "prepare";
            break;
        case WT_TS_TXN_TYPE_READ:
            name = "read";
            break;
        case WT_TS_TXN_TYPE_ROLLBACK:
            name = "rollback";
            break;
        }
        WT_RET_MSG(session, EINVAL, "illegal %s timestamp: zero not permitted", name);
    }

    switch (which) {
    case WT_TS_TXN_TYPE_COMMIT:
        WT_RET(__txn_set_commit_timestamp(session, ts));
        break;
    case WT_TS_TXN_TYPE_DURABLE:
        WT_RET(__txn_set_durable_timestamp(session, ts));
        break;
    case WT_TS_TXN_TYPE_PREPARE:
        WT_RET(__txn_set_prepare_timestamp(session, ts));
        break;
    case WT_TS_TXN_TYPE_READ:
        WT_RET(__wti_txn_set_read_timestamp(session, ts));
        break;
    case WT_TS_TXN_TYPE_ROLLBACK:
        WT_RET(__txn_set_rollback_timestamp(session, ts));
        break;
    }
    __txn_publish_durable_timestamp(session);

    /* Timestamps are only logged in debugging mode. */
    if (FLD_ISSET(conn->debug_flags, WT_CONN_DEBUG_TABLE_LOGGING) &&
      F_ISSET(&conn->log_mgr, WT_LOG_ENABLED) && !F_ISSET(conn, WT_CONN_RECOVERING))
        WT_RET(__wti_txn_ts_log(session));

    return (0);
}

/*
 * __wt_txn_set_prepared_id --
 *     Parse a request to set a prepared_id in a transaction.
 */
int
__wt_txn_set_prepared_id(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CONFIG cparser;
    WT_CONFIG_ITEM ckey, cval;
    WT_DECL_RET;
    uint64_t prepared_id;

    WT_RET(__wt_txn_context_check(session, true));

    prepared_id = WT_PREPARED_ID_NONE;

    /*
     * If the API received no configuration string, or we just have the base configuration, there
     * are no strings to parse. Additionally, take a shortcut in parsing that works because we're
     * only given a base configuration and a user configuration.
     */
    if (cfg != NULL && cfg[0] != NULL && cfg[1] != NULL) {
        WT_ASSERT(session, cfg[2] == NULL);
        __wt_config_init(session, &cparser, cfg[1]);
        while ((ret = __wt_config_next(&cparser, &ckey, &cval)) == 0) {
            WT_ASSERT(session, ckey.str != NULL);
            if (WT_CONFIG_LIT_MATCH("prepared_id", ckey))
                WT_RET(__wt_txn_parse_prepared_id(session, &prepared_id, &cval));
        }
        WT_RET_NOTFOUND_OK(ret);
    }

    if (prepared_id != WT_PREPARED_ID_NONE)
        WT_RET(__txn_set_prepared_id(session, prepared_id));

    return (0);
}

/*
 * __wt_txn_set_prepared_id_uint --
 *     Directly set the prepared id in a transaction, bypassing parsing logic. Prefer this to
 *     __wt_txn_set_prepared_id when string parsing is a performance bottleneck.
 */
int
__wt_txn_set_prepared_id_uint(WT_SESSION_IMPL *session, uint64_t prepared_id)
{
    WT_RET(__wt_txn_context_check(session, true));

    if (prepared_id == WT_PREPARED_ID_NONE)
        WT_RET_MSG(session, EINVAL, "illegal prepared id: zero not permitted");

    WT_RET(__txn_set_prepared_id(session, prepared_id));

    return (0);
}

/*
 * __wti_txn_clear_durable_timestamp --
 *     Clear a transaction's published durable timestamp.
 */
void
__wti_txn_clear_durable_timestamp(WT_SESSION_IMPL *session)
{
    WT_TXN *txn;
    WT_TXN_SHARED *txn_shared;

    txn = session->txn;
    txn_shared = WT_SESSION_TXN_SHARED(session);

    if (!F_ISSET(txn, WT_TXN_SHARED_TS_DURABLE))
        return;

    WT_RELEASE_BARRIER();
    F_CLR(txn, WT_TXN_SHARED_TS_DURABLE);
    txn_shared->pinned_durable_timestamp = WT_TS_NONE;
}

/*
 * __wti_txn_clear_read_timestamp --
 *     Clear a transaction's published read timestamp.
 */
void
__wti_txn_clear_read_timestamp(WT_SESSION_IMPL *session)
{
    WT_TXN *txn;
    WT_TXN_SHARED *txn_shared;

    txn = session->txn;
    txn_shared = WT_SESSION_TXN_SHARED(session);

    if (F_ISSET(txn, WT_TXN_SHARED_TS_READ)) {
        /* Assert the read timestamp is greater than or equal to the pinned timestamp. */
        WT_ASSERT(session, txn_shared->read_timestamp >= S2C(session)->txn_global.pinned_timestamp);

        WT_RELEASE_BARRIER();
        F_CLR(txn, WT_TXN_SHARED_TS_READ);
    }
    txn_shared->read_timestamp = WT_TS_NONE;
}
