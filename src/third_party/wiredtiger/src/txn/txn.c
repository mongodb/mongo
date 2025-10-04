/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __snapsort_partition --
 *     Custom quick sort partitioning for snapshots.
 */
static uint32_t
__snapsort_partition(uint64_t *array, uint32_t f, uint32_t l, uint64_t pivot)
{
    uint32_t i, j;

    i = f - 1;
    j = l + 1;
    for (;;) {
        while (pivot < array[--j])
            ;
        while (array[++i] < pivot)
            ;
        if (i < j) {
            uint64_t tmp = array[i];
            array[i] = array[j];
            array[j] = tmp;
        } else
            return (j);
    }
}

/*
 * __snapsort_impl --
 *     Custom quick sort implementation for snapshots.
 */
static void
__snapsort_impl(uint64_t *array, uint32_t f, uint32_t l)
{
    while (f + 16 < l) {
        uint64_t v1 = array[f], v2 = array[l], v3 = array[(f + l) / 2];
        uint64_t median =
          v1 < v2 ? (v3 < v1 ? v1 : WT_MIN(v2, v3)) : (v3 < v2 ? v2 : WT_MIN(v1, v3));
        uint32_t m = __snapsort_partition(array, f, l, median);
        __snapsort_impl(array, f, m);
        f = m + 1;
    }
}

/*
 * __snapsort --
 *     Sort an array of transaction IDs.
 */
static void
__snapsort(uint64_t *array, uint32_t size)
{
    __snapsort_impl(array, 0, size - 1);
    WT_INSERTION_SORT(array, size, uint64_t, WT_TXNID_LT);
}

/*
 * __txn_sort_snapshot --
 *     Sort a snapshot for faster searching and set the min/max bounds.
 */
static void
__txn_sort_snapshot(WT_SESSION_IMPL *session, uint32_t n, uint64_t snap_max)
{
    WT_TXN *txn;

    txn = session->txn;

    if (n > 1)
        __snapsort(txn->snapshot_data.snapshot, n);

    txn->snapshot_data.snapshot_count = n;
    txn->snapshot_data.snap_max = snap_max;
    txn->snapshot_data.snap_min = (n > 0 && txn->snapshot_data.snapshot[0] <= snap_max) ?
      txn->snapshot_data.snapshot[0] :
      snap_max;
    F_SET(txn, WT_TXN_HAS_SNAPSHOT);
    WT_ASSERT(session, n == 0 || txn->snapshot_data.snap_min != WT_TXN_NONE);
}

/*
 * __wt_txn_release_snapshot --
 *     Release the snapshot in the current transaction.
 */
void
__wt_txn_release_snapshot(WT_SESSION_IMPL *session)
{
    WT_TXN *txn;
    WT_TXN_GLOBAL *txn_global;
    WT_TXN_SHARED *txn_shared;

    txn = session->txn;
    txn_global = &S2C(session)->txn_global;
    txn_shared = WT_SESSION_TXN_SHARED(session);

    WT_ASSERT_OPTIONAL(session, WT_DIAGNOSTIC_TXN_VISIBILITY,
      __wt_atomic_loadv64(&txn_shared->pinned_id) == WT_TXN_NONE ||
        session->txn->isolation == WT_ISO_READ_UNCOMMITTED ||
        !__wt_txn_visible_all(session, __wt_atomic_loadv64(&txn_shared->pinned_id), WT_TS_NONE),
      "A transactions pinned id cannot become globally visible before its snapshot is released");

    __wt_atomic_storev64(&txn_shared->metadata_pinned, WT_TXN_NONE);
    __wt_atomic_storev64(&txn_shared->pinned_id, WT_TXN_NONE);
    F_CLR(txn, WT_TXN_REFRESH_SNAPSHOT);
    F_CLR(txn, WT_TXN_HAS_SNAPSHOT);

    /* Clear a checkpoint's pinned ID and timestamp. */
    if (WT_SESSION_IS_CHECKPOINT(session)) {
        __wt_atomic_storev64(&txn_global->checkpoint_txn_shared.pinned_id, WT_TXN_NONE);
        txn_global->checkpoint_timestamp = WT_TS_NONE;
    }

    /* Leave the generation after releasing the snapshot. */
    __wt_session_gen_leave(session, WT_GEN_HAS_SNAPSHOT);

    __txn_clear_bytes_dirty(session);
}

/*
 * __wt_txn_active --
 *     Check if a transaction is still active. If not, it is either committed, prepared, or rolled
 *     back. It is possible that we race with commit, prepare or rollback and a transaction is still
 *     active before the start of the call is eventually reported as resolved.
 */
bool
__wt_txn_active(WT_SESSION_IMPL *session, uint64_t txnid)
{
    WT_CONNECTION_IMPL *conn;
    WT_TXN_GLOBAL *txn_global;
    WT_TXN_SHARED *s;
    uint64_t oldest_id;
    uint32_t i, session_cnt;
    bool active;

    conn = S2C(session);
    txn_global = &conn->txn_global;
    active = true;
    i = 0;

    /* We're going to scan the table: wait for the lock. */
    __wt_readlock(session, &txn_global->rwlock);
    oldest_id = __wt_atomic_loadv64(&txn_global->oldest_id);

    if (txnid < oldest_id) {
        active = false;
        goto done;
    }

    /* Walk the array of concurrent transactions. */
    WT_ACQUIRE_READ_WITH_BARRIER(session_cnt, conn->session_array.cnt);
    WT_STAT_CONN_INCR(session, txn_walk_sessions);
    for (i = 0, s = txn_global->txn_shared_list; i < session_cnt; i++, s++) {
        /* If the transaction is in the list, it is uncommitted. */
        if (__wt_atomic_loadv64(&s->id) == txnid)
            goto done;
    }

    active = false;
done:
    /* We increment this stat here as the loop traversal can exit using a goto. */
    WT_STAT_CONN_INCRV(session, txn_sessions_walked, i);
    __wt_readunlock(session, &txn_global->rwlock);
    return (active);
}

/*
 * __txn_get_snapshot_int --
 *     Allocate a snapshot, optionally update our shared txn ids.
 */
static void
__txn_get_snapshot_int(WT_SESSION_IMPL *session, bool update_shared_state)
{
    WT_CONNECTION_IMPL *conn;
    WT_TXN *txn;
    WT_TXN_GLOBAL *txn_global;
    WT_TXN_SHARED *s, *txn_shared;
    uint64_t current_id, id, pinned_id, prev_oldest_id, snapshot_gen;
    uint32_t i, n, session_cnt;

    conn = S2C(session);
    txn = session->txn;
    txn_global = &conn->txn_global;
    txn_shared = WT_SESSION_TXN_SHARED(session);
    n = 0;

    /* Fast path if we already have the current snapshot. */
    if ((snapshot_gen = __wt_session_gen(session, WT_GEN_HAS_SNAPSHOT)) != 0) {
        WT_ASSERT(
          session, F_ISSET(txn, WT_TXN_HAS_SNAPSHOT) || !F_ISSET(txn, WT_TXN_REFRESH_SNAPSHOT));
        if (!F_ISSET(txn, WT_TXN_REFRESH_SNAPSHOT) &&
          snapshot_gen == __wt_gen(session, WT_GEN_HAS_SNAPSHOT))
            return;

        /* Leave the generation here and enter again later to acquire a new snapshot. */
        __wt_session_gen_leave(session, WT_GEN_HAS_SNAPSHOT);
    }
    __wt_session_gen_enter(session, WT_GEN_HAS_SNAPSHOT);

    /* We're going to scan the table: wait for the lock. */
    __wt_readlock(session, &txn_global->rwlock);

    current_id = pinned_id = __wt_atomic_loadv64(&txn_global->current);
    prev_oldest_id = __wt_atomic_loadv64(&txn_global->oldest_id);

    /*
     * Include the checkpoint transaction, if one is running: we should ignore any uncommitted
     * changes the checkpoint has written to the metadata. We don't have to keep the checkpoint's
     * changes pinned so don't go including it in the published pinned ID.
     *
     * We can assume that if a function calls without intention to publish then it is the special
     * case of checkpoint calling it twice. In which case do not include the checkpoint id.
     */
    if ((id = __wt_atomic_loadv64(&txn_global->checkpoint_txn_shared.id)) != WT_TXN_NONE) {
        if (txn->id != id)
            txn->snapshot_data.snapshot[n++] = id;
        if (update_shared_state)
            __wt_atomic_storev64(&txn_shared->metadata_pinned, id);
    }

    /* For pure read-only workloads, avoid scanning. */
    if (prev_oldest_id == current_id) {
        pinned_id = current_id;
        /* Check that the oldest ID has not moved in the meantime. */
        WT_ASSERT(session, prev_oldest_id == __wt_atomic_loadv64(&txn_global->oldest_id));
        goto done;
    }

    /* Walk the array of concurrent transactions. */
    WT_ACQUIRE_READ_WITH_BARRIER(session_cnt, conn->session_array.cnt);
    WT_STAT_CONN_INCR(session, txn_walk_sessions);
    for (i = 0, s = txn_global->txn_shared_list; i < session_cnt; i++, s++) {
        /*
         * Build our snapshot of any concurrent transaction IDs.
         *
         * Ignore:
         *  - Our own ID: we always read our own updates.
         *  - The ID if it is older than the oldest ID we saw. This
         *    can happen if we race with a thread that is allocating
         *    an ID -- the ID will not be used because the thread will
         *    keep spinning until it gets a valid one.
         *  - The ID if it is higher than the current ID we saw. This
         *    can happen if the transaction is already finished. In
         *    this case, we ignore this transaction because it would
         *    not be visible to the current snapshot.
         */
        while (s != txn_shared && (id = __wt_atomic_loadv64(&s->id)) != WT_TXN_NONE &&
          (prev_oldest_id <= id) && (id < current_id)) {
            /*
             * If the transaction is still allocating its ID, then we spin here until it gets its
             * valid ID.
             */
            WT_ACQUIRE_BARRIER();
            if (!__wt_atomic_loadv8(&s->is_allocating)) {
                /*
                 * There is still a chance that fetched ID is not valid after ID allocation, so we
                 * check again here. The read of transaction ID should be carefully ordered: we want
                 * to re-read ID from transaction state after this transaction completes ID
                 * allocation.
                 */
                WT_ACQUIRE_BARRIER();
                if (id == __wt_atomic_loadv64(&s->id)) {
                    txn->snapshot_data.snapshot[n++] = id;
                    if (id < pinned_id)
                        pinned_id = id;
                    break;
                }
            }
            WT_PAUSE();
        }
    }
    WT_STAT_CONN_INCRV(session, txn_sessions_walked, i);

    /*
     * If we got a new snapshot, update the published pinned ID for this session.
     */
    WT_ASSERT(session, prev_oldest_id <= pinned_id);
    WT_ASSERT(session, prev_oldest_id == __wt_atomic_loadv64(&txn_global->oldest_id));
done:
    if (update_shared_state)
        __wt_atomic_storev64(&txn_shared->pinned_id, pinned_id);
    __wt_readunlock(session, &txn_global->rwlock);
    __txn_sort_snapshot(session, n, current_id);
}

/*
 * __wt_txn_get_snapshot --
 *     Common case, allocate a snapshot and update our shared ids.
 */
void
__wt_txn_get_snapshot(WT_SESSION_IMPL *session)
{
    __txn_get_snapshot_int(session, true);
}

/*
 * __wt_txn_bump_snapshot --
 *     Uncommon case, allocate a snapshot but skip updating our shared ids.
 */
void
__wt_txn_bump_snapshot(WT_SESSION_IMPL *session)
{
    __txn_get_snapshot_int(session, false);
}

/*
 * __wt_txn_snapshot_save_and_refresh --
 *     Save the existing snapshot and allocate a new snapshot.
 */
int
__wt_txn_snapshot_save_and_refresh(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;
    WT_TXN *txn;

    txn = session->txn;

    WT_RET(__wt_calloc_def(session, sizeof(WT_TXN_SNAPSHOT), &txn->backup_snapshot_data));

    txn->backup_snapshot_data->snap_max = txn->snapshot_data.snap_max;
    txn->backup_snapshot_data->snap_min = txn->snapshot_data.snap_min;
    txn->backup_snapshot_data->snapshot_count = txn->snapshot_data.snapshot_count;

    WT_ERR(__wt_calloc_def(session, sizeof(uint64_t) * S2C(session)->session_array.size,
      &txn->backup_snapshot_data->snapshot));

    /* Swap the snapshot pointers. */
    __txn_swap_snapshot(&txn->snapshot_data.snapshot, &txn->backup_snapshot_data->snapshot);

    /* Get the snapshot without publishing the shared ids. */
    __wt_txn_bump_snapshot(session);

err:
    /* Free the backup_snapshot_data if the memory allocation of the underlying snapshot has failed.
     */
    if (ret != 0)
        __wt_free(session, txn->backup_snapshot_data);

    return (ret);
}

/*
 * __wt_txn_snapshot_release_and_restore --
 *     Switch back to the original snapshot.
 */
void
__wt_txn_snapshot_release_and_restore(WT_SESSION_IMPL *session)
{
    WT_TXN *txn;
    WT_TXN_SNAPSHOT *snapshot_backup;

    txn = session->txn;
    snapshot_backup = txn->backup_snapshot_data;

    txn->snapshot_data.snap_max = snapshot_backup->snap_max;
    txn->snapshot_data.snap_min = snapshot_backup->snap_min;
    txn->snapshot_data.snapshot_count = snapshot_backup->snapshot_count;

    /* Swap the snapshot pointers. */
    __txn_swap_snapshot(&snapshot_backup->snapshot, &txn->snapshot_data.snapshot);
    __wt_free(session, snapshot_backup->snapshot);
    __wt_free(session, snapshot_backup);
}

/*
 * __txn_oldest_scan --
 *     Sweep the running transactions to calculate the oldest ID required.
 */
