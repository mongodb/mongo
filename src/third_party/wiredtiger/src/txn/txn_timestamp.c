/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_txn_parse_timestamp_raw --
 *     Decodes and sets a timestamp. Don't do any checking.
 */
int
__wt_txn_parse_timestamp_raw(
  WT_SESSION_IMPL *session, const char *name, wt_timestamp_t *timestamp, WT_CONFIG_ITEM *cval)
{
    static const int8_t hextable[] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, -1, -1, -1, -1, -1, -1,
      -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 10, 11, 12, 13, 14, 15, -1};
    wt_timestamp_t ts;
    size_t len;
    int hex_val;
    const char *hex_itr;

    *timestamp = 0;

    if (cval->len == 0)
        return (0);

    /* Protect against unexpectedly long hex strings. */
    if (cval->len > 2 * sizeof(wt_timestamp_t))
        WT_RET_MSG(
          session, EINVAL, "%s timestamp too long '%.*s'", name, (int)cval->len, cval->str);

    for (ts = 0, hex_itr = cval->str, len = cval->len; len > 0; --len) {
        if ((size_t)*hex_itr < WT_ELEMENTS(hextable))
            hex_val = hextable[(size_t)*hex_itr++];
        else
            hex_val = -1;
        if (hex_val < 0)
            WT_RET_MSG(session, EINVAL, "Failed to parse %s timestamp '%.*s'", name, (int)cval->len,
              cval->str);
        ts = (ts << 4) | (uint64_t)hex_val;
    }
    *timestamp = ts;

    return (0);
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
        WT_RET_MSG(session, EINVAL, "Failed to parse %s timestamp '%.*s': zero not permitted", name,
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
    WT_ORDERED_READ(*read_timestampp, txn_shared->read_timestamp);
}

/*
 * __wt_txn_get_pinned_timestamp --
 *     Calculate the current pinned timestamp.
 */
int
__wt_txn_get_pinned_timestamp(WT_SESSION_IMPL *session, wt_timestamp_t *tsp, uint32_t flags)
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

    if (include_oldest && !txn_global->has_oldest_timestamp)
        return (WT_NOTFOUND);

    if (!txn_has_write_lock)
        __wt_readlock(session, &txn_global->rwlock);

    tmp_ts = include_oldest ? txn_global->oldest_timestamp : WT_TS_NONE;

    /* Check for a running checkpoint */
    if (LF_ISSET(WT_TXN_TS_INCLUDE_CKPT) && txn_global->checkpoint_timestamp != WT_TS_NONE &&
      (tmp_ts == WT_TS_NONE || txn_global->checkpoint_timestamp < tmp_ts))
        tmp_ts = txn_global->checkpoint_timestamp;

    /* Walk the array of concurrent transactions. */
    WT_ORDERED_READ(session_cnt, conn->session_cnt);
    WT_STAT_CONN_INCR(session, txn_walk_sessions);
    for (i = 0, s = txn_global->txn_shared_list; i < session_cnt; i++, s++) {
        WT_STAT_CONN_INCR(session, txn_sessions_walked);
        __txn_get_read_timestamp(s, &tmp_read_ts);
        /*
         * A zero timestamp is possible here only when the oldest timestamp is not accounted for.
         */
        if (tmp_ts == WT_TS_NONE || (tmp_read_ts != WT_TS_NONE && tmp_read_ts < tmp_ts))
            tmp_ts = tmp_read_ts;
    }

    if (!txn_has_write_lock)
        __wt_readunlock(session, &txn_global->rwlock);

    if (!include_oldest && tmp_ts == 0)
        return (WT_NOTFOUND);
    *tsp = tmp_ts;

    return (0);
}

/*
 * __txn_get_durable_timestamp --
 *     Get the durable timestamp from the transaction.
 */