static void
__txn_oldest_scan(WT_SESSION_IMPL *session, uint64_t *oldest_idp, uint64_t *last_runningp,
  uint64_t *metadata_pinnedp, WT_SESSION_IMPL **oldest_sessionp)
{
    WT_CONNECTION_IMPL *conn;
    WT_SESSION_IMPL *oldest_session;
    WT_TXN_GLOBAL *txn_global;
    WT_TXN_SHARED *s;
    uint64_t id, last_running, metadata_pinned, oldest_id, prev_oldest_id;
    uint32_t i, session_cnt;

    conn = S2C(session);
    txn_global = &conn->txn_global;
    oldest_session = NULL;

    /* The oldest ID cannot change while we are holding the scan lock. */
    prev_oldest_id = __wt_atomic_loadv64(&txn_global->oldest_id);
    last_running = oldest_id = __wt_atomic_loadv64(&txn_global->current);
    if ((metadata_pinned = __wt_atomic_loadv64(&txn_global->checkpoint_txn_shared.id)) ==
      WT_TXN_NONE)
        metadata_pinned = oldest_id;

    /* Walk the array of concurrent transactions. */
    WT_ACQUIRE_READ_WITH_BARRIER(session_cnt, conn->session_array.cnt);
    WT_STAT_CONN_INCR(session, txn_walk_sessions);
    for (i = 0, s = txn_global->txn_shared_list; i < session_cnt; i++, s++) {
        /* Update the last running transaction ID. */
        while ((id = __wt_atomic_loadv64(&s->id)) != WT_TXN_NONE && prev_oldest_id <= id &&
          id < last_running) {
            /*
             * If the transaction is still allocating its ID, then we spin here until it gets its
             * valid ID.
             */
            WT_ACQUIRE_BARRIER();
            if (!__wt_atomic_loadv8(&s->is_allocating)) {
                /*
                 * There is still a chance that fetched ID is not valid after ID allocation, so we
                 * check again here. The read of transaction ID should be carefully ordered: we want
                 * to re-read ID from transaction state after this transaction completes ID
                 * allocation.
                 */
                WT_ACQUIRE_BARRIER();
                if (id == __wt_atomic_loadv64(&s->id)) {
                    last_running = id;
                    break;
                }
            }
            WT_PAUSE();
        }

        /* Update the metadata pinned ID. */
        if ((id = __wt_atomic_loadv64(&s->metadata_pinned)) != WT_TXN_NONE && id < metadata_pinned)
            metadata_pinned = id;

        /*
         * !!!
         * Note: Don't ignore pinned ID values older than the previous
         * oldest ID.  Read-uncommitted operations publish pinned ID
         * values without acquiring the scan lock to protect the global
         * table.  See the comment in __wt_txn_cursor_op for more
         * details.
         */
        if ((id = __wt_atomic_loadv64(&s->pinned_id)) != WT_TXN_NONE && id < oldest_id) {
            oldest_id = id;
            oldest_session = &WT_CONN_SESSIONS_GET(conn)[i];
        }
    }
    WT_STAT_CONN_INCRV(session, txn_sessions_walked, i);

    if (last_running < oldest_id)
        oldest_id = last_running;

    /* The metadata pinned ID can't move past the oldest ID. */
    if (oldest_id < metadata_pinned)
        metadata_pinned = oldest_id;

    *last_runningp = last_running;
    *metadata_pinnedp = metadata_pinned;
    *oldest_idp = oldest_id;
    *oldest_sessionp = oldest_session;
}

/*
 * __wt_txn_update_oldest --
 *     Sweep the running transactions to update the oldest ID required.
 */
int
__wt_txn_update_oldest(WT_SESSION_IMPL *session, uint32_t flags)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_SESSION_IMPL *oldest_session;
    WT_TXN_GLOBAL *txn_global;
    uint64_t current_id, last_running, metadata_pinned, non_strict_min_threshold, oldest_id;
    uint64_t prev_last_running, prev_metadata_pinned, prev_oldest_id;
    bool strict, wait;

    conn = S2C(session);
    txn_global = &conn->txn_global;
    strict = LF_ISSET(WT_TXN_OLDEST_STRICT);
    wait = LF_ISSET(WT_TXN_OLDEST_WAIT);

    /*
     * When not in strict mode we want to avoid scanning too frequently. Set a minimum transaction
     * ID age threshold before we perform another scan.
     */
    non_strict_min_threshold = 100;

    current_id = last_running = metadata_pinned = __wt_atomic_loadv64(&txn_global->current);
    prev_last_running = __wt_atomic_loadv64(&txn_global->last_running);
    prev_metadata_pinned = __wt_atomic_loadv64(&txn_global->metadata_pinned);
    prev_oldest_id = __wt_atomic_loadv64(&txn_global->oldest_id);

    /* Try to move the pinned timestamp forward. */
    if (strict)
        __wti_txn_update_pinned_timestamp(session, false);

    /*
     * For pure read-only workloads, or if the update isn't forced and the oldest ID isn't too far
     * behind, avoid scanning.
     */
    if ((prev_oldest_id == current_id && prev_metadata_pinned == current_id) ||
      (!strict && current_id < prev_oldest_id + non_strict_min_threshold))
        return (0);

    /* First do a read-only scan. */
    if (wait)
        __wt_readlock(session, &txn_global->rwlock);
    else if ((ret = __wt_try_readlock(session, &txn_global->rwlock)) != 0)
        return (ret == EBUSY ? 0 : ret);
    __txn_oldest_scan(session, &oldest_id, &last_running, &metadata_pinned, &oldest_session);
    __wt_readunlock(session, &txn_global->rwlock);

    /*
     * If the state hasn't changed (or hasn't moved far enough for non-forced updates), give up.
     */
    if ((oldest_id == prev_oldest_id ||
          (!strict && (oldest_id < prev_oldest_id + non_strict_min_threshold))) &&
      ((last_running == prev_last_running) ||
        (!strict && last_running < prev_last_running + non_strict_min_threshold)) &&
      metadata_pinned == prev_metadata_pinned)
        return (0);

    /* It looks like an update is necessary, wait for exclusive access. */
    if (wait)
        __wt_writelock(session, &txn_global->rwlock);
    else if ((ret = __wt_try_writelock(session, &txn_global->rwlock)) != 0)
        return (ret == EBUSY ? 0 : ret);

    /*
     * If the oldest ID has been updated while we waited, don't bother scanning.
     */
    if (oldest_id <= __wt_atomic_loadv64(&txn_global->oldest_id) &&
      last_running <= __wt_atomic_loadv64(&txn_global->last_running) &&
      metadata_pinned <= __wt_atomic_loadv64(&txn_global->metadata_pinned))
        goto done;

    /*
     * Re-scan now that we have exclusive access. This is necessary because threads get transaction
     * snapshots with read locks, and we have to be sure that there isn't a thread that has got a
     * snapshot locally but not yet published its snap_min.
     */
    __txn_oldest_scan(session, &oldest_id, &last_running, &metadata_pinned, &oldest_session);

    /* Update the public IDs. */
    if (__wt_atomic_loadv64(&txn_global->metadata_pinned) < metadata_pinned)
        __wt_atomic_storev64(&txn_global->metadata_pinned, metadata_pinned);
    if (__wt_atomic_loadv64(&txn_global->oldest_id) < oldest_id)
        __wt_atomic_storev64(&txn_global->oldest_id, oldest_id);
    if (__wt_atomic_loadv64(&txn_global->last_running) < last_running) {
        __wt_atomic_storev64(&txn_global->last_running, last_running);

        /*
         * Output a verbose message about long-running transactions, but only when some progress is
         * being made.
         */
        current_id = __wt_atomic_loadv64(&txn_global->current);
        WT_ASSERT(session, oldest_id <= current_id);
        if (WT_VERBOSE_ISSET(session, WT_VERB_TRANSACTION) &&
          current_id - oldest_id > (10 * WT_THOUSAND) && oldest_session != NULL) {
            __wt_verbose(session, WT_VERB_TRANSACTION,
              "oldest id %" PRIu64 " pinned in session %" PRIu32 " [%s] with snap_min %" PRIu64,
              oldest_id, oldest_session->id, oldest_session->lastop,
              oldest_session->txn->snapshot_data.snap_min);
        }
    }

done:
    __wt_writeunlock(session, &txn_global->rwlock);
    return (ret);
}

/*
 * __txn_config_operation_timeout --
 *     Configure a transactions operation timeout duration.
 */
static int
__txn_config_operation_timeout(WT_SESSION_IMPL *session, const char *cfg[], bool start_timer)
{
    WT_CONFIG_ITEM cval;
    WT_TXN *txn;

    txn = session->txn;

    if (cfg == NULL)
        return (0);

    /* Retrieve the maximum operation time. */
    WT_RET(__wt_config_gets_def(session, cfg, "operation_timeout_ms", 0, &cval));

    /*
     * The default configuration value is 0, we can't tell if they're setting it back to 0 or, if
     * the default was automatically passed in.
     */
    if (cval.val != 0) {
        txn->operation_timeout_us = (uint64_t)(cval.val * WT_THOUSAND);
        /*
         * The op timer will generally be started on entry to the API call however when we configure
         * it internally we need to start it separately.
         */
        if (start_timer)
            __wt_op_timer_start(session);
    }
    return (0);
}

/*
 * __txn_conf_operation_timeout --
 *     Configure a transactions operation timeout duration.
 */
static int
__txn_conf_operation_timeout(WT_SESSION_IMPL *session, const WT_CONF *conf, bool start_timer)
{
    WT_CONFIG_ITEM cval;
    WT_TXN *txn;

    txn = session->txn;

    if (conf == NULL)
        return (0);

    /* Retrieve the maximum operation time, defaulting to the database-wide configuration. */
    WT_RET(__wt_conf_gets_def(session, conf, operation_timeout_ms, 0, &cval));

    /*
     * The default configuration value is 0, we can't tell if they're setting it back to 0 or, if
     * the default was automatically passed in.
     */
    if (cval.val != 0) {
        txn->operation_timeout_us = (uint64_t)(cval.val * WT_THOUSAND);
        /*
         * The op timer will generally be started on entry to the API call however when we configure
         * it internally we need to start it separately.
         */
        if (start_timer)
            __wt_op_timer_start(session);
    }
    return (0);
}

/*
 * __wt_txn_config --
 *     Configure a transaction.
 */
int
__wt_txn_config(WT_SESSION_IMPL *session, WT_CONF *conf)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    WT_TXN *txn;
    wt_timestamp_t read_ts;

    txn = session->txn;

    if (conf == NULL)
        return (0);

    WT_ERR(__wt_conf_gets_def(session, conf, isolation, 0, &cval));
    if (cval.len != 0)
        txn->isolation = WT_CONF_STRING_MATCH(snapshot, cval) ? WT_ISO_SNAPSHOT :
          WT_CONF_STRING_MATCH(read_committed, cval)          ? WT_ISO_READ_COMMITTED :
                                                                WT_ISO_READ_UNCOMMITTED;

    WT_ERR(__txn_conf_operation_timeout(session, conf, false));

    /*
     * The default sync setting is inherited from the connection, but can be overridden by an
     * explicit "sync" setting for this transaction.
     *
     * We want to distinguish between inheriting implicitly and explicitly.
     */
    F_CLR(txn, WT_TXN_SYNC_SET);
    WT_ERR(__wt_conf_gets_def(session, conf, sync, (int)UINT_MAX, &cval));
    if (cval.val == 0 || cval.val == 1)
        /*
         * This is an explicit setting of sync. Set the flag so that we know not to overwrite it in
         * commit_transaction.
         */
        F_SET(txn, WT_TXN_SYNC_SET);

    /*
     * If sync is turned off explicitly, clear the transaction's sync field.
     */
    if (cval.val == 0)
        txn->txn_log.txn_logsync = 0;

    /* Check if prepared updates should be ignored during reads. */
    WT_ERR(__wt_conf_gets_def(session, conf, ignore_prepare, 0, &cval));
    if (cval.len > 0 && WT_CONF_STRING_MATCH(force, cval))
        F_SET(txn, WT_TXN_IGNORE_PREPARE);
    else if (cval.val)
        F_SET(txn, WT_TXN_IGNORE_PREPARE | WT_TXN_READONLY);

    /* Check if commits without a timestamp are allowed. */
    WT_ERR(__wt_conf_gets_def(session, conf, no_timestamp, 0, &cval));
    if (cval.val)
        F_SET(txn, WT_TXN_TS_NOT_SET);

    /*
     * Check if the prepare timestamp and the commit timestamp of a prepared transaction need to be
     * rounded up.
     */
    WT_ERR(__wt_conf_gets_def(session, conf, Roundup_timestamps.prepared, 0, &cval));
    if (cval.val) {
        if (F_ISSET(S2C(session), WT_CONN_PRESERVE_PREPARED))
            WT_ERR_MSG(session, EINVAL,
              "cannot round up prepare timestamp to the oldest timestamp when the preserve prepare "
              "config is on");
        F_SET(txn, WT_TXN_TS_ROUND_PREPARED);
    }

    /* Check if read timestamp needs to be rounded up. */
    WT_ERR(__wt_conf_gets_def(session, conf, Roundup_timestamps.read, 0, &cval));
    if (cval.val)
        F_SET(txn, WT_TXN_TS_ROUND_READ);

    WT_ERR(__wt_conf_gets_def(session, conf, read_timestamp, 0, &cval));
    if (cval.len != 0) {
        WT_ERR(__wt_txn_parse_timestamp(session, "read timestamp", &read_ts, &cval));
        WT_ERR(__wti_txn_set_read_timestamp(session, read_ts));
    }

err:
    if (ret != 0)
        /*
         * In the event that we error during configuration we should clear the flags on the
         * transaction so they are not set in a subsequent call to transaction begin.
         */
        txn->flags = 0;
    return (ret);
}

/*
 * __wt_txn_reconfigure --
 *     WT_SESSION::reconfigure for transactions.
 */
int
__wt_txn_reconfigure(WT_SESSION_IMPL *session, const char *config)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    WT_TXN *txn;

    txn = session->txn;

    ret = __wt_config_getones(session, config, "isolation", &cval);
    if (ret == 0)
        /* Can only reconfigure this if transaction is not active. */
        WT_RET(__wt_txn_context_check(session, false));

    if (ret == 0 && cval.len != 0) {
        session->isolation = txn->isolation = WT_CONFIG_LIT_MATCH("snapshot", cval) ?
                                                          WT_ISO_SNAPSHOT :
          WT_CONFIG_LIT_MATCH("read-uncommitted", cval) ? WT_ISO_READ_UNCOMMITTED :
                                                          WT_ISO_READ_COMMITTED;
    }
    WT_RET_NOTFOUND_OK(ret);

    return (0);
}

/*
 * __txn_release --
 *     Release the resources associated with the current transaction.
 */
static void
__txn_release(WT_SESSION_IMPL *session)
{
    WT_TXN *txn;
    WT_TXN_GLOBAL *txn_global;

    txn = session->txn;
    txn_global = &S2C(session)->txn_global;

    WT_ASSERT(session, txn->mod_count == 0);

    /* Clear the transaction's ID from the global table. */
    if (WT_SESSION_IS_CHECKPOINT(session)) {
        WT_ASSERT(session, __wt_atomic_loadv64(&WT_SESSION_TXN_SHARED(session)->id) == WT_TXN_NONE);
        txn->id = WT_TXN_NONE;
        __wt_atomic_storev64(&txn_global->checkpoint_txn_shared.id, WT_TXN_NONE);

        /*
         * Be extra careful to cleanup everything for checkpoints: once the global checkpoint ID is
         * cleared, we can no longer tell if this session is doing a checkpoint.
         */
        __wt_atomic_storev32(&txn_global->checkpoint_id, 0);
    } else if (F_ISSET(txn, WT_TXN_HAS_ID)) {
        /*
         * If transaction is prepared, this would have been done in prepare.
         */
        if (!F_ISSET(txn, WT_TXN_PREPARE))
            __txn_remove_from_global_table(session);
        else
            WT_ASSERT(
              session, __wt_atomic_loadv64(&WT_SESSION_TXN_SHARED(session)->id) == WT_TXN_NONE);
        txn->id = WT_TXN_NONE;
    }

    __wti_txn_clear_durable_timestamp(session);

    /* Free the scratch buffer allocated for logging. */
    __wt_logrec_free(session, &txn->txn_log.logrec);

    /* Discard any memory from the session's stash that we can. */
    WT_ASSERT(session, __wt_session_gen(session, WT_GEN_SPLIT) == 0);
    __wt_stash_discard(session);

    /*
     * Reset the transaction state to not running and release the snapshot.
     */
    __wt_txn_release_snapshot(session);
    /* Clear the read timestamp. */
    __wti_txn_clear_read_timestamp(session);
    txn->isolation = session->isolation;

    /*
     * Ensure the transaction flags are cleared on exit
     *
     * Purposely do NOT clear the commit and durable timestamps on release. Other readers may still
     * find these transactions in the durable queue and will need to see those timestamps.
     */
    txn->flags = 0;
    txn->prepare_timestamp = WT_TS_NONE;

    /* Clear operation timer. */
    txn->operation_timeout_us = 0;
}

/*
 * __txn_prepare_rollback_restore_hs_update --
 *     Restore the history store update to the update chain.
 */
static int
__txn_prepare_rollback_restore_hs_update(
  WT_SESSION_IMPL *session, WT_CURSOR *hs_cursor, WT_PAGE *page, WT_UPDATE *upd_chain, bool commit)
{
    WT_DECL_ITEM(hs_value);
    WT_DECL_RET;
    WT_TIME_WINDOW *hs_tw;
    WT_UPDATE *tombstone, *upd;
    wt_timestamp_t durable_ts, hs_stop_durable_ts;
    size_t size, total_size;
    uint64_t type_full;
    char ts_string[3][WT_TS_INT_STRING_SIZE];

    WT_ASSERT(session, upd_chain != NULL);

    hs_tw = NULL;
    size = total_size = 0;
    tombstone = upd = NULL;

    WT_ERR(__wt_scr_alloc(session, 0, &hs_value));

    /* Get current value. */
    WT_ERR(hs_cursor->get_value(hs_cursor, &hs_stop_durable_ts, &durable_ts, &type_full, hs_value));

    /*
     * No need to restore the history store update if we want to commit the prepared update and the
     * record has a valid stop point.
     */
    if (commit && hs_stop_durable_ts != WT_TS_MAX)
        goto done;

    /* The value older than the prepared update in the history store must be a full value. */
    WT_ASSERT(session, (uint8_t)type_full == WT_UPDATE_STANDARD);

    /* Use time window in cell to initialize the update. */
    __wt_hs_upd_time_window(hs_cursor, &hs_tw);
    WT_ERR(__wt_upd_alloc(session, hs_value, WT_UPDATE_STANDARD, &upd, &size));
    upd->txnid = hs_tw->start_txn;
    upd->upd_durable_ts = hs_tw->durable_start_ts;
    upd->upd_start_ts = hs_tw->start_ts;

    total_size += size;

    __wt_verbose_debug2(session, WT_VERB_TRANSACTION,
      "update restored from history store (txnid: %" PRIu64
      ", start_ts: %s, prepare_ts: %s, durable_ts: %s",
      upd->txnid, __wt_timestamp_to_string(upd->upd_start_ts, ts_string[0]),
      __wt_timestamp_to_string(upd->prepare_ts, ts_string[1]),
      __wt_timestamp_to_string(upd->upd_durable_ts, ts_string[2]));

    /*
     * If the history store record has a valid stop time point and we want to rollback the prepared
     * update, append it.
     */
    if (hs_stop_durable_ts != WT_TS_MAX) {
        WT_ASSERT(session, hs_tw->stop_ts != WT_TS_MAX);
        WT_ERR(__wt_upd_alloc(session, NULL, WT_UPDATE_TOMBSTONE, &tombstone, &size));
        tombstone->upd_durable_ts = hs_tw->durable_stop_ts;
        tombstone->upd_start_ts = hs_tw->stop_ts;
        tombstone->txnid = hs_tw->stop_txn;
        tombstone->next = upd;
        /* Set the flag to indicate that this update has been restored from history store. */
        F_SET(tombstone, WT_UPDATE_RESTORED_FROM_HS | WT_UPDATE_HS);
        F_SET(upd, WT_UPDATE_RESTORED_FROM_HS | WT_UPDATE_HS);
        total_size += size;

        __wt_verbose_debug2(session, WT_VERB_TRANSACTION,
          "tombstone restored from history store (txnid: %" PRIu64
          ", start_ts: %s, prepare_ts:%s, durable_ts: %s",
          tombstone->txnid, __wt_timestamp_to_string(tombstone->upd_start_ts, ts_string[0]),
          __wt_timestamp_to_string(tombstone->prepare_ts, ts_string[1]),
          __wt_timestamp_to_string(tombstone->upd_durable_ts, ts_string[2]));

        upd = tombstone;
    } else
        /*
         * Set the flag to indicate that this update has been restored from history store with max
         * stop point.
         */
        F_SET(upd, WT_UPDATE_RESTORED_FROM_HS | WT_UPDATE_HS_MAX_STOP);

    /* Walk to the end of the chain and we can only have prepared updates on the update chain. */
    for (;; upd_chain = upd_chain->next) {
        WT_ASSERT(session,
          upd_chain->txnid != WT_TXN_ABORTED && upd_chain->prepare_state == WT_PREPARE_INPROGRESS);

        if (upd_chain->next == NULL)
            break;
    }

    /* Append the update to the end of the chain. */
    WT_RELEASE_WRITE_WITH_BARRIER(upd_chain->next, upd);

    __wt_cache_page_inmem_incr(session, page, total_size, false);

    if (0) {
err:
        WT_ASSERT(session, tombstone == NULL || upd == tombstone);
        __wt_free_update_list(session, &upd);
    }
done:
    __wt_scr_free(session, &hs_value);
    return (ret);
}

/*
 * __txn_search_prepared_op --
 *     Search for an operation's prepared update.
 */
static int
__txn_search_prepared_op(
  WT_SESSION_IMPL *session, WT_TXN_OP *op, WT_CURSOR **cursorp, WT_UPDATE **updp)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_TXN *txn;
    uint32_t txn_flags;
    const char *open_cursor_cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_open_cursor), NULL};

    *updp = NULL;

    txn = session->txn;

    cursor = *cursorp;
    if (cursor == NULL || CUR2BT(cursor)->id != op->btree->id) {
        *cursorp = NULL;
        if (cursor != NULL)
            WT_RET(cursor->close(cursor));
        WT_RET(__wt_open_cursor(session, op->btree->dhandle->name, NULL, open_cursor_cfg, &cursor));
        *cursorp = cursor;
    }

    /*
     * Transaction error is cleared temporarily as cursor functions are not allowed after an error.
     */
    txn_flags = FLD_MASK(txn->flags, WT_TXN_ERROR);

    /*
     * The API layer will immediately return an error if the WT_TXN_PREPARE flag is set before
     * attempting cursor operations. However, we can't clear the WT_TXN_PREPARE flag because a
     * function in the eviction flow may attempt to forcibly rollback the transaction if it is not
     * marked as a prepared transaction. The flag WT_TXN_PREPARE_IGNORE_API_CHECK is set so that
     * cursor operations can proceed without having to clear the WT_TXN_PREPARE flag.
     */
    F_SET(txn, WT_TXN_PREPARE_IGNORE_API_CHECK);

    switch (op->type) {
    case WT_TXN_OP_BASIC_COL:
    case WT_TXN_OP_INMEM_COL:
        ((WT_CURSOR_BTREE *)cursor)->iface.recno = op->u.op_col.recno;
        break;
    case WT_TXN_OP_BASIC_ROW:
    case WT_TXN_OP_INMEM_ROW:
        F_CLR(txn, txn_flags);
        __wt_cursor_set_raw_key(cursor, &op->u.op_row.key);
        F_SET(txn, txn_flags);
        break;
    case WT_TXN_OP_NONE:
    case WT_TXN_OP_REF_DELETE:
    case WT_TXN_OP_TRUNCATE_COL:
    case WT_TXN_OP_TRUNCATE_ROW:
        WT_RET_PANIC_ASSERT(session, WT_DIAGNOSTIC_PREPARED, false, WT_PANIC,
          "invalid prepared operation update type");
        break;
    }

    F_CLR(txn, txn_flags);
    WT_WITH_BTREE(session, op->btree, ret = __wt_btcur_search_prepared(cursor, updp));
    F_SET(txn, txn_flags);
    F_CLR(txn, WT_TXN_PREPARE_IGNORE_API_CHECK);
    WT_RET(ret);
    /* We resolve each prepared key exactly once. We should always find an update. */
    WT_RET_ASSERT(session, WT_DIAGNOSTIC_PREPARED, *updp != NULL, WT_NOTFOUND,
      "unable to locate update associated with a prepared operation");

    return (0);
}

/*
 * __txn_prepare_rollback_delete_key --
 *     Prepend a global visible tombstone to the head of the update chain to delete the key for
 *     prepare rollback.
 */
static int
__txn_prepare_rollback_delete_key(WT_SESSION_IMPL *session, WT_TXN_OP *op, WT_CURSOR_BTREE *cbt)
{
    WT_BTREE *btree;
    WT_DECL_RET;
    WT_UPDATE *tombstone;
    size_t not_used;

    tombstone = NULL;
    btree = S2BT(session);

    WT_ERR(__wt_upd_alloc_tombstone(session, &tombstone, &not_used));
    F_SET(tombstone, WT_UPDATE_PREPARE_ROLLBACK);
    WT_WITH_BTREE(session, op->btree,
      ret = btree->type == BTREE_ROW ?
        __wt_row_modify(cbt, &cbt->iface.key, NULL, &tombstone, WT_UPDATE_INVALID, false, false) :
        __wt_col_modify(cbt, cbt->recno, NULL, &tombstone, WT_UPDATE_INVALID, false, false));
    WT_ERR(ret);
    tombstone = NULL;

err:
    __wt_free(session, tombstone);
    return (ret);
}

/*
 * __txn_resolve_prepared_update_chain --
 *     Helper for resolving updates. Recursively visit the update chain and resolve the updates on
 *     the way back out, so older updates are resolved first; this avoids a race with reconciliation
 *     (see WT-6778).
 */
static void
__txn_resolve_prepared_update_chain(WT_SESSION_IMPL *session, WT_UPDATE *upd, bool commit)
{
    WT_TXN *txn;

    txn = session->txn;

    /*
     * Aborted updates can exist in the update chain of our transaction. Generally this will occur
     * due to a reserved update. As such we should skip over these updates entirely.
     */
    while (upd != NULL && upd->txnid == WT_TXN_ABORTED)
        upd = upd->next;

    /*
     * The previous loop exits on null, check that here. Additionally if the transaction id is then
     * different or update's state is not in progress, we know we've reached the end of our update
     * chain and don't need to look deeper.
     */
    if (upd == NULL || (upd->txnid != WT_TXN_NONE && upd->txnid != session->txn->id))
        return;

    if (upd->prepare_state != WT_PREPARE_INPROGRESS)
        return;

    WT_ASSERT(session,
      !F_ISSET(S2C(session), WT_CONN_PRESERVE_PREPARED) || upd->prepared_id == txn->prepared_id);

    /* Go down the chain. Do the resolves on the way back up. */
    __txn_resolve_prepared_update_chain(session, upd->next, commit);

    if (!commit) {
        /* As updating timestamp might not be an atomic operation, we will manage using state. */
        upd->prepare_state = WT_PREPARE_LOCKED;
        WT_RELEASE_BARRIER();
        if (F_ISSET(txn, WT_TXN_HAS_TS_ROLLBACK))
            upd->upd_rollback_ts = txn->rollback_timestamp;
        upd->upd_saved_txnid = upd->txnid;
        WT_RELEASE_WRITE(upd->txnid, WT_TXN_ABORTED);
        WT_RELEASE_WRITE(upd->prepare_state, WT_PREPARE_INPROGRESS);
        WT_STAT_CONN_INCR(session, txn_prepared_updates_rolledback);
        return;
    }

    /*
     * Performing an update on the same key where the truncate operation is performed can lead to
     * updates that are already resolved in the updated list. Ignore the already resolved updates.
     */
    if (upd->prepare_state == WT_PREPARE_RESOLVED) {
        WT_ASSERT(session, upd->type == WT_UPDATE_TOMBSTONE);
        return;
    }

    /* Resolve the prepared update to be a committed update. */
    __txn_apply_prepare_state_update(session, upd, true);

    /* Sleep for 100ms in the prepared resolution path if configured. */
    if (FLD_ISSET(S2C(session)->timing_stress_flags, WT_TIMING_STRESS_PREPARE_RESOLUTION_2))
        __wt_sleep(0, 100 * WT_THOUSAND);
    WT_STAT_CONN_INCR(session, txn_prepared_updates_committed);
}