static void
__txn_get_durable_timestamp(WT_TXN_SHARED *txn_shared, wt_timestamp_t *durable_timestampp)
{
    WT_ORDERED_READ(*durable_timestampp, txn_shared->pinned_durable_timestamp);
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
    if (WT_STRING_MATCH("all_durable", cval.str, cval.len)) {
        if (!txn_global->has_durable_timestamp)
            return (WT_NOTFOUND);
        ts = txn_global->durable_timestamp;
        WT_ASSERT(session, ts != WT_TS_NONE);

        __wt_readlock(session, &txn_global->rwlock);

        /* Walk the array of concurrent transactions. */
        WT_ORDERED_READ(session_cnt, conn->session_cnt);
        WT_STAT_CONN_INCR(session, txn_walk_sessions);
        for (i = 0, s = txn_global->txn_shared_list; i < session_cnt; i++, s++) {
            WT_STAT_CONN_INCR(session, txn_sessions_walked);
            __txn_get_durable_timestamp(s, &tmpts);
            if (tmpts != WT_TS_NONE && --tmpts < ts)
                ts = tmpts;
        }

        __wt_readunlock(session, &txn_global->rwlock);

        /*
         * If a transaction is committing with a durable timestamp of 1, we could return zero here,
         * which is unexpected. Fail instead.
         */
        if (ts == WT_TS_NONE)
            return (WT_NOTFOUND);
    } else if (WT_STRING_MATCH("last_checkpoint", cval.str, cval.len)) {
        /* Read-only value forever. Make sure we don't used a cached version. */
        WT_BARRIER();
        ts = txn_global->last_ckpt_timestamp;
    } else if (WT_STRING_MATCH("oldest_timestamp", cval.str, cval.len) ||
      WT_STRING_MATCH("oldest", cval.str, cval.len)) {
        if (!txn_global->has_oldest_timestamp)
            return (WT_NOTFOUND);
        ts = txn_global->oldest_timestamp;
    } else if (WT_STRING_MATCH("oldest_reader", cval.str, cval.len))
        WT_RET(__wt_txn_get_pinned_timestamp(session, &ts, WT_TXN_TS_INCLUDE_CKPT));
    else if (WT_STRING_MATCH("pinned", cval.str, cval.len))
        WT_RET(__wt_txn_get_pinned_timestamp(
          session, &ts, WT_TXN_TS_INCLUDE_CKPT | WT_TXN_TS_INCLUDE_OLDEST));
    else if (WT_STRING_MATCH("recovery", cval.str, cval.len))
        /* Read-only value forever. No lock needed. */
        ts = txn_global->recovery_timestamp;
    else if (WT_STRING_MATCH("stable_timestamp", cval.str, cval.len) ||
      WT_STRING_MATCH("stable", cval.str, cval.len)) {
        if (!txn_global->has_stable_timestamp)
            return (WT_NOTFOUND);
        ts = txn_global->stable_timestamp;
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
    if (!F_ISSET(txn, WT_TXN_RUNNING))
        return (WT_NOTFOUND);

    WT_RET(__wt_config_gets(session, cfg, "get", &cval));
    if (WT_STRING_MATCH("commit", cval.str, cval.len))
        *tsp = txn->commit_timestamp;
    else if (WT_STRING_MATCH("first_commit", cval.str, cval.len))
        *tsp = txn->first_commit_timestamp;
    else if (WT_STRING_MATCH("prepare", cval.str, cval.len))
        *tsp = txn->prepare_timestamp;
    else if (WT_STRING_MATCH("read", cval.str, cval.len))
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
 * __wt_txn_update_pinned_timestamp --
 *     Update the pinned timestamp (the oldest timestamp that has to be maintained for current or
 *     future readers).
 */
int
__wt_txn_update_pinned_timestamp(WT_SESSION_IMPL *session, bool force)
{
    WT_DECL_RET;
    WT_TXN_GLOBAL *txn_global;
    wt_timestamp_t last_pinned_timestamp, pinned_timestamp;

    txn_global = &S2C(session)->txn_global;

    /* Skip locking and scanning when the oldest timestamp is pinned. */
    if (txn_global->oldest_is_pinned)
        return (0);

    /* Scan to find the global pinned timestamp. */
    if ((ret = __wt_txn_get_pinned_timestamp(
           session, &pinned_timestamp, WT_TXN_TS_INCLUDE_OLDEST)) != 0)
        return (ret == WT_NOTFOUND ? 0 : ret);

    if (txn_global->has_pinned_timestamp && !force) {
        last_pinned_timestamp = txn_global->pinned_timestamp;

        if (pinned_timestamp <= last_pinned_timestamp)
            return (0);
    }

    __wt_writelock(session, &txn_global->rwlock);
    /*
     * Scan the global pinned timestamp again, it's possible that it got changed after the previous
     * scan.
     */
    if ((ret = __wt_txn_get_pinned_timestamp(
           session, &pinned_timestamp, WT_TXN_TS_ALREADY_LOCKED | WT_TXN_TS_INCLUDE_OLDEST)) != 0) {
        __wt_writeunlock(session, &txn_global->rwlock);
        return (ret == WT_NOTFOUND ? 0 : ret);
    }

    if (!txn_global->has_pinned_timestamp || force ||
      txn_global->pinned_timestamp < pinned_timestamp) {
        txn_global->pinned_timestamp = pinned_timestamp;
        txn_global->has_pinned_timestamp = true;
        txn_global->oldest_is_pinned = txn_global->pinned_timestamp == txn_global->oldest_timestamp;
        txn_global->stable_is_pinned = txn_global->pinned_timestamp == txn_global->stable_timestamp;
        __wt_verbose_timestamp(session, pinned_timestamp, "Updated pinned timestamp");
    }
    __wt_writeunlock(session, &txn_global->rwlock);

    return (0);
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
    WT_RET(__wt_txn_parse_timestamp(session, "durable", &durable_ts, &durable_cval));
    WT_RET(__wt_txn_parse_timestamp(session, "oldest", &oldest_ts, &oldest_cval));
    WT_RET(__wt_txn_parse_timestamp(session, "stable", &stable_ts, &stable_cval));

    WT_RET(__wt_config_gets_def(session, cfg, "force", 0, &cval));
    force = cval.val != 0;

    if (force)
        goto set;

    __wt_readlock(session, &txn_global->rwlock);

    last_oldest_ts = txn_global->oldest_timestamp;
    last_stable_ts = txn_global->stable_timestamp;

    /* It is a no-op to set the oldest or stable timestamps behind the global values. */
    if (has_oldest && txn_global->has_oldest_timestamp && oldest_ts <= last_oldest_ts)
        has_oldest = false;

    if (has_stable && txn_global->has_stable_timestamp && stable_ts <= last_stable_ts)
        has_stable = false;

    /*
     * First do error checking on the timestamp values. The oldest timestamp must always be less
     * than or equal to the stable timestamp. If we're only setting one then compare against the
     * system timestamp. If we're setting both then compare the passed in values.
     */
    if (!has_durable && txn_global->has_durable_timestamp)
        durable_ts = txn_global->durable_timestamp;
    if (!has_oldest && txn_global->has_oldest_timestamp)
        oldest_ts = last_oldest_ts;
    if (!has_stable && txn_global->has_stable_timestamp)
        stable_ts = last_stable_ts;

    /*
     * If a durable timestamp was supplied, check that it is no older than either the stable
     * timestamp or the oldest timestamp.
     */
    if (has_durable && (has_oldest || txn_global->has_oldest_timestamp) && oldest_ts > durable_ts) {
        __wt_readunlock(session, &txn_global->rwlock);
        WT_RET_MSG(session, EINVAL,
          "set_timestamp: oldest timestamp %s must not be later than durable timestamp %s",
          __wt_timestamp_to_string(oldest_ts, ts_string[0]),
          __wt_timestamp_to_string(durable_ts, ts_string[1]));
    }

    if (has_durable && (has_stable || txn_global->has_stable_timestamp) && stable_ts > durable_ts) {
        __wt_readunlock(session, &txn_global->rwlock);
        WT_RET_MSG(session, EINVAL,
          "set_timestamp: stable timestamp %s must not be later than durable timestamp %s",
          __wt_timestamp_to_string(stable_ts, ts_string[0]),
          __wt_timestamp_to_string(durable_ts, ts_string[1]));
    }

    /*
     * The oldest and stable timestamps must always satisfy the condition that oldest <= stable.
     */
    if ((has_oldest || has_stable) && (has_oldest || txn_global->has_oldest_timestamp) &&
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
      (!txn_global->has_oldest_timestamp || force || oldest_ts > txn_global->oldest_timestamp)) {
        txn_global->oldest_timestamp = oldest_ts;
        WT_STAT_CONN_INCR(session, txn_set_ts_oldest_upd);
        txn_global->has_oldest_timestamp = true;
        txn_global->oldest_is_pinned = false;
        __wt_verbose_timestamp(session, oldest_ts, "Updated global oldest timestamp");
    }

    if (has_stable &&
      (!txn_global->has_stable_timestamp || force || stable_ts > txn_global->stable_timestamp)) {
        txn_global->stable_timestamp = stable_ts;
        WT_STAT_CONN_INCR(session, txn_set_ts_stable_upd);
        txn_global->has_stable_timestamp = true;
        txn_global->stable_is_pinned = false;
        __wt_verbose_timestamp(session, stable_ts, "Updated global stable timestamp");
    }
    __wt_writeunlock(session, &txn_global->rwlock);

    if (has_oldest || has_stable)
        WT_RET(__wt_txn_update_pinned_timestamp(session, force));

    return (0);
}

/*
 * __txn_assert_after_reads --
 *     Assert that commit and prepare timestamps are greater than the latest active read timestamp,
 *     if any.
 */
static int
__txn_assert_after_reads(WT_SESSION_IMPL *session, const char *op, wt_timestamp_t ts)
{
#ifdef HAVE_DIAGNOSTIC
    WT_TXN_GLOBAL *txn_global;
    WT_TXN_SHARED *s;
    wt_timestamp_t tmp_timestamp;
    uint32_t i, session_cnt;
    char ts_string[2][WT_TS_INT_STRING_SIZE];

    txn_global = &S2C(session)->txn_global;

    __wt_readlock(session, &txn_global->rwlock);
    /* Walk the array of concurrent transactions. */
    WT_ORDERED_READ(session_cnt, S2C(session)->session_cnt);
    WT_STAT_CONN_INCR(session, txn_walk_sessions);
    for (i = 0, s = txn_global->txn_shared_list; i < session_cnt; i++, s++) {
        WT_STAT_CONN_INCR(session, txn_sessions_walked);
        __txn_get_read_timestamp(s, &tmp_timestamp);
        if (tmp_timestamp != WT_TS_NONE && tmp_timestamp >= ts) {
            __wt_readunlock(session, &txn_global->rwlock);
            WT_RET_MSG(session, EINVAL,
              "%s timestamp %s must be greater than the latest active read timestamp %s ", op,
              __wt_timestamp_to_string(ts, ts_string[0]),
              __wt_timestamp_to_string(tmp_timestamp, ts_string[1]));
        }
    }
    __wt_readunlock(session, &txn_global->rwlock);
#else
    WT_UNUSED(session);
    WT_UNUSED(op);
    WT_UNUSED(ts);
#endif

    return (0);
}

/*
 * __wt_txn_set_commit_timestamp --
 *     Validate the commit timestamp of a transaction.
 */
int
__wt_txn_set_commit_timestamp(WT_SESSION_IMPL *session, wt_timestamp_t commit_ts)
{
    WT_TXN *txn;
    WT_TXN_GLOBAL *txn_global;
    wt_timestamp_t oldest_ts, stable_ts;
    char ts_string[2][WT_TS_INT_STRING_SIZE];
    bool has_oldest_ts, has_stable_ts;

    txn = session->txn;
    txn_global = &S2C(session)->txn_global;

    /* Added this redundant initialization to circumvent build failure. */
    oldest_ts = stable_ts = WT_TS_NONE;

    if (txn->isolation != WT_ISO_SNAPSHOT)
        WT_RET_MSG(session, EINVAL,
          "setting a commit_timestamp requires a transaction running at snapshot isolation");

    /*
     * Compare against the oldest and the stable timestamp. Return an error if the given timestamp
     * is less than oldest and/or stable timestamp.
     */
    has_oldest_ts = txn_global->has_oldest_timestamp;
    if (has_oldest_ts)
        oldest_ts = txn_global->oldest_timestamp;
    has_stable_ts = txn_global->has_stable_timestamp;
    if (has_stable_ts)
        stable_ts = txn_global->stable_timestamp;

    if (!F_ISSET(txn, WT_TXN_HAS_TS_PREPARE)) {
        /*
         * For a non-prepared transactions the commit timestamp should not be less than the stable
         * timestamp.
         */
        if (has_oldest_ts && commit_ts < oldest_ts)
            WT_RET_MSG(session, EINVAL, "commit timestamp %s is less than the oldest timestamp %s",
              __wt_timestamp_to_string(commit_ts, ts_string[0]),
              __wt_timestamp_to_string(oldest_ts, ts_string[1]));

#ifdef WT_STANDALONE_BUILD
        if (has_stable_ts && commit_ts <= stable_ts)
            WT_RET_MSG(session, EINVAL, "commit timestamp %s must be after the stable timestamp %s",
              __wt_timestamp_to_string(commit_ts, ts_string[0]),
              __wt_timestamp_to_string(stable_ts, ts_string[1]));
#else
        /* Don't change this error message, MongoDB servers check for it. */
        if (has_stable_ts && commit_ts < stable_ts)
            WT_RET_MSG(session, EINVAL, "commit timestamp %s is less than the stable timestamp %s",
              __wt_timestamp_to_string(commit_ts, ts_string[0]),
              __wt_timestamp_to_string(stable_ts, ts_string[1]));
#endif

        /*
         * Compare against the commit timestamp of the current transaction. Return an error if the
         * given timestamp is older than the first commit timestamp.
         */
        if (F_ISSET(txn, WT_TXN_HAS_TS_COMMIT) && commit_ts < txn->first_commit_timestamp)
            WT_RET_MSG(session, EINVAL,
              "commit timestamp %s older than the first commit timestamp %s for this transaction",
              __wt_timestamp_to_string(commit_ts, ts_string[0]),
              __wt_timestamp_to_string(txn->first_commit_timestamp, ts_string[1]));

        WT_RET(__txn_assert_after_reads(session, "commit", commit_ts));
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
            commit_ts = txn->prepare_timestamp;
        }
        if (!F_ISSET(txn, WT_TXN_PREPARE))
            WT_RET_MSG(
              session, EINVAL, "commit timestamp must not be set before transaction is prepared");
    }

    WT_ASSERT(session,
      !F_ISSET(txn, WT_TXN_HAS_TS_DURABLE) || txn->durable_timestamp == txn->commit_timestamp);
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

    F_SET(txn, WT_TXN_HAS_TS_COMMIT);
    return (0);
}

/*
 * __wt_txn_set_durable_timestamp --
 *     Validate the durable timestamp of a transaction.
 */
int
__wt_txn_set_durable_timestamp(WT_SESSION_IMPL *session, wt_timestamp_t durable_ts)
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

    if (!F_ISSET(txn, WT_TXN_PREPARE))
        WT_RET_MSG(session, EINVAL,
          "durable timestamp should not be specified for non-prepared transaction");

    if (!F_ISSET(txn, WT_TXN_HAS_TS_COMMIT))
        WT_RET_MSG(session, EINVAL, "commit timestamp is needed before the durable timestamp");

    /*
     * Compare against the oldest and the stable timestamp. Return an error if the given timestamp
     * is less than oldest and/or stable timestamp.
     */
    has_oldest_ts = txn_global->has_oldest_timestamp;
    if (has_oldest_ts)
        oldest_ts = txn_global->oldest_timestamp;
    has_stable_ts = txn_global->has_stable_timestamp;
    if (has_stable_ts)
        stable_ts = txn_global->stable_timestamp;

    if (has_oldest_ts && durable_ts < oldest_ts)
        WT_RET_MSG(session, EINVAL, "durable timestamp %s is less than the oldest timestamp %s",
          __wt_timestamp_to_string(durable_ts, ts_string[0]),
          __wt_timestamp_to_string(oldest_ts, ts_string[1]));

#ifdef WT_STANDALONE_BUILD
    if (has_stable_ts && durable_ts <= stable_ts)
        WT_RET_MSG(session, EINVAL, "durable timestamp %s must be after the stable timestamp %s",
          __wt_timestamp_to_string(durable_ts, ts_string[0]),
          __wt_timestamp_to_string(stable_ts, ts_string[1]));
#else
    if (has_stable_ts && durable_ts < stable_ts)
        WT_RET_MSG(session, EINVAL, "durable timestamp %s is less than the stable timestamp %s",
          __wt_timestamp_to_string(durable_ts, ts_string[0]),
          __wt_timestamp_to_string(stable_ts, ts_string[1]));
#endif

    /* Check if the durable timestamp is less than the commit timestamp. */
    if (durable_ts < txn->commit_timestamp)
        WT_RET_MSG(session, EINVAL,
          "durable timestamp %s is less than the commit timestamp %s for this transaction",
          __wt_timestamp_to_string(durable_ts, ts_string[0]),
          __wt_timestamp_to_string(txn->commit_timestamp, ts_string[1]));

    txn->durable_timestamp = durable_ts;
    F_SET(txn, WT_TXN_HAS_TS_DURABLE);

    return (0);
}