/*
 * __txn_resolve_prepared_op --
 *     Resolve a transaction's operations indirect references.
 */
static int
__txn_resolve_prepared_op(WT_SESSION_IMPL *session, WT_TXN_OP *op, bool commit, WT_CURSOR **cursorp)
{
    WT_BTREE *btree;
    WT_CURSOR *hs_cursor;
    WT_CURSOR_BTREE *cbt;
    WT_DECL_RET;
    WT_ITEM hs_recno_key;
    WT_PAGE *page;
    WT_TIME_WINDOW tw;
    WT_TXN *txn;
    WT_UPDATE *first_committed_upd, *upd;
    WT_UPDATE *head_upd;
    uint8_t hs_recno_key_buf[WT_INTPACK64_MAXSIZE], *p, resolve_case;
    char ts_string[3][WT_TS_INT_STRING_SIZE];
    bool tw_found;

    hs_cursor = NULL;
    txn = session->txn;
#define RESOLVE_UPDATE_CHAIN 0
#define RESOLVE_PREPARE_ON_DISK 1
#define RESOLVE_IN_MEMORY 2
    WT_NOT_READ(resolve_case, RESOLVE_UPDATE_CHAIN);

    WT_RET(__txn_search_prepared_op(session, op, cursorp, &upd));

    if (commit)
        __wt_verbose_debug2(session, WT_VERB_TRANSACTION,
          "commit resolving prepared transaction with txnid: %" PRIu64
          " and timestamp: %s to commit and durable timestamps: %s, %s",
          txn->id, __wt_timestamp_to_string(txn->prepare_timestamp, ts_string[0]),
          __wt_timestamp_to_string(txn->commit_timestamp, ts_string[1]),
          __wt_timestamp_to_string(txn->durable_timestamp, ts_string[2]));
    else
        __wt_verbose_debug2(session, WT_VERB_TRANSACTION,
          "rollback resolving prepared transaction with txnid: %" PRIu64
          " and prepared timestamp: %s and rollback timestamp: %s",
          txn->id, __wt_timestamp_to_string(txn->prepare_timestamp, ts_string[0]),
          __wt_timestamp_to_string(txn->rollback_timestamp, ts_string[1]));

    /*
     * Aborted updates can exist in the update chain of our transaction due to reserved update. Skip
     * aborted update until we see the first valid update.
     */
    for (; upd != NULL && upd->txnid == WT_TXN_ABORTED; upd = upd->next)
        ;
    head_upd = upd;

    /*
     * The head of the update chain is not a prepared update, which means all the prepared updates
     * of the key are resolved. The head of the update chain can also be null in the scenario that
     * we rolled back all associated updates in the previous iteration of this function.
     */
    if (upd == NULL || upd->prepare_state != WT_PREPARE_INPROGRESS)
        goto prepare_verify;

    /* A prepared operation that is rolled back will not have a timestamp worth asserting on. */
    if (commit)
        WT_RET(
          __wt_txn_timestamp_usage_check(session, op, txn->commit_timestamp, upd->prev_durable_ts));

    for (first_committed_upd = upd; first_committed_upd != NULL &&
         (first_committed_upd->txnid == WT_TXN_ABORTED ||
           first_committed_upd->prepare_state == WT_PREPARE_INPROGRESS);
         first_committed_upd = first_committed_upd->next)
        ;

    /*
     * Get the underlying btree and the in-memory page with the prepared updates that are to be
     * resolved. The hazard pointer on the page is already acquired during the cursor search
     * operation to prevent eviction evicting the page while resolving the prepared updates.
     */
    cbt = (WT_CURSOR_BTREE *)(*cursorp);
    page = cbt->ref->page;

    /*
     * If the prepared update is a single tombstone, we don't need to do anything special and we can
     * directly resolve it in memory.
     *
     * If the prepared update is not a tombstone or we have multiple prepared updates in the same
     * transaction. There are four base cases:
     *
     * 1) Prepared updates are on the update chain.
     *     commit: simply resolve the updates on chain.
     *     rollback: simply resolve the updates on chain.
     *
     * 2) Prepared updates are written to the data store.
     *     If there is no older updates written to the history store:
     *         commit: simply resolve the prepared updates in memory.
     *         rollback: delete the whole key.
     *
     *     If there are older updates written to the history store:
     *         commit: restore the newest history store update with a max stop time point to the
     *                 update chain. Reconciliation should know when to delete it from the history
     *                 store.
     *         rollback:restore the newest update in the history store to the update chain.
     *                  Reconciliation should know when to delete it from the history store.
     *
     * 4) We are running an in-memory database:
     *     commit: resolve the prepared updates in memory.
     *     rollback: if the prepared update is written to the disk image, delete the whole key.
     */
    btree = S2BT(session);

    /*
     * We also need to handle the on disk prepared updates if we have a prepared delete and a
     * prepared update on the disk image.
     */
    if (F_ISSET(upd, WT_UPDATE_PREPARE_RESTORED_FROM_DS) &&
      (upd->type != WT_UPDATE_TOMBSTONE || (upd->next != NULL && upd->txnid == upd->next->txnid)))
        resolve_case = RESOLVE_PREPARE_ON_DISK;
    else if (F_ISSET(S2C(session), WT_CONN_IN_MEMORY) || F_ISSET(btree, WT_BTREE_IN_MEMORY))
        resolve_case = RESOLVE_IN_MEMORY;
    else
        resolve_case = RESOLVE_UPDATE_CHAIN;

    switch (resolve_case) {
    case RESOLVE_UPDATE_CHAIN:
        /*
         * If the prepared update is the only update on the update chain and there is no on-disk
         * value. Delete the key with a tombstone.
         */
        if (!commit && first_committed_upd == NULL) {
            tw_found = __wt_read_cell_time_window(cbt, &tw);
            if (!tw_found)
                WT_ERR(__txn_prepare_rollback_delete_key(session, op, cbt));
            else
                WT_ASSERT_ALWAYS(
                  session, !WT_TIME_WINDOW_HAS_PREPARE(&tw), "no committed update to fallback to.");
        }
        break;
    case RESOLVE_PREPARE_ON_DISK:
        /*
         * Open a history store table cursor and scan the history store for the given btree and key
         * with maximum start timestamp to let the search point to the last version of the key.
         */
        WT_ERR(__wt_curhs_open(session, btree->id, NULL, &hs_cursor));
        F_SET(hs_cursor, WT_CURSTD_HS_READ_COMMITTED);
        if (btree->type == BTREE_ROW)
            hs_cursor->set_key(hs_cursor, 4, btree->id, &cbt->iface.key, WT_TS_MAX, UINT64_MAX);
        else {
            p = hs_recno_key_buf;
            WT_ERR(__wt_vpack_uint(&p, 0, cbt->recno));
            hs_recno_key.data = hs_recno_key_buf;
            hs_recno_key.size = WT_PTRDIFF(p, hs_recno_key_buf);
            hs_cursor->set_key(hs_cursor, 4, btree->id, &hs_recno_key, WT_TS_MAX, UINT64_MAX);
        }
        /*
         * Locate the previous update from the history store. We know there may be content in the
         * history store if the prepared update is written to the disk image.
         *
         * We need to locate the history store update before we resolve the prepared updates because
         * if we abort the prepared updates first, the history store search may race with other
         * sessions modifying the same key and checkpoint moving the new updates to the history
         * store.
         */
        WT_ERR_NOTFOUND_OK(__wt_curhs_search_near_before(session, hs_cursor), true);

        if (ret == 0)
            /* Restore the history store update to the update chain. */
            WT_ERR(__txn_prepare_rollback_restore_hs_update(session, hs_cursor, page, upd, commit));
        else {
            ret = 0;
            /*
             * Allocate a tombstone and prepend it to the row so when we reconcile the update chain
             * we don't copy the prepared cell, which is now associated with a rolled back prepare,
             * and instead write nothing.
             */
            if (!commit)
                WT_ERR(__txn_prepare_rollback_delete_key(session, op, cbt));
        }
        break;
    case RESOLVE_IN_MEMORY:
        /*
         * For in-memory configurations of WiredTiger if a prepared update is reconciled and then
         * rolled back, the on-page value will not be marked as aborted until the next eviction. In
         * the special case where this rollback operation results in the update chain being entirely
         * comprised of aborted updates, other transactions attempting to write to the same key will
         * look at the on-page value, think the prepared transaction is still active, and falsely
         * report a write conflict. To prevent this scenario, prepend a tombstone to the update
         * chain.
         */
        if (!commit && first_committed_upd == NULL) {
            tw_found = __wt_read_cell_time_window(cbt, &tw);
            if (tw_found && WT_TIME_WINDOW_HAS_PREPARE(&tw))
                WT_ERR(__txn_prepare_rollback_delete_key(session, op, cbt));
        }
        break;
    default:
        WT_ERR_PANIC(
          session, WT_PANIC, "invalid prepared operation resolve case: %d", resolve_case);
        break;
    }

    /*
     * Newer updates are inserted at head of update chain, and transaction operations are added at
     * the tail of the transaction modify chain.
     *
     * For example, a transaction has modified [k,v] as
     *	[k, v]  -> [k, u1]   (txn_op : txn_op1)
     *	[k, u1] -> [k, u2]   (txn_op : txn_op2)
     *	update chain : u2->u1
     *	txn_mod      : txn_op1->txn_op2.
     *
     * Only the key is saved in the transaction operation structure, hence we cannot identify
     * whether "txn_op1" corresponds to "u2" or "u1" during commit/rollback.
     *
     * To make things simpler we will handle all the updates that match the key saved in a
     * transaction operation in a single go. As a result, multiple updates of a key, if any will be
     * resolved as part of the first transaction operation resolution of that key, and subsequent
     * transaction operation resolution of the same key will be effectively a no-op.
     *
     * In the above example, we will resolve "u2" and "u1" as part of resolving "txn_op1" and will
     * not do any significant thing as part of "txn_op2".
     */
    __txn_resolve_prepared_update_chain(session, upd, commit);

    /* Mark the page dirty once the prepared updates are resolved. */
    __wt_page_modify_set(session, page);

prepare_verify:
    /*
     * If we are committing a prepared transaction we can check that we resolved the whole update
     * chain. As long as we don't walk past a globally visible update we are guaranteed that the
     * update chain won't be freed concurrently. In the commit case prepared updates cannot become
     * globally visible before we finish resolving them, this is an implicit contract within
     * WiredTiger.
     *
     * In the rollback case the updates are changed to aborted and in theory a newer update could be
     * added to the chain concurrently and become globally visible. Thus our updates could be freed.
     * We don't walk the chain in rollback for that reason.
     */
    if (EXTRA_DIAGNOSTICS_ENABLED(session, WT_DIAGNOSTIC_PREPARED) && commit) {
        for (; head_upd != NULL; head_upd = head_upd->next) {
            /*
             * Ignore aborted updates. We could have them in the middle of the relevant update
             * chain, as a result of the cursor reserve API.
             */
            if (head_upd->txnid == WT_TXN_ABORTED)
                continue;
            /*
             * Exit once we have visited all updates from the current transaction. When a
             * transaction is claim prepared, we don't assign a txn id to it so the txn id can be 0.
             * which is the same with head_upd if it's restored from disk. Break if we see a
             * different txn id (fuzzy checkpoint), or see a different prepared id (precise
             * checkpoint)
             */
            if (head_upd->txnid != txn->id || head_upd->prepared_id != txn->prepared_id)
                break;
            /* Any update we find should be resolved. */
            WT_ASSERT_ALWAYS(session, head_upd->prepare_state == WT_PREPARE_RESOLVED,
              "A prepared update wasn't resolved when it should be");
        }
    }

err:
    if (hs_cursor != NULL)
        WT_TRET(hs_cursor->close(hs_cursor));
    return (ret);
}

/*
 * __txn_mod_sortable_key --
 *     Given an operation return a boolean indicating if it has a sortable key.
 */
static WT_INLINE bool
__txn_mod_sortable_key(WT_TXN_OP *opt)
{
    switch (opt->type) {
    case (WT_TXN_OP_NONE):
    case (WT_TXN_OP_REF_DELETE):
    case (WT_TXN_OP_TRUNCATE_COL):
    case (WT_TXN_OP_TRUNCATE_ROW):
        return (false);
    case (WT_TXN_OP_BASIC_COL):
    case (WT_TXN_OP_BASIC_ROW):
    case (WT_TXN_OP_INMEM_COL):
    case (WT_TXN_OP_INMEM_ROW):
        return (true);
    }
    __wt_abort(NULL);
    return (false);
}

/*
 * __txn_mod_compare --
 *     Qsort comparison routine for transaction modify list.
 */
static int WT_CDECL
__txn_mod_compare(const void *a, const void *b)
{
    WT_TXN_OP *aopt, *bopt;
    bool a_has_sortable_key;
    bool b_has_sortable_key;

    aopt = (WT_TXN_OP *)a;
    bopt = (WT_TXN_OP *)b;

    /*
     * We want to sort on two things:
     *  - B-tree ID
     *  - Key
     * However, there are a number of modification types that don't have a key to be sorted on. This
     * requires us to add a stage between sorting on B-tree ID and key. At this intermediate stage,
     * we sort on whether the modifications have a key.
     *
     * We need to uphold the contract that all modifications on the same key are contiguous in the
     * final modification array. Technically they could be separated by non key modifications,
     * but for simplicity's sake we sort them apart.
     *
     * Qsort comparators are expected to return -1 if the first argument is smaller than the second,
     * 1 if the second argument is smaller than the first, and 0 if both arguments are equal.
     */

    /* Order by b-tree ID. */
    if (aopt->btree->id < bopt->btree->id)
        return (-1);
    if (aopt->btree->id > bopt->btree->id)
        return (1);

    /*
     * Order by whether the given operation has a key. We don't want to call key compare incorrectly
     * especially given that u is a union which would create undefined behavior.
     */
    a_has_sortable_key = __txn_mod_sortable_key(aopt);
    b_has_sortable_key = __txn_mod_sortable_key(bopt);
    if (a_has_sortable_key && !b_has_sortable_key)
        return (-1);
    if (!a_has_sortable_key && b_has_sortable_key)
        return (1);
    /*
     * In the case where both arguments don't have a key they are considered to be equal, we don't
     * care exactly how they get sorted.
     */
    if (!a_has_sortable_key && !b_has_sortable_key)
        return (0);

    /* Finally, order by key. We cannot sort if there is a collator as we need a session pointer. */
    if (aopt->btree->type == BTREE_ROW) {
        return (aopt->btree->collator == NULL ?
            __wt_lex_compare(&aopt->u.op_row.key, &bopt->u.op_row.key) :
            0);
    }
    if (aopt->u.op_col.recno < bopt->u.op_col.recno)
        return (-1);
    if (aopt->u.op_col.recno > bopt->u.op_col.recno)
        return (1);
    return (0);
}

/*
 * __txn_check_if_stable_has_moved_ahead_commit_ts --
 *     Check if the stable timestamp has moved ahead of the commit timestamp.
 */
static int
__txn_check_if_stable_has_moved_ahead_commit_ts(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_TXN *txn;
    WT_TXN_GLOBAL *txn_global;

    conn = S2C(session);
    txn = session->txn;
    txn_global = &conn->txn_global;

    if (txn_global->has_stable_timestamp && txn->first_commit_timestamp != WT_TS_NONE &&
      txn_global->stable_timestamp >= txn->first_commit_timestamp)
        WT_RET_MSG(session, EINVAL,
          "Rollback the transaction because the stable timestamp has moved ahead of the commit "
          "timestamp.");

    return (0);
}

/*
 * __wt_txn_commit --
 *     Commit the current transaction.
 */
int
__wt_txn_commit(WT_SESSION_IMPL *session, const char *cfg[])
{
    struct timespec tsp;
    WT_CACHE *cache;
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_REF_STATE previous_state;
    WT_TXN *txn;
    WT_TXN_GLOBAL *txn_global;
    WT_TXN_OP *op;
    WT_UPDATE *upd;
    wt_timestamp_t candidate_durable_timestamp, prev_durable_timestamp;
#ifdef HAVE_DIAGNOSTIC
    uint32_t prepare_count;
#endif
    u_int i;
    bool cannot_fail, locked, prepare, readonly, update_durable_ts;

    conn = S2C(session);
    cache = conn->cache;
    cursor = NULL;
    txn = session->txn;
    txn_global = &conn->txn_global;
#ifdef HAVE_DIAGNOSTIC
    prepare_count = 0;
#endif
    prepare = F_ISSET(txn, WT_TXN_PREPARE);
    readonly = txn->mod_count == 0;
    cannot_fail = locked = false;

    /* Permit the commit if the transaction failed, but was read-only. */
    WT_ASSERT(session, F_ISSET(txn, WT_TXN_RUNNING));
    WT_ASSERT(session, !F_ISSET(txn, WT_TXN_ERROR) || txn->mod_count == 0);

    /* Configure the timeout for this commit operation. */
    WT_ERR(__txn_config_operation_timeout(session, cfg, true));

    /*
     * Clear the prepared round up flag if the transaction is not prepared. There is no rounding up
     * to do in that case.
     */
    if (!prepare)
        F_CLR(txn, WT_TXN_TS_ROUND_PREPARED);

    /* Set the commit and the durable timestamps. */
    WT_ERR(__wt_txn_set_timestamp(session, cfg, true));

    if (prepare) {
        if (!F_ISSET(txn, WT_TXN_HAS_TS_COMMIT))
            WT_ERR_MSG(session, EINVAL, "commit_timestamp is required for a prepared transaction");

        if (!F_ISSET(txn, WT_TXN_HAS_TS_DURABLE))
            WT_ERR_MSG(session, EINVAL, "durable_timestamp is required for a prepared transaction");

        WT_ASSERT(session, txn->prepare_timestamp <= txn->commit_timestamp);
    } else {
        if (F_ISSET(txn, WT_TXN_HAS_TS_PREPARE))
            WT_ERR_MSG(session, EINVAL, "prepare timestamp is set for non-prepared transaction");

        if (F_ISSET(txn, WT_TXN_HAS_TS_DURABLE))
            WT_ERR_MSG(session, EINVAL,
              "durable_timestamp should not be specified for non-prepared transaction");
    }

    /*
     * Release our snapshot in case it is keeping data pinned (this is particularly important for
     * checkpoints). This will not make the updates visible to other threads because we haven't
     * removed the transaction id from the global transaction table. Before releasing our snapshot,
     * copy values into any positioned cursors so they don't point to updates that could be freed
     * once we don't have a snapshot. If this transaction is prepared, then copying values would
     * have been done during prepare.
     */
    if (session->ncursors > 0 && !prepare) {
        WT_DIAGNOSTIC_YIELD;
        WT_ERR(__wt_session_copy_values(session));
    }
    __wt_txn_release_snapshot(session);

    /*
     * Resolving prepared updates is expensive. Sort prepared modifications so all updates for each
     * page within each file are done at the same time.
     */
    if (prepare)
        __wt_qsort(txn->mod, txn->mod_count, sizeof(WT_TXN_OP), __txn_mod_compare);

    /* Process updates. */
    for (i = 0, op = txn->mod; i < txn->mod_count; i++, op++) {
        switch (op->type) {
        case WT_TXN_OP_NONE:
            break;
        case WT_TXN_OP_BASIC_COL:
        case WT_TXN_OP_BASIC_ROW:
        case WT_TXN_OP_INMEM_COL:
        case WT_TXN_OP_INMEM_ROW:
            if (!prepare) {
                upd = op->u.op_upd;

                /*
                 * Switch reserved operations to abort to simplify obsolete update list truncation.
                 */
                if (upd->type == WT_UPDATE_RESERVE) {
                    upd->txnid = WT_TXN_ABORTED;
                    break;
                }

                /*
                 * Don't reset the timestamp of the history store records with history store
                 * transaction timestamp. Those records should already have the original time window
                 * when they are inserted into the history store.
                 */
                if (cache->hs_fileid != 0 && op->btree->id == cache->hs_fileid)
                    break;

                WT_ERR(__wt_txn_op_set_timestamp(session, op, true));
            } else {
                /*
                 * If an operation has the key repeated flag set, skip resolving prepared updates as
                 * the work will happen on a different modification in this txn.
                 */
                if (!F_ISSET(op, WT_TXN_OP_KEY_REPEATED))
                    WT_ERR(__txn_resolve_prepared_op(session, op, true, &cursor));

                /*
                 * Sleep for some number of updates between resolving prepared operations when
                 * configured, however, avoid causing too much stress when there are a large number
                 * of updates. Multiplying by 36 provides a reasonable chance of calling the stress
                 * (as it's a highly composite number) without exceeding a total of 36 calls over
                 * the total mod_count.
                 */
                if ((i * 36) % txn->mod_count == 0)
                    __wt_timing_stress(session, WT_TIMING_STRESS_PREPARE_RESOLUTION_1, NULL);

#ifdef HAVE_DIAGNOSTIC
                ++prepare_count;
#endif
            }
            break;
        case WT_TXN_OP_REF_DELETE:
            WT_ERR(__wt_txn_op_set_timestamp(session, op, true));
            break;
        case WT_TXN_OP_TRUNCATE_COL:
        case WT_TXN_OP_TRUNCATE_ROW:
            /* Other operations don't need timestamps. */
            break;
        }

        /* If we used the cursor to resolve prepared updates, free and clear the key. */
        if (cursor != NULL)
            __wt_buf_free(session, &cursor->key);
    }

    if (cursor != NULL) {
        WT_ERR(cursor->close(cursor));
        cursor = NULL;
    }

#ifdef HAVE_DIAGNOSTIC
    WT_ASSERT(session, txn->prepare_count == prepare_count);
    txn->prepare_count = 0;
#endif

    /* Add a 2 second wait to simulate commit transaction slowness. */
    tsp.tv_sec = 2;
    tsp.tv_nsec = 0;
    __wt_timing_stress(session, WT_TIMING_STRESS_COMMIT_TRANSACTION_SLOW, &tsp);

    /*
     * There is a possible scenario where the checkpoint can miss the normal transactions updates to
     * include whose stable timestamps are ahead of the commit timestamp. This could happen when the
     * stable timestamps moves ahead of commit timestamp after the timestamp validity check in
     * commit transaction.
     *
     * Rollback the updates of the transactions whose commit timestamp have moved ahead of the
     * stable timestamp. Enter the commit generation for transactions whose commit timestamps are
     * behind stable timestamp and let the checkpoint drain for the transactions that are currently
     * committing to include in the checkpoint.
     */
    if (!prepare) {
        __wt_session_gen_enter(session, WT_GEN_TXN_COMMIT);
        WT_ERR(__txn_check_if_stable_has_moved_ahead_commit_ts(session));
    }

    /*
     * If we are logging, write a commit log record after we have finished committing the updates
     * in-memory. Otherwise, we may still rollback if we fail.
     */
    if (txn->txn_log.logrec != NULL) {
        /* Assert environment and tree are logging compatible, the fast-check is short-hand. */
        WT_ASSERT(
          session, !F_ISSET(conn, WT_CONN_RECOVERING) && F_ISSET(&conn->log_mgr, WT_LOG_ENABLED));

        /*
         * The default sync setting is inherited from the connection, but can be overridden by an
         * explicit "sync" setting for this transaction.
         */
        WT_ERR(__wt_config_gets_def(session, cfg, "sync", 0, &cval));

        /*
         * If the user chose the default setting, check whether sync is enabled for this transaction
         * (either inherited or via begin_transaction). If sync is disabled, clear the field to
         * avoid the log write being flushed.
         *
         * Otherwise check for specific settings. We don't need to check for "on" because that is
         * the default inherited from the connection. If the user set anything in begin_transaction,
         * we only override with an explicit setting.
         */
        if (cval.len == 0) {
            if (!FLD_ISSET(txn->txn_log.txn_logsync, WT_LOG_SYNC_ENABLED) &&
              !F_ISSET(txn, WT_TXN_SYNC_SET))
                txn->txn_log.txn_logsync = 0;
        } else {
            /*
             * If the caller already set sync on begin_transaction then they should not be using
             * sync on commit_transaction. Flag that as an error.
             */
            if (F_ISSET(txn, WT_TXN_SYNC_SET))
                WT_ERR_MSG(session, EINVAL, "sync already set during begin_transaction");
            if (WT_CONFIG_LIT_MATCH("off", cval))
                txn->txn_log.txn_logsync = 0;
            /*
             * We don't need to check for "on" here because that is the default to inherit from the
             * connection setting.
             */
        }

        /*
         * We hold the visibility lock for reading from the time we write our log record until the
         * time we release our transaction so that the LSN any checkpoint gets will always reflect
         * visible data.
         */
        __wt_readlock(session, &txn_global->visibility_rwlock);
        locked = true;
        WT_ERR(__wti_txn_log_commit(session));
    }

    /*
     * !!!WARNING: Don't add anything that can fail here. We cannot fail after we have logged the
     * transaction.
     */

    /*
     * Note: we're going to commit: nothing can fail after this point. Set a check, it's too easy to
     * call an error handling macro between here and the end of the function.
     */
    cannot_fail = true;

    /*
     * Free updates.
     *
     * Resolve any fast-truncate transactions and allow eviction to proceed on instantiated pages.
     * This isn't done as part of the initial processing because until now the commit could still
     * switch to an abort. The action allowing eviction to proceed is clearing the WT_UPDATE list,
     * (if any), associated with the commit. We're the only consumer of that list and we no longer
     * need it, and eviction knows it means abort or commit has completed on instantiated pages.
     */
    for (i = 0, op = txn->mod; i < txn->mod_count; i++, op++) {
        if (op->type == WT_TXN_OP_REF_DELETE) {
            WT_REF_LOCK(session, op->u.ref, &previous_state);

            /*
             * Only two cases are possible. First: the state is WT_REF_DELETED. In this case
             * page_del cannot be NULL yet because an uncommitted operation cannot have reached
             * global visibility. Otherwise: there is an uncommitted delete operation we're
             * handling, so the page can't be in a non-deleted state, and the tree can't be
             * readonly. Therefore the page must have been instantiated, the state must be
             * WT_REF_MEM, and there should be an update list in modify->inst_updates. There may
             * also be a non-NULL page_del to update.
             */
            if (previous_state != WT_REF_DELETED) {
                WT_ASSERT(session, op->u.ref->page != NULL && op->u.ref->page->modify != NULL);
                __wt_free(session, op->u.ref->page->modify->inst_updates);
            }
            if (op->u.ref->page_del != NULL)
                op->u.ref->page_del->committed = true;
            WT_REF_UNLOCK(op->u.ref, previous_state);
        }
        __wt_txn_op_free(session, op);
    }
    txn->mod_count = 0;

    /*
     * If durable is set, we'll try to update the global durable timestamp with that value. If
     * durable isn't set, durable is implied to be the same as commit so we'll use that instead.
     */
    candidate_durable_timestamp = WT_TS_NONE;
    if (F_ISSET(txn, WT_TXN_HAS_TS_DURABLE))
        candidate_durable_timestamp = txn->durable_timestamp;
    else if (F_ISSET(txn, WT_TXN_HAS_TS_COMMIT))
        candidate_durable_timestamp = txn->commit_timestamp;

    __txn_release(session);

    /* Leave the commit generation after snapshot is released. */
    if (!prepare)
        __wt_session_gen_leave(session, WT_GEN_TXN_COMMIT);

    if (locked)
        __wt_readunlock(session, &txn_global->visibility_rwlock);

    /*
     * If we have made some updates visible, start a new snapshot generation: any cached snapshots
     * have to be refreshed.
     */
    if (!readonly)
        __wt_gen_next(session, WT_GEN_HAS_SNAPSHOT, NULL);

    /* First check if we've made something durable in the future. */
    update_durable_ts = false;
    prev_durable_timestamp = WT_TS_NONE;
    if (candidate_durable_timestamp != WT_TS_NONE) {
        prev_durable_timestamp = txn_global->durable_timestamp;
        update_durable_ts = candidate_durable_timestamp > prev_durable_timestamp;
    }

    /*
     * If it looks like we'll need to move the global durable timestamp, attempt atomic cas and
     * re-check.
     */
    if (update_durable_ts)
        while (candidate_durable_timestamp > prev_durable_timestamp) {
            if (__wt_atomic_cas64(&txn_global->durable_timestamp, prev_durable_timestamp,
                  candidate_durable_timestamp)) {
                txn_global->has_durable_timestamp = true;
                break;
            }
            prev_durable_timestamp = txn_global->durable_timestamp;
        }

    /*
     * Stable timestamp cannot be concurrently increased greater than or equal to the prepared
     * transaction's durable timestamp. Otherwise, checkpoint may only write partial updates of the
     * transaction.
     */
    if (prepare && txn->durable_timestamp <= txn_global->stable_timestamp) {
        WT_ERR(__wt_verbose_dump_sessions(session, true));
        WT_ERR_PANIC(session, WT_PANIC,
          "stable timestamp is larger than or equal to the committing prepared transaction's "
          "durable timestamp");
    }

    /*
     * We're between transactions, if we need to block for eviction, it's a good time to do so. The
     * return must reflect the transaction state, ignore any error returned, and clear the
     * WT_SESSION_SAVE_ERRORS flag to prevent errors from being saved in the session.
     */
    if (!readonly) {
        bool save_errors = F_ISSET(session, WT_SESSION_SAVE_ERRORS);
        F_CLR(session, WT_SESSION_SAVE_ERRORS);
        WT_IGNORE_RET(__wt_evict_app_assist_worker_check(session, false, false, true, NULL));
        if (save_errors)
            F_SET(session, WT_SESSION_SAVE_ERRORS);
    }
    return (0);

err:
    /*
     * Leave the commit generation in the error case.
     */
    if (!prepare)
        __wt_session_gen_leave(session, WT_GEN_TXN_COMMIT);

    if (cursor != NULL)
        WT_TRET(cursor->close(cursor));

    if (locked)
        __wt_readunlock(session, &txn_global->visibility_rwlock);

    /* Check for a failure after we can no longer fail. */
    if (cannot_fail)
        WT_RET_PANIC(session, ret,
          "failed to commit a transaction after data corruption point, failing the system");

    /*
     * Check for a prepared transaction, and quit: we can't ignore the error and we can't roll back
     * a prepared transaction.
     */
    if (prepare)
        WT_RET_PANIC(session, ret, "failed to commit prepared transaction, failing the system");

    WT_TRET(__wt_session_reset_cursors(session, false));
    WT_TRET(__wt_txn_rollback(session, cfg, false));
    return (ret);
}