/*
 * __wt_txn_set_prepare_timestamp --
 *     Validate and set the prepare timestamp of a transaction.
 */
int
__wt_txn_set_prepare_timestamp(WT_SESSION_IMPL *session, wt_timestamp_t prepare_ts)
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

    WT_RET(__txn_assert_after_reads(session, "prepare", prepare_ts));

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
         * or avoided. FIXME-WT-8747: remove this note when WT-8747 fixes this.)
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
 * __wt_txn_set_read_timestamp --
 *     Parse a request to set a transaction's read_timestamp.
 */
int
__wt_txn_set_read_timestamp(WT_SESSION_IMPL *session, wt_timestamp_t read_ts)
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
 * __wt_txn_set_timestamp --
 *     Parse a request to set a timestamp in a transaction.
 */
int
__wt_txn_set_timestamp(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CONFIG cparser;
    WT_CONFIG_ITEM ckey, cval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    wt_timestamp_t commit_ts, durable_ts, prepare_ts, read_ts;
    bool set_ts;

    conn = S2C(session);
    commit_ts = durable_ts = prepare_ts = read_ts = WT_TS_NONE;
    set_ts = false;

    WT_TRET(__wt_txn_context_check(session, true));

    /*
     * If the API received no configuration string, or we just have the base configuration, there's
     * nothing to do.
     */
    if (cfg == NULL || cfg[0] == NULL || cfg[1] == NULL)
        return (0);

    /*
     * We take a shortcut in parsing that works because we're only given a base configuration and a
     * user configuration.
     */
    WT_ASSERT(session, cfg[0] != NULL && cfg[1] != NULL && cfg[2] == NULL);
    __wt_config_init(session, &cparser, cfg[1]);
    while ((ret = __wt_config_next(&cparser, &ckey, &cval)) == 0) {
        WT_ASSERT(session, ckey.str != NULL);
        if (WT_STRING_MATCH("commit_timestamp", ckey.str, ckey.len)) {
            WT_RET(__wt_txn_parse_timestamp(session, "commit", &commit_ts, &cval));
            set_ts = true;
        } else if (WT_STRING_MATCH("durable_timestamp", ckey.str, ckey.len)) {
            WT_RET(__wt_txn_parse_timestamp(session, "durable", &durable_ts, &cval));
            set_ts = true;
        } else if (WT_STRING_MATCH("prepare_timestamp", ckey.str, ckey.len)) {
            WT_RET(__wt_txn_parse_timestamp(session, "prepare", &prepare_ts, &cval));
            set_ts = true;
        } else if (WT_STRING_MATCH("read_timestamp", ckey.str, ckey.len)) {
            WT_RET(__wt_txn_parse_timestamp(session, "read", &read_ts, &cval));
            set_ts = true;
        }
    }
    WT_RET_NOTFOUND_OK(ret);

    /* Look for a commit timestamp. */
    if (commit_ts != WT_TS_NONE)
        WT_RET(__wt_txn_set_commit_timestamp(session, commit_ts));

    /*
     * Look for a durable timestamp. Durable timestamp should be set only after setting the commit
     * timestamp.
     */
    if (durable_ts != WT_TS_NONE)
        WT_RET(__wt_txn_set_durable_timestamp(session, durable_ts));
    __wt_txn_publish_durable_timestamp(session);

    /* Look for a read timestamp. */
    if (read_ts != WT_TS_NONE)
        WT_RET(__wt_txn_set_read_timestamp(session, read_ts));

    /* Look for a prepare timestamp. */
    if (prepare_ts != WT_TS_NONE)
        WT_RET(__wt_txn_set_prepare_timestamp(session, prepare_ts));

    /* Timestamps are only logged in debugging mode. */
    if (set_ts && FLD_ISSET(conn->log_flags, WT_CONN_LOG_DEBUG_MODE) &&
      FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED) && !F_ISSET(conn, WT_CONN_RECOVERING))
        WT_RET(__wt_txn_ts_log(session));

    return (0);
}