/*
 * __wt_txn_prepare --
 *     Prepare the current transaction.
 */
int
__wt_txn_prepare(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_TXN *txn;
    WT_TXN_OP *op;
    WT_UPDATE *tmp, *upd;
    u_int i, prepared_updates, prepared_updates_key_repeated;

    txn = session->txn;
    prepared_updates = prepared_updates_key_repeated = 0;

    WT_ASSERT(session, F_ISSET(txn, WT_TXN_RUNNING));
    WT_ASSERT(session, !F_ISSET(txn, WT_TXN_ERROR));

    /*
     * A transaction should not have updated any of the logged tables, if debug mode logging is not
     * turned on.
     */
    if (txn->txn_log.logrec != NULL &&
      !FLD_ISSET(S2C(session)->debug_flags, WT_CONN_DEBUG_TABLE_LOGGING))
        WT_RET_MSG(session, EINVAL, "a prepared transaction cannot include a logged table");

    /* Set the prepare timestamp. */
    WT_RET(__wt_txn_set_timestamp(session, cfg, false));
    /* Set the prepared id. */
    WT_RET(__wt_txn_set_prepared_id(session, cfg));

    if (F_ISSET(S2C(session), WT_CONN_PRESERVE_PREPARED) && !F_ISSET(txn, WT_TXN_HAS_PREPARED_ID))
        WT_RET_MSG(session, EINVAL,
          "prepared_id need to be set if the preserve_prepared config is enabled.");

    if (!F_ISSET(txn, WT_TXN_HAS_TS_PREPARE))
        WT_RET_MSG(session, EINVAL, "prepare timestamp is not set");

    if (F_ISSET(txn, WT_TXN_HAS_TS_COMMIT))
        WT_RET_MSG(
          session, EINVAL, "commit timestamp must not be set before transaction is prepared");

    /*
     * We are about to release the snapshot: copy values into any positioned cursors so they don't
     * point to updates that could be freed once we don't have a snapshot.
     */
    if (session->ncursors > 0) {
        WT_DIAGNOSTIC_YIELD;
        WT_RET(__wt_session_copy_values(session));
    }
    /*
     * Release our snapshot in case it is keeping data pinned. This will not make the updates
     * visible to other threads until we remove the transaction id from the global transaction table
     * at the end of the function.
     */
    __wt_txn_release_snapshot(session);

    for (i = 0, op = txn->mod; i < txn->mod_count; i++, op++) {
        /* Assert it's not an update to the history store file. */
        WT_ASSERT(session, S2C(session)->cache->hs_fileid == 0 || !WT_IS_HS(op->btree->dhandle));

        /* Metadata updates should never be prepared. */
        WT_ASSERT(session, !WT_IS_METADATA(op->btree->dhandle));
        if (WT_IS_METADATA(op->btree->dhandle))
            continue;

        /*
         * Logged table updates should never be prepared. As these updates are immediately durable,
         * it is not possible to roll them back if the prepared transaction is rolled back.
         */
        if (F_ISSET(op->btree, WT_BTREE_LOGGED))
            WT_RET_MSG(session, ENOTSUP,
              "%s: transaction prepare is not supported on logged tables or tables without "
              "timestamps",
              op->btree->dhandle->name);
        switch (op->type) {
        case WT_TXN_OP_NONE:
            break;
        case WT_TXN_OP_BASIC_COL:
        case WT_TXN_OP_BASIC_ROW:
        case WT_TXN_OP_INMEM_COL:
        case WT_TXN_OP_INMEM_ROW:
            upd = op->u.op_upd;

            /*
             * Switch reserved operation to abort to simplify obsolete update list truncation. The
             * object free function clears the operation type so we don't try to visit this update
             * again: it can be discarded.
             */
            if (upd->type == WT_UPDATE_RESERVE) {
                upd->txnid = WT_TXN_ABORTED;
                __wt_txn_op_free(session, op);
                break;
            }

            ++prepared_updates;

            __txn_apply_prepare_state_update(session, upd, false);
            op->u.op_upd = NULL;

            /*
             * If there are older updates to this key by the same transaction, set the repeated key
             * flag on this operation. This is later used in txn commit/rollback so we only resolve
             * each set of prepared updates once. Skip reserved updates, they're ignored as they're
             * simply discarded when we find them. Also ignore updates created by instantiating fast
             * truncation pages, they aren't linked into the transaction's modify list and so can't
             * be considered.
             */
            for (tmp = upd->next; tmp != NULL; tmp = tmp->next) {
                /* We may see aborted reserve updates in between the prepared updates. */
                if (tmp->txnid == WT_TXN_ABORTED)
                    continue;

                if (tmp->txnid != upd->txnid)
                    break;

                if (tmp->type != WT_UPDATE_RESERVE &&
                  !F_ISSET(tmp, WT_UPDATE_RESTORED_FAST_TRUNCATE)) {
                    F_SET(op, WT_TXN_OP_KEY_REPEATED);
                    ++prepared_updates_key_repeated;
                    break;
                }
            }
            break;
        case WT_TXN_OP_REF_DELETE:
            __wt_txn_op_delete_apply_prepare_state(session, op, false);
            break;
        case WT_TXN_OP_TRUNCATE_COL:
        case WT_TXN_OP_TRUNCATE_ROW:
            /* Other operations don't need timestamps. */
            break;
        }
    }
    WT_STAT_CONN_INCRV(session, txn_prepared_updates, prepared_updates);
    WT_STAT_CONN_INCRV(session, txn_prepared_updates_key_repeated, prepared_updates_key_repeated);
#ifdef HAVE_DIAGNOSTIC
    txn->prepare_count = prepared_updates;
#endif

    /* Set transaction state to prepare. */
    F_SET(session->txn, WT_TXN_PREPARE);

    /*
     * Clear the transaction's ID from the global table, to facilitate prepared data visibility, but
     * not from local transaction structure.
     */
    if (F_ISSET(txn, WT_TXN_HAS_ID))
        __txn_remove_from_global_table(session);

    return (0);
}

/*
 * __wt_txn_rollback --
 *     Roll back the current transaction.
 */
int
__wt_txn_rollback(WT_SESSION_IMPL *session, const char *cfg[], bool api_call)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_TXN *txn;
    WT_TXN_OP *op;
    WT_UPDATE *upd;
    u_int i;
#ifdef HAVE_DIAGNOSTIC
    u_int prepare_count;
#endif
    bool prepare, readonly;

    cursor = NULL;
    txn = session->txn;
#ifdef HAVE_DIAGNOSTIC
    prepare_count = 0;
#endif
    prepare = F_ISSET(txn, WT_TXN_PREPARE);
    readonly = txn->mod_count == 0;

    WT_ASSERT(session, F_ISSET(txn, WT_TXN_RUNNING));

    /* Configure the timeout for this rollback operation. */
    WT_TRET(__txn_config_operation_timeout(session, cfg, true));

    /* Set the rollback timestamp if it is an user api call. */
    if (api_call)
        WT_RET(__wt_txn_set_timestamp(session, cfg, false));

    /*
     * Release our snapshot in case it is keeping data pinned. This will not make the updates
     * visible to other threads because we haven't removed the transaction id from the global
     * transaction table at the end of the function.
     */
    __wt_txn_release_snapshot(session);

    /*
     * Resolving prepared updates is expensive. Sort prepared modifications so all updates for each
     * page within each file are done at the same time.
     */
    if (prepare)
        __wt_qsort(txn->mod, txn->mod_count, sizeof(WT_TXN_OP), __txn_mod_compare);

    /* Rollback and free updates. */
    for (i = 0, op = txn->mod; i < txn->mod_count; i++, op++) {
        /* Assert it's not an update to the history store file. */
        WT_ASSERT(session, S2C(session)->cache->hs_fileid == 0 || !WT_IS_HS(op->btree->dhandle));

        /* Metadata updates should never be rolled back. */
        WT_ASSERT(session, !WT_IS_METADATA(op->btree->dhandle));
        if (WT_IS_METADATA(op->btree->dhandle))
            continue;

        /* If this is a rollback during shutdown, prepared transaction work should not be a undone
         */
        if (F_ISSET_ATOMIC_32(S2C(session), WT_CONN_CLOSING) && prepare)
            continue;

        switch (op->type) {
        case WT_TXN_OP_NONE:
            break;
        case WT_TXN_OP_BASIC_COL:
        case WT_TXN_OP_BASIC_ROW:
        case WT_TXN_OP_INMEM_COL:
        case WT_TXN_OP_INMEM_ROW:
            upd = op->u.op_upd;

            if (!prepare) {
                if (S2C(session)->cache->hs_fileid != 0 &&
                  op->btree->id == S2C(session)->cache->hs_fileid)
                    break;
                WT_ASSERT(session, upd->txnid == txn->id || upd->txnid == WT_TXN_ABORTED);
                upd->txnid = WT_TXN_ABORTED;
            } else {
                /*
                 * If an operation has the key repeated flag set, skip resolving prepared updates as
                 * the work will happen on a different modification in this txn.
                 */
                if (!F_ISSET(op, WT_TXN_OP_KEY_REPEATED))
                    WT_TRET(__txn_resolve_prepared_op(session, op, false, &cursor));
#ifdef HAVE_DIAGNOSTIC
                ++prepare_count;
#endif
            }
            break;
        case WT_TXN_OP_REF_DELETE:
            WT_TRET(__wt_delete_page_rollback(session, op));
            break;
        case WT_TXN_OP_TRUNCATE_COL:
        case WT_TXN_OP_TRUNCATE_ROW:
            /*
             * Nothing to do: these operations are only logged for recovery. The in-memory changes
             * will be rolled back with a combination of WT_TXN_OP_REF_DELETE and WT_TXN_OP_INMEM
             * operations.
             */
            break;
        }

        __wt_txn_op_free(session, op);
        /* If we used the cursor to resolve prepared updates, free and clear the key. */
        if (cursor != NULL)
            __wt_buf_free(session, &cursor->key);
    }
    txn->mod_count = 0;
#ifdef HAVE_DIAGNOSTIC
    WT_ASSERT(session, txn->prepare_count == prepare_count);
    txn->prepare_count = 0;
#endif

    if (cursor != NULL) {
        /*
         * Technically the WiredTiger API allows closing a cursor to return rollback. This is a
         * strange error to get in the rollback path so we swallow that error here. Analysis made at
         * the time suggests that it is impossible for resetting a cursor to return rollback, which
         * is called from cursor close, but we cannot guarantee it.
         *
         * Because swallowing an error that you believe cannot happen doesn't make a lot of sense we
         * assert the error is not generated in diagnostic mode.
         */
#ifdef HAVE_DIAGNOSTIC
        int ret2 = cursor->close(cursor);
        WT_ASSERT(session, ret2 != WT_ROLLBACK);
        WT_TRET(ret2);
#else
        WT_TRET_ERROR_OK(cursor->close(cursor), WT_ROLLBACK);
#endif
        cursor = NULL;
    }

    __txn_release(session);

    /*
     * We're between transactions, if we need to block for eviction, it's a good time to do so. The
     * return must reflect the transaction state, ignore any error returned, and clear the
     * WT_SESSION_SAVE_ERRORS flag to prevent errors from being saved in the session.
     */
    if (!readonly) {
        bool save_errors = F_ISSET(session, WT_SESSION_SAVE_ERRORS);
        F_CLR(session, WT_SESSION_SAVE_ERRORS);
        WT_IGNORE_RET(__wt_evict_app_assist_worker_check(session, false, false, true, NULL));
        if (save_errors)
            F_SET(session, WT_SESSION_SAVE_ERRORS);
    }
    return (ret);
}

/*
 * __wt_txn_init --
 *     Initialize a session's transaction data.
 */