/*
 * __wt_txn_publish_durable_timestamp --
 *     Publish a transaction's durable timestamp.
 */
void
__wt_txn_publish_durable_timestamp(WT_SESSION_IMPL *session)
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
 * __wt_txn_clear_durable_timestamp --
 *     Clear a transaction's published durable timestamp.
 */
void
__wt_txn_clear_durable_timestamp(WT_SESSION_IMPL *session)
{
    WT_TXN *txn;
    WT_TXN_SHARED *txn_shared;

    txn = session->txn;
    txn_shared = WT_SESSION_TXN_SHARED(session);

    if (!F_ISSET(txn, WT_TXN_SHARED_TS_DURABLE))
        return;

    WT_WRITE_BARRIER();
    F_CLR(txn, WT_TXN_SHARED_TS_DURABLE);
    txn_shared->pinned_durable_timestamp = WT_TS_NONE;
}

/*
 * __wt_txn_clear_read_timestamp --
 *     Clear a transaction's published read timestamp.
 */
void
__wt_txn_clear_read_timestamp(WT_SESSION_IMPL *session)
{
    WT_TXN *txn;
    WT_TXN_SHARED *txn_shared;

    txn = session->txn;
    txn_shared = WT_SESSION_TXN_SHARED(session);

    if (F_ISSET(txn, WT_TXN_SHARED_TS_READ)) {
        /* Assert the read timestamp is greater than or equal to the pinned timestamp. */
        WT_ASSERT(session, txn_shared->read_timestamp >= S2C(session)->txn_global.pinned_timestamp);

        WT_WRITE_BARRIER();
        F_CLR(txn, WT_TXN_SHARED_TS_READ);
    }
    txn_shared->read_timestamp = WT_TS_NONE;
}