int
__wt_txn_init(WT_SESSION_IMPL *session, WT_SESSION_IMPL *session_ret)
{
    WT_TXN *txn;

    /* Allocate the WT_TXN structure, including a variable length array of snapshot information. */
    WT_RET(__wt_calloc(session, 1,
      sizeof(WT_TXN) + sizeof(txn->snapshot_data.snapshot[0]) * S2C(session)->session_array.size,
      &session_ret->txn));
    txn = session_ret->txn;
    txn->snapshot_data.snapshot = txn->__snapshot;
    txn->id = WT_TXN_NONE;

    WT_ASSERT(session,
      S2C(session_ret)->txn_global.txn_shared_list == NULL ||
        __wt_atomic_loadv64(&WT_SESSION_TXN_SHARED(session_ret)->pinned_id) == WT_TXN_NONE);

    /*
     * Take care to clean these out in case we are reusing the transaction for eviction.
     */
    txn->mod = NULL;

    txn->isolation = session_ret->isolation;
    return (0);
}

/*
 * __wt_txn_init_checkpoint_cursor --
 *     Create a transaction object for a checkpoint cursor. On success, takes charge of the snapshot
 *     array passed down, which should have been allocated separately, and nulls the pointer. (On
 *     failure, the caller must destroy it.)
 */
int
__wt_txn_init_checkpoint_cursor(
  WT_SESSION_IMPL *session, WT_CKPT_SNAPSHOT *snapinfo, WT_TXN **txn_ret)
{
    WT_TXN *txn;

    /*
     * Allocate the WT_TXN structure. Don't use the variable-length array at the end, because the
     * code for reading the snapshot allocates the snapshot list itself; copying it serves no
     * purpose, and twisting up the read code to allow controlling the allocation from here is not
     * worthwhile.
     *
     * Allocate a byte at the end so that __snapshot (at the end of the struct) doesn't point at an
     * adjacent malloc block; we'd like to be able to assert that in checkpoint cursor transactions
     * snapshot doesn't point at __snapshot, to make sure an ordinary transaction doesn't flow to
     * the checkpoint cursor close function. If an adjacent malloc block, that might not be true.
     */
    WT_RET(__wt_calloc(session, 1, sizeof(WT_TXN) + 1, &txn));

    /* We have no transaction ID and won't gain one, being read-only. */
    txn->id = WT_TXN_NONE;

    /* Use snapshot isolation. */
    txn->isolation = WT_ISO_SNAPSHOT;

    /* Save the snapshot data. */
    txn->snapshot_data.snap_min = snapinfo->snapshot_min;
    txn->snapshot_data.snap_max = snapinfo->snapshot_max;
    txn->snapshot_data.snapshot = snapinfo->snapshot_txns;
    txn->snapshot_data.snapshot_count = snapinfo->snapshot_count;

    /*
     * At this point we have taken charge of the snapshot's transaction list; it has been moved to
     * the dummy transaction. Null the caller's copy so it doesn't get freed twice if something
     * above us fails after we return.
     */
    snapinfo->snapshot_txns = NULL;

    /* Set the read, stable and oldest timestamps.  */
    txn->checkpoint_read_timestamp = snapinfo->stable_ts;
    txn->checkpoint_stable_timestamp = snapinfo->stable_ts;
    txn->checkpoint_oldest_timestamp = snapinfo->oldest_ts;

    /* Set the flag that indicates if we have a timestamp. */
    if (txn->checkpoint_read_timestamp != WT_TS_NONE)
        F_SET(txn, WT_TXN_SHARED_TS_READ);

    /*
     * Set other relevant flags. Always ignore prepared values; they can get into checkpoints.
     *
     * Prepared values don't get written out by checkpoints by default, but can appear if pages get
     * evicted. So whether any given prepared value from any given prepared but yet-uncommitted
     * transaction shows up or not is arbitrary and unpredictable. Therefore, failing on it serves
     * no data integrity purpose and will only make the system flaky.
     *
     * There is a problem, however. Prepared transactions are allowed to commit before stable if
     * stable moves forward, as long as the durable timestamp is after stable. Such transactions can
     * therefore be committed after (in execution time) the checkpoint is taken but with a commit
     * timestamp less than the checkpoint's stable timestamp. They will then exist in the live
     * database and be visible if read as of the checkpoint timestamp, but not exist in the
     * checkpoint, which is inconsistent. There is probably nothing that can be done about this
     * without making prepared transactions durable in prepared state, which is a Big Deal, so
     * applications using prepared transactions and using this commit leeway need to be cognizant of
     * the issue.
     */
    F_SET(txn,
      WT_TXN_HAS_SNAPSHOT | WT_TXN_IS_CHECKPOINT | WT_TXN_READONLY | WT_TXN_RUNNING |
        WT_TXN_IGNORE_PREPARE);

    *txn_ret = txn;
    return (0);
}

/*
 * __wt_txn_close_checkpoint_cursor --
 *     Dispose of the private transaction object in a checkpoint cursor.
 */
void
__wt_txn_close_checkpoint_cursor(WT_SESSION_IMPL *session, WT_TXN **txn_arg)
{
    WT_TXN *txn;

    txn = *txn_arg;
    *txn_arg = NULL;

    /* The snapshot list isn't at the end of the transaction structure here; free it explicitly. */
    WT_ASSERT(session, txn->snapshot_data.snapshot != txn->__snapshot);
    __wt_free(session, txn->snapshot_data.snapshot);

    __wt_free(session, txn);
}

/*
 * __wt_txn_stats_update --
 *     Update the transaction statistics for return to the application.
 */
void
__wt_txn_stats_update(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_CONNECTION_STATS **stats;
    WT_TXN_GLOBAL *txn_global;
    wt_timestamp_t checkpoint_timestamp;
    wt_timestamp_t durable_timestamp;
    wt_timestamp_t oldest_active_read_timestamp;
    wt_timestamp_t pinned_timestamp;
    uint64_t checkpoint_pinned;

    conn = S2C(session);
    txn_global = &conn->txn_global;
    stats = conn->stats;
    checkpoint_pinned = __wt_atomic_loadv64(&txn_global->checkpoint_txn_shared.pinned_id);

    WT_STATP_CONN_SET(session, stats, txn_pinned_range,
      __wt_atomic_loadv64(&txn_global->current) - __wt_atomic_loadv64(&txn_global->oldest_id));

    checkpoint_timestamp = txn_global->checkpoint_timestamp;
    durable_timestamp = txn_global->durable_timestamp;
    pinned_timestamp = txn_global->pinned_timestamp;
    if (checkpoint_timestamp != WT_TS_NONE && checkpoint_timestamp < pinned_timestamp)
        pinned_timestamp = checkpoint_timestamp;
    WT_STATP_CONN_SET(session, stats, txn_pinned_timestamp, durable_timestamp - pinned_timestamp);
    WT_STATP_CONN_SET(
      session, stats, txn_pinned_timestamp_checkpoint, durable_timestamp - checkpoint_timestamp);
    WT_STATP_CONN_SET(session, stats, txn_pinned_timestamp_oldest,
      durable_timestamp - txn_global->oldest_timestamp);

    __wti_txn_get_pinned_timestamp(session, &oldest_active_read_timestamp, 0);
    if (oldest_active_read_timestamp == 0) {
        WT_STATP_CONN_SET(session, stats, txn_timestamp_oldest_active_read, 0);
        WT_STATP_CONN_SET(session, stats, txn_pinned_timestamp_reader, 0);
    } else {
        WT_STATP_CONN_SET(
          session, stats, txn_timestamp_oldest_active_read, oldest_active_read_timestamp);
        WT_STATP_CONN_SET(session, stats, txn_pinned_timestamp_reader,
          durable_timestamp - oldest_active_read_timestamp);
    }

    WT_STATP_CONN_SET(session, stats, txn_pinned_checkpoint_range,
      checkpoint_pinned == WT_TXN_NONE ?
        0 :
        __wt_atomic_loadv64(&txn_global->current) - checkpoint_pinned);
}

/*
 * __wt_txn_release_resources --
 *     Release resources for a session's transaction data.
 */
void
__wt_txn_release_resources(WT_SESSION_IMPL *session)
{
    WT_TXN *txn;

    if ((txn = session->txn) == NULL)
        return;

    WT_ASSERT(session, txn->mod_count == 0);
    __wt_free(session, txn->mod);
    txn->mod_alloc = 0;
    txn->mod_count = 0;
}

/*
 * __wt_txn_destroy --
 *     Destroy a session's transaction data.
 */
void
__wt_txn_destroy(WT_SESSION_IMPL *session)
{
    __wt_txn_release_resources(session);
    __wt_free(session, session->txn);
}

/*
 * __wt_txn_global_init --
 *     Initialize the global transaction state.
 */
int
__wt_txn_global_init(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CONNECTION_IMPL *conn;
    WT_TXN_GLOBAL *txn_global;
    WT_TXN_SHARED *s;
    u_int i;

    WT_UNUSED(cfg);
    conn = S2C(session);

    txn_global = &conn->txn_global;
    __wt_atomic_storev64(&txn_global->current, WT_TXN_FIRST);
    __wt_atomic_storev64(&txn_global->last_running, WT_TXN_FIRST);
    __wt_atomic_storev64(&txn_global->metadata_pinned, WT_TXN_FIRST);
    __wt_atomic_storev64(&txn_global->oldest_id, WT_TXN_FIRST);

    WT_RWLOCK_INIT_TRACKED(session, &txn_global->rwlock, txn_global);
    WT_RET(__wt_rwlock_init(session, &txn_global->visibility_rwlock));

    WT_RET(__wt_calloc_def(session, conn->session_array.size, &txn_global->txn_shared_list));

    for (i = 0, s = txn_global->txn_shared_list; i < conn->session_array.size; i++, s++) {
        __wt_atomic_storev64(&s->id, WT_TXN_NONE);
        __wt_atomic_storev64(&s->pinned_id, WT_TXN_NONE);
        __wt_atomic_storev64(&s->metadata_pinned, WT_TXN_NONE);
    }

    return (0);
}

/*
 * __wt_txn_global_destroy --
 *     Destroy the global transaction state.
 */
void
__wt_txn_global_destroy(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_TXN_GLOBAL *txn_global;

    conn = S2C(session);
    txn_global = &conn->txn_global;

    if (txn_global == NULL)
        return;

    __wt_rwlock_destroy(session, &txn_global->rwlock);
    __wt_rwlock_destroy(session, &txn_global->visibility_rwlock);
    __wt_free(session, txn_global->txn_shared_list);
}

/*
 * __wt_txn_activity_drain --
 *     Wait for transactions to quiesce.
 */
int
__wt_txn_activity_drain(WT_SESSION_IMPL *session)
{
    bool txn_active;

    /*
     * It's possible that the eviction server is in the middle of a long operation, with a
     * transaction ID pinned. In that case, we will loop here until the transaction ID is released,
     * when the oldest transaction ID will catch up with the current ID.
     */
    for (;;) {
        WT_RET(__wt_txn_activity_check(session, &txn_active));
        if (!txn_active)
            break;

        WT_STAT_CONN_INCR(session, txn_release_blocked);
        __wt_yield();
    }

    return (0);
}

/*
 * __wt_txn_global_shutdown --
 *     Shut down the global transaction state.
 */
int
__wt_txn_global_shutdown(WT_SESSION_IMPL *session, const char **cfg)
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_SESSION_IMPL *s;
    WT_TIMER timer;
    char conn_rts_cfg[16], ts_string[WT_TS_INT_STRING_SIZE];
    const char *ckpt_cfg;
    bool conn_is_disagg, use_timestamp;

    conn = S2C(session);
    conn_is_disagg = __wt_conn_is_disagg(session);
    use_timestamp = false;

    __wt_verbose_info(session, WT_VERB_RECOVERY_PROGRESS, "%s",
      "perform final checkpoint and shutting down the global transaction state");

    /*
     * Perform a system-wide checkpoint so that all tables are consistent with each other. All
     * transactions are resolved but ignore timestamps to make sure all data gets to disk. Do this
     * before shutting down all the subsystems. We have shut down all user sessions, but send in
     * true for waiting for internal races.
     */
    F_SET_ATOMIC_32(conn, WT_CONN_CLOSING_CHECKPOINT);
    WT_TRET(__wt_config_gets(session, cfg, "use_timestamp", &cval));
    ckpt_cfg = "use_timestamp=false";
    if (cval.val != 0) {
        ckpt_cfg = "use_timestamp=true";
        if (conn->txn_global.has_stable_timestamp)
            use_timestamp = true;
    }
    if (!F_ISSET(conn, WT_CONN_IN_MEMORY | WT_CONN_READONLY) &&
      !F_ISSET_ATOMIC_32(conn, WT_CONN_PANIC)) {
        /*
         * Perform rollback to stable to ensure that the stable version is written to disk on a
         * clean shutdown.
         */
        if (use_timestamp && !conn_is_disagg && !F_ISSET(conn, WT_CONN_PRECISE_CHECKPOINT)) {
            const char *rts_cfg[] = {
              WT_CONFIG_BASE(session, WT_CONNECTION_rollback_to_stable), NULL, NULL};
            if (conn->rts->cfg_threads_num != 0) {
                WT_RET(__wt_snprintf(
                  conn_rts_cfg, sizeof(conn_rts_cfg), "threads=%u", conn->rts->cfg_threads_num));
                rts_cfg[1] = conn_rts_cfg;
            }

            __wt_timer_start(session, &timer);
            __wt_verbose_info(session, WT_VERB_RTS,
              "[SHUTDOWN_INIT] performing shutdown rollback to stable, stable_timestamp=%s",
              __wt_timestamp_to_string(conn->txn_global.stable_timestamp, ts_string));
            WT_TRET(conn->rts->rollback_to_stable(session, rts_cfg, true));

            /* Time since the shutdown RTS has started. */
            __wt_timer_evaluate_ms(session, &timer, &conn->shutdown_timeline.rts_ms);
            if (ret != 0)
                __wt_verbose_notice(session, WT_VERB_RTS,
                  WT_RTS_VERB_TAG_SHUTDOWN_RTS
                  "performing shutdown rollback to stable failed with code %s",
                  __wt_strerror(session, ret, NULL, 0));
            else
                __wt_verbose_info(session, WT_VERB_RECOVERY_PROGRESS,
                  "shutdown rollback to stable has successfully finished and ran for %" PRIu64
                  " milliseconds",
                  conn->shutdown_timeline.rts_ms);
        } else if (conn_is_disagg)
            __wt_verbose_info(session, WT_VERB_RTS, "%s", "skipped shutdown RTS due to disagg");

        s = NULL;
        /*
         * Do shutdown checkpoint if we are not using disaggregated storage or the node still
         * consider itself the leader. If it is not the real leader, the storage layer services
         * should return an error as it is not allowed to write.
         *
         * FIXME-WT-14739: we should be able to do shutdown checkpoint for followers as well when we
         * are able to skip the shared tables in checkpoint.
         */
        if (!conn_is_disagg || conn->layered_table_manager.leader) {
            WT_TRET(__wt_open_internal_session(conn, "close_ckpt", true, 0, 0, &s));
            if (s != NULL) {
                const char *checkpoint_cfg[] = {
                  WT_CONFIG_BASE(session, WT_SESSION_checkpoint), ckpt_cfg, NULL};

                __wt_timer_start(session, &timer);

                WT_TRET(__wt_checkpoint_db(s, checkpoint_cfg, true));

                /*
                 * Mark the metadata dirty so we flush it on close, allowing recovery to be skipped.
                 */
                WT_WITH_DHANDLE(s, WT_SESSION_META_DHANDLE(s), __wt_tree_modify_set(s));

                WT_TRET(__wt_session_close_internal(s));

                /* Time since the shutdown checkpoint has started. */
                __wt_timer_evaluate_ms(session, &timer, &conn->shutdown_timeline.checkpoint_ms);
                __wt_verbose_info(session, WT_VERB_RECOVERY_PROGRESS,
                  "shutdown checkpoint has successfully finished and ran for %" PRIu64
                  " milliseconds",
                  conn->shutdown_timeline.checkpoint_ms);
            }
        }
    }

    return (ret);
}

/*
 * __wt_txn_is_blocking --
 *     Return an error if this transaction is likely blocking eviction because of a pinned
 *     transaction ID, called by eviction to determine if a worker thread should be released from
 *     eviction.
 */
int
__wt_txn_is_blocking(WT_SESSION_IMPL *session)
{
    WT_TXN *txn;
    WT_TXN_SHARED *txn_shared;
    uint64_t global_oldest;

    txn = session->txn;
    txn_shared = WT_SESSION_TXN_SHARED(session);
    global_oldest = __wt_atomic_loadv64(&S2C(session)->txn_global.oldest_id);

    /* We can't roll back prepared transactions. */
    if (F_ISSET(txn, WT_TXN_PREPARE))
        return (0);

#ifndef WT_STANDALONE_BUILD
    /*
     * FIXME: SERVER-44870
     *
     * MongoDB can't (yet) handle rolling back read only transactions. For this reason, don't check
     * unless there's at least one update or we're configured to time out thread operations (a way
     * to confirm our caller is prepared for rollback).
     */
    if (txn->mod_count == 0 && !__wt_op_timer_fired(session))
        return (0);
#else
    /*
     * Most applications that are not using transactions to read/walk with a cursor cannot handle
     * having rollback returned nor should the API reset and retry the operation, losing the
     * cursor's position. Skip the check if there are no updates, the thread operation did not time
     * out and the operation is not running in a transaction.
     */
    if (txn->mod_count == 0 && !__wt_op_timer_fired(session) && !F_ISSET(txn, WT_TXN_RUNNING))
        return (0);
#endif

    /*
     * Check if either the transaction's ID or its pinned ID is equal to the oldest transaction ID.
     */
    bool is_txn_id_global_oldest;
    if (((is_txn_id_global_oldest = __wt_atomic_loadv64(&txn_shared->id) == global_oldest)) ||
      __wt_atomic_loadv64(&txn_shared->pinned_id) == global_oldest) {
        if (is_txn_id_global_oldest)
            WT_STAT_CONN_INCR(session, txn_rollback_oldest_id);
        else
            WT_STAT_CONN_INCR(session, txn_rollback_oldest_pinned);
        WT_RET_SUB(
          session, WT_ROLLBACK, WT_OLDEST_FOR_EVICTION, WT_TXN_ROLLBACK_REASON_OLDEST_FOR_EVICTION);
    }
    return (0);
}

/*
 * __wt_verbose_dump_txn_one --
 *     Output diagnostic information about a transaction structure.
 */
int
__wt_verbose_dump_txn_one(
  WT_SESSION_IMPL *session, WT_SESSION_IMPL *txn_session, int error_code, const char *error_string)
{
    WT_DECL_ITEM(buf);
    WT_DECL_ITEM(snapshot_buf);
    WT_DECL_RET;
    WT_TXN *txn;
    WT_TXN_SHARED *txn_shared;
    uint32_t i, buf_len;
    char ckpt_lsn_str[WT_MAX_LSN_STRING];
    char ts_string[6][WT_TS_INT_STRING_SIZE];
    const char *iso_tag;

    txn = txn_session->txn;
    txn_shared = WT_SESSION_TXN_SHARED(txn_session);
    WT_ERROR_INFO *txn_err_info = &(txn_session->err_info);

    /*
     * Unless an error occurs, there's no need to print transactions without a snapshot, as they are
     * typically harmless to the database.
     */
    if (error_code == 0 && txn->isolation != WT_ISO_READ_UNCOMMITTED &&
      !F_ISSET(txn, WT_TXN_HAS_SNAPSHOT))
        return (0);

    buf_len = 512;
    WT_RET(__wt_scr_alloc(session, buf_len, &buf));
    WT_ERR(__wt_snprintf((char *)buf->data, buf_len,
      "session ID: %" PRIu32 ", txn ID: %" PRIu64 ", pinned ID: %" PRIu64
      ", metadata pinned ID: %" PRIu64 ", name: %s",
      txn_session->id, __wt_atomic_loadv64(&txn_shared->id),
      __wt_atomic_loadv64(&txn_shared->pinned_id),
      __wt_atomic_loadv64(&txn_shared->metadata_pinned),
      txn_session->name == NULL ? "EMPTY" : txn_session->name));

    if (error_code != 0)
        WT_ERR_MSG(session, error_code, "%s, %s", (char *)buf->data,
          error_string != NULL ? error_string : "");
    else
        WT_ERR(__wt_msg(session, "%s", (char *)buf->data));

    __wt_scr_free(session, &buf);

    /* Since read uncommitted isolation doesn't create snapshots, there is no need to log them. */
    if (txn->isolation == WT_ISO_READ_UNCOMMITTED)
        goto err;

    WT_NOT_READ(iso_tag, "INVALID");
    switch (txn->isolation) {
    case WT_ISO_READ_COMMITTED:
        iso_tag = "WT_ISO_READ_COMMITTED";
        break;
    case WT_ISO_SNAPSHOT:
        iso_tag = "WT_ISO_SNAPSHOT";
        break;
    case WT_ISO_READ_UNCOMMITTED:
        return (__wt_illegal_value(session, txn->isolation));
    }

    WT_ERR(__wt_scr_alloc(session, 2048, &snapshot_buf));
    WT_ERR(__wt_buf_fmt(session, snapshot_buf, "%s", "["));
    for (i = 0; i < txn->snapshot_data.snapshot_count; i++)
        WT_ERR(__wt_buf_catfmt(
          session, snapshot_buf, "%s%" PRIu64, i == 0 ? "" : ", ", txn->snapshot_data.snapshot[i]));
    WT_ERR(__wt_buf_catfmt(session, snapshot_buf, "%s", "]\0"));
    buf_len = (uint32_t)snapshot_buf->size + 512;
    WT_ERR(__wt_scr_alloc(session, buf_len, &buf));

    WT_ERR(__wt_lsn_string(&txn->ckpt_lsn, sizeof(ckpt_lsn_str), ckpt_lsn_str));

    /*
     * Dump the information of the passed transaction into a buffer, to be logged with an optional
     * error message.
     */
    WT_ERR(
      __wt_snprintf((char *)buf->data, buf_len,
        "transaction id: %" PRIu64 ", mod count: %u"
        ", snap min: %" PRIu64 ", snap max: %" PRIu64 ", snapshot count: %u"
        ", snapshot: %s"
        ", commit_timestamp: %s"
        ", durable_timestamp: %s"
        ", first_commit_timestamp: %s"
        ", prepare_timestamp: %s"
        ", prepared id: %" PRIu64 ", pinned_durable_timestamp: %s"
        ", read_timestamp: %s"
        ", checkpoint LSN: [%s]"
        ", full checkpoint: %s"
        ", flags: 0x%08" PRIx32 ", isolation: %s"
        ", last saved error code: %d"
        ", last saved sub-level error code: %d"
        ", last saved error message: %s",
        txn->id, txn->mod_count, txn->snapshot_data.snap_min, txn->snapshot_data.snap_max,
        txn->snapshot_data.snapshot_count, (char *)snapshot_buf->data,
        __wt_timestamp_to_string(txn->commit_timestamp, ts_string[0]),
        __wt_timestamp_to_string(txn->durable_timestamp, ts_string[1]),
        __wt_timestamp_to_string(txn->first_commit_timestamp, ts_string[2]),
        __wt_timestamp_to_string(txn->prepare_timestamp, ts_string[3]), txn->prepared_id,
        __wt_timestamp_to_string(txn_shared->pinned_durable_timestamp, ts_string[4]),
        __wt_timestamp_to_string(txn_shared->read_timestamp, ts_string[5]), ckpt_lsn_str,
        txn->full_ckpt ? "true" : "false", txn->flags, iso_tag, txn_err_info->err,
        txn_err_info->sub_level_err, txn_err_info->err_msg));

    /*
     * Log a message and return an error if error code and an optional error string has been passed.
     */
    if (0 != error_code) {
        WT_ERR_MSG(session, error_code, "%s, %s", (char *)buf->data,
          error_string != NULL ? error_string : "");
    } else {
        WT_ERR(__wt_msg(session, "%s", (char *)buf->data));
    }

err:
    __wt_scr_free(session, &buf);
    __wt_scr_free(session, &snapshot_buf);

    return (ret);
}

/*
 * __wt_verbose_dump_txn --
 *     Output diagnostic information about the global transaction state.
 */
int
__wt_verbose_dump_txn(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_SESSION_IMPL *sess;
    WT_TXN_GLOBAL *txn_global;
    WT_TXN_SHARED *s;
    uint32_t i, session_cnt;
    char ts_string[WT_TS_INT_STRING_SIZE];

    conn = S2C(session);
    txn_global = &conn->txn_global;

    WT_RET(__wt_msg(session, "%s", WT_DIVIDER));
    WT_RET(__wt_msg(session, "transaction state dump"));

    WT_RET(__wt_msg(session, "current ID: %" PRIu64, __wt_atomic_loadv64(&txn_global->current)));
    WT_RET(__wt_msg(
      session, "last running ID: %" PRIu64, __wt_atomic_loadv64(&txn_global->last_running)));
    WT_RET(__wt_msg(
      session, "metadata_pinned ID: %" PRIu64, __wt_atomic_loadv64(&txn_global->metadata_pinned)));
    WT_RET(__wt_msg(session, "oldest ID: %" PRIu64, __wt_atomic_loadv64(&txn_global->oldest_id)));

    WT_RET(__wt_msg(session, "durable timestamp: %s",
      __wt_timestamp_to_string(txn_global->durable_timestamp, ts_string)));
    WT_RET(__wt_msg(session, "oldest timestamp: %s",
      __wt_timestamp_to_string(txn_global->oldest_timestamp, ts_string)));
    WT_RET(__wt_msg(session, "pinned timestamp: %s",
      __wt_timestamp_to_string(txn_global->pinned_timestamp, ts_string)));
    WT_RET(__wt_msg(session, "stable timestamp: %s",
      __wt_timestamp_to_string(txn_global->stable_timestamp, ts_string)));
    WT_RET(__wt_msg(
      session, "has_durable_timestamp: %s", txn_global->has_durable_timestamp ? "yes" : "no"));
    WT_RET(__wt_msg(session, "has_oldest_timestamp: %s",
      __wt_atomic_loadbool(&txn_global->has_oldest_timestamp) ? "yes" : "no"));
    WT_RET(__wt_msg(
      session, "has_pinned_timestamp: %s", txn_global->has_pinned_timestamp ? "yes" : "no"));
    WT_RET(__wt_msg(
      session, "has_stable_timestamp: %s", txn_global->has_stable_timestamp ? "yes" : "no"));
    WT_RET(__wt_msg(session, "oldest_is_pinned: %s", txn_global->oldest_is_pinned ? "yes" : "no"));
    WT_RET(__wt_msg(session, "stable_is_pinned: %s", txn_global->stable_is_pinned ? "yes" : "no"));

    WT_RET(__wt_msg(session, "checkpoint running: %s",
      __wt_atomic_loadvbool(&txn_global->checkpoint_running) ? "yes" : "no"));
    WT_RET(
      __wt_msg(session, "checkpoint generation: %" PRIu64, __wt_gen(session, WT_GEN_CHECKPOINT)));
    WT_RET(__wt_msg(session, "checkpoint pinned ID: %" PRIu64,
      __wt_atomic_loadv64(&txn_global->checkpoint_txn_shared.pinned_id)));
    WT_RET(__wt_msg(session, "checkpoint txn ID: %" PRIu64,
      __wt_atomic_loadv64(&txn_global->checkpoint_txn_shared.id)));

    WT_ACQUIRE_READ_WITH_BARRIER(session_cnt, conn->session_array.cnt);
    WT_RET(__wt_msg(session, "session count: %" PRIu32, session_cnt));
    WT_RET(__wt_msg(session, "Transaction state of active sessions:"));

    /*
     * Walk each session transaction state and dump information. Accessing the content of session
     * handles is not thread safe, so some information may change while traversing if other threads
     * are active at the same time, which is OK since this is diagnostic code.
     */
    WT_STAT_CONN_INCR(session, txn_walk_sessions);
    for (i = 0, s = txn_global->txn_shared_list; i < session_cnt; i++, s++) {
        /* Skip sessions with no active transaction */
        if (__wt_atomic_loadv64(&s->id) == WT_TXN_NONE &&
          __wt_atomic_loadv64(&s->pinned_id) == WT_TXN_NONE)
            continue;

        sess = &WT_CONN_SESSIONS_GET(conn)[i];
        WT_RET(__wt_verbose_dump_txn_one(session, sess, 0, NULL));
    }
    WT_STAT_CONN_INCRV(session, txn_sessions_walked, i);

    return (0);
}

#ifdef HAVE_UNITTEST
int WT_CDECL
__ut_txn_mod_compare(const void *a, const void *b)
{
    return (__txn_mod_compare(a, b));
}
#endif
