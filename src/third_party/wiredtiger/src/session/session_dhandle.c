/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __session_add_dhandle --
 *     Add a handle to the session's cache.
 */
static int
__session_add_dhandle(WT_SESSION_IMPL *session)
{
    WT_DATA_HANDLE_CACHE *dhandle_cache;
    uint64_t bucket;

    /* Allocate a handle cache entry. */
    WT_RET(__wt_calloc_one(session, &dhandle_cache));

    dhandle_cache->dhandle = session->dhandle;

    bucket = dhandle_cache->dhandle->name_hash & (S2C(session)->dh_hash_size - 1);
    TAILQ_INSERT_HEAD(&session->dhandles, dhandle_cache, q);
    TAILQ_INSERT_HEAD(&session->dhhash[bucket], dhandle_cache, hashq);

    return (0);
}

/*
 * __session_discard_dhandle --
 *     Remove a data handle from the session cache.
 */
static void
__session_discard_dhandle(WT_SESSION_IMPL *session, WT_DATA_HANDLE_CACHE *dhandle_cache)
{
    uint64_t bucket;

    bucket = dhandle_cache->dhandle->name_hash & (S2C(session)->dh_hash_size - 1);
    TAILQ_REMOVE(&session->dhandles, dhandle_cache, q);
    TAILQ_REMOVE(&session->dhhash[bucket], dhandle_cache, hashq);

    WT_DHANDLE_RELEASE(dhandle_cache->dhandle);
    __wt_overwrite_and_free(session, dhandle_cache);
}

/*
 * __session_find_dhandle --
 *     Search for a data handle in the session cache.
 */
static void
__session_find_dhandle(WT_SESSION_IMPL *session, const char *uri, const char *checkpoint,
  WT_DATA_HANDLE_CACHE **dhandle_cachep)
{
    WT_DATA_HANDLE *dhandle;
    WT_DATA_HANDLE_CACHE *dhandle_cache;
    uint64_t bucket;

    dhandle = NULL;

    bucket = __wt_hash_city64(uri, strlen(uri)) & (S2C(session)->dh_hash_size - 1);
retry:
    TAILQ_FOREACH (dhandle_cache, &session->dhhash[bucket], hashq) {
        dhandle = dhandle_cache->dhandle;
        if (WT_DHANDLE_INACTIVE(dhandle) && !WT_IS_METADATA(dhandle)) {
            __session_discard_dhandle(session, dhandle_cache);
            /* We deleted our entry, retry from the start. */
            goto retry;
        }

        if (strcmp(uri, dhandle->name) != 0)
            continue;
        if (checkpoint == NULL && dhandle->checkpoint == NULL)
            break;
        if (checkpoint != NULL && dhandle->checkpoint != NULL &&
          strcmp(checkpoint, dhandle->checkpoint) == 0)
            break;
    }

    *dhandle_cachep = dhandle_cache;
}

/*
 * __wt_session_lock_dhandle --
 *     Return when the current data handle is either (a) open with the requested lock mode; or (b)
 *     closed and write locked. If exclusive access is requested and cannot be granted immediately
 *     because the handle is in use, fail with EBUSY. Here is a brief summary of how different
 *     operations synchronize using either the schema lock, handle locks or handle flags: open --
 *     one thread gets the handle exclusive, reverts to a shared handle lock once the handle is
 *     open; bulk load --
 *     sets bulk and exclusive; salvage, truncate, update, verify --
 *     hold the schema lock, get the handle exclusive, set a "special" flag; sweep --
 *     gets a write lock on the handle, doesn't set exclusive The principle is that some application
 *     operations can cause other application operations to fail (so attempting to open a cursor on
 *     a file while it is being bulk-loaded will fail), but internal or database-wide operations
 *     should not prevent application-initiated operations. For example, attempting to verify a file
 *     should not fail because the sweep server happens to be in the process of closing that file.
 */
int
__wt_session_lock_dhandle(WT_SESSION_IMPL *session, uint32_t flags, bool *is_deadp)
{
    WT_BTREE *btree;
    WT_DATA_HANDLE *dhandle;
    WT_DECL_RET;
    bool is_open, lock_busy, want_exclusive;

    *is_deadp = false;

    dhandle = session->dhandle;
    btree = dhandle->handle;
    lock_busy = false;
    want_exclusive = LF_ISSET(WT_DHANDLE_EXCLUSIVE);

    /*
     * If this session already has exclusive access to the handle, there is no point trying to lock
     * it again.
     *
     * This should only happen if a checkpoint handle is locked multiple times during a checkpoint
     * operation, or the handle is already open without any special flags. In particular, it must
     * fail if attempting to checkpoint a handle opened for a bulk load, even in the same session.
     */
    if (dhandle->excl_session == session) {
        if (!LF_ISSET(WT_DHANDLE_LOCK_ONLY) &&
          (!F_ISSET(dhandle, WT_DHANDLE_OPEN) ||
            (btree != NULL && F_ISSET(btree, WT_BTREE_SPECIAL_FLAGS))))
            return (__wt_set_return(session, EBUSY));
        ++dhandle->excl_ref;
        return (0);
    }

    /*
     * Check that the handle is open. We've already incremented the reference count, so once the
     * handle is open it won't be closed by another thread.
     *
     * If we can see the WT_DHANDLE_OPEN flag set while holding a lock on the handle, then it's
     * really open and we can start using it. Alternatively, if we can get an exclusive lock and
     * WT_DHANDLE_OPEN is still not set, we need to do the open.
     */
    for (;;) {
        /* If the handle is dead, give up. */
        if (F_ISSET(dhandle, WT_DHANDLE_DEAD)) {
            *is_deadp = true;
            return (0);
        }

        /*
         * If the handle is already open for a special operation, give up.
         */
        if (btree != NULL && F_ISSET(btree, WT_BTREE_SPECIAL_FLAGS))
            return (__wt_set_return(session, EBUSY));

        /*
         * If the handle is open, get a read lock and recheck.
         *
         * Wait for a read lock if we want exclusive access and failed to get it: the sweep server
         * may be closing this handle, and we need to wait for it to release its lock. If we want
         * exclusive access and find the handle open once we get the read lock, give up: some other
         * thread has it locked for real.
         */
        if (F_ISSET(dhandle, WT_DHANDLE_OPEN) && (!want_exclusive || lock_busy)) {
            WT_WITH_DHANDLE(session, dhandle, __wt_session_dhandle_readlock(session));
            if (F_ISSET(dhandle, WT_DHANDLE_DEAD)) {
                *is_deadp = true;
                WT_WITH_DHANDLE(session, dhandle, __wt_session_dhandle_readunlock(session));
                return (0);
            }

            is_open = F_ISSET(dhandle, WT_DHANDLE_OPEN);
            if (is_open && !want_exclusive)
                return (0);
            WT_WITH_DHANDLE(session, dhandle, __wt_session_dhandle_readunlock(session));
        } else
            is_open = false;

        /*
         * It isn't open or we want it exclusive: try to get an exclusive lock. There is some
         * subtlety here: if we race with another thread that successfully opens the file, we don't
         * want to block waiting to get exclusive access.
         */
        WT_WITH_DHANDLE(session, dhandle, ret = __wt_session_dhandle_try_writelock(session));
        if (ret == 0) {
            if (F_ISSET(dhandle, WT_DHANDLE_DEAD)) {
                *is_deadp = true;
                WT_WITH_DHANDLE(session, dhandle, __wt_session_dhandle_writeunlock(session));
                return (0);
            }

            /*
             * If it was opened while we waited, drop the write lock and get a read lock instead.
             */
            if (F_ISSET(dhandle, WT_DHANDLE_OPEN) && !want_exclusive) {
                lock_busy = false;
                WT_WITH_DHANDLE(session, dhandle, __wt_session_dhandle_writeunlock(session));
                continue;
            }

            /* We have an exclusive lock, we're done. */
            F_SET(dhandle, WT_DHANDLE_EXCLUSIVE);
            WT_ASSERT(session, dhandle->excl_session == NULL && dhandle->excl_ref == 0);
            dhandle->excl_session = session;
            dhandle->excl_ref = 1;
            WT_ASSERT(session, !F_ISSET(dhandle, WT_DHANDLE_DEAD));
            return (0);
        }
        if (ret != EBUSY || (is_open && want_exclusive) || LF_ISSET(WT_DHANDLE_LOCK_ONLY))
            return (ret);
        lock_busy = true;

        /* Give other threads a chance to make progress. */
        WT_STAT_CONN_INCR(session, dhandle_lock_blocked);
        __wt_yield();
    }
}

/*
 * __wt_session_release_dhandle --
 *     Unlock a data handle.
 */
int
__wt_session_release_dhandle(WT_SESSION_IMPL *session)
{
    WT_BTREE *btree;
    WT_DATA_HANDLE *dhandle;
    WT_DATA_HANDLE_CACHE *dhandle_cache;
    WT_DECL_RET;
    bool locked, write_locked;

    dhandle = session->dhandle;
    btree = dhandle->handle;
    write_locked = F_ISSET(dhandle, WT_DHANDLE_EXCLUSIVE);
    locked = true;

    /*
     * If we had special flags set, close the handle so that future access can get a handle without
     * special flags.
     */
    if (F_ISSET(dhandle, WT_DHANDLE_DISCARD | WT_DHANDLE_DISCARD_KILL)) {
        WT_SAVE_DHANDLE(session,
          __session_find_dhandle(session, dhandle->name, dhandle->checkpoint, &dhandle_cache));
        if (dhandle_cache != NULL)
            __session_discard_dhandle(session, dhandle_cache);
    }

    /*
     * Close the handle if we are finishing a bulk load or if the handle is set to discard on
     * release. Bulk loads are special because they may have huge root pages in memory, and we need
     * to push those pages out of the cache. The only way to do that is to close the handle.
     */
    if (btree != NULL && F_ISSET(btree, WT_BTREE_BULK)) {
        WT_ASSERT(
          session, F_ISSET(dhandle, WT_DHANDLE_EXCLUSIVE) && !F_ISSET(dhandle, WT_DHANDLE_DISCARD));
        /*
         * Acquire the schema lock while closing out the handles. This avoids racing with a
         * checkpoint while it gathers a set of handles.
         */
        WT_WITH_SCHEMA_LOCK(session, ret = __wt_conn_dhandle_close(session, false, false));
    } else if ((btree != NULL && F_ISSET(btree, WT_BTREE_SPECIAL_FLAGS)) ||
      F_ISSET(dhandle, WT_DHANDLE_DISCARD | WT_DHANDLE_DISCARD_KILL)) {
        WT_ASSERT(session, F_ISSET(dhandle, WT_DHANDLE_EXCLUSIVE));

        ret = __wt_conn_dhandle_close(session, false, F_ISSET(dhandle, WT_DHANDLE_DISCARD_KILL));
        F_CLR(dhandle, WT_DHANDLE_DISCARD | WT_DHANDLE_DISCARD_KILL);
    }

    if (session == dhandle->excl_session) {
        if (--dhandle->excl_ref == 0)
            dhandle->excl_session = NULL;
        else
            locked = false;
    }
    if (locked) {
        if (write_locked) {
            F_CLR(dhandle, WT_DHANDLE_EXCLUSIVE);
            WT_WITH_DHANDLE(session, dhandle, __wt_session_dhandle_writeunlock(session));
        } else
            WT_WITH_DHANDLE(session, dhandle, __wt_session_dhandle_readunlock(session));
    }

    session->dhandle = NULL;
    return (ret);
}

/*
 * __session_fetch_checkpoint_meta --
 *     Retrieve information about the selected checkpoint. Notes on the returned values are found
 *     under __session_lookup_checkpoint.
 */
static int
__session_fetch_checkpoint_meta(WT_SESSION_IMPL *session, const char *ckpt_name,
  WT_CKPT_SNAPSHOT *info_ret, uint64_t *snapshot_time_ret, uint64_t *stable_time_ret,
  uint64_t *oldest_time_ret)
{
    /* Get the timestamps. */
    WT_RET(__wt_meta_read_checkpoint_timestamp(
      session, ckpt_name, &info_ret->stable_ts, stable_time_ret));
    WT_RET(
      __wt_meta_read_checkpoint_oldest(session, ckpt_name, &info_ret->oldest_ts, oldest_time_ret));

    /* Get the snapshot. */
    WT_RET(__wt_meta_read_checkpoint_snapshot(session, ckpt_name, &info_ret->snapshot_write_gen,
      &info_ret->snapshot_min, &info_ret->snapshot_max, &info_ret->snapshot_txns,
      &info_ret->snapshot_count, snapshot_time_ret));

    /*
     * If we successfully read a null snapshot, set the min and max to WT_TXN_MAX so everything is
     * visible. (Whether this is desirable isn't entirely clear, but if we leave them set to
     * WT_TXN_NONE, then nothing is visible, and that's clearly not useful. The other choices are to
     * fail, which doesn't help, or to signal somehow to the checkpoint cursor that it should run
     * without a dummy transaction, which doesn't work.)
     */
    if (info_ret->snapshot_min == WT_TXN_NONE && info_ret->snapshot_max == WT_TXN_NONE) {
        info_ret->snapshot_min = info_ret->snapshot_max = WT_TXN_MAX;
        WT_ASSERT(session, info_ret->snapshot_txns == NULL && info_ret->snapshot_count == 0);
    }

    return (0);
}

/*
 * __session_fetch_checkpoint_snapshot_wall_time --
 *     Like __session_fetch_checkpoint_meta, but retrieves just the wall clock time of the snapshot.
 */
static int
__session_fetch_checkpoint_snapshot_wall_time(
  WT_SESSION_IMPL *session, const char *ckpt_name, uint64_t *walltime)
{
    return (__wt_meta_read_checkpoint_snapshot(
      session, ckpt_name, NULL, NULL, NULL, NULL, NULL, walltime));
}

/*
 * __session_open_hs_ckpt --
 *     Get a btree handle for the requested checkpoint of the history store and return it.
 */
static int
__session_open_hs_ckpt(WT_SESSION_IMPL *session, const char *checkpoint, const char *cfg[],
  uint32_t flags, int64_t order_expected, WT_DATA_HANDLE **hs_dhandlep)
{
    WT_RET(__wt_session_get_dhandle(session, WT_HS_URI, checkpoint, cfg, flags));

    if (session->dhandle->checkpoint_order != order_expected) {
        /* Not what we were expecting; treat as EBUSY and let the caller retry. */
        WT_RET(__wt_session_release_dhandle(session));
        return (__wt_set_return(session, EBUSY));
    }

    /* The handle is left in the session; return it explicitly for caller's convenience. */
    *hs_dhandlep = session->dhandle;
    return (0);
}

/*
 * __wt_session_get_btree_ckpt --
 *     Check the configuration strings for a checkpoint name. If opening a checkpoint, resolve the
 *     checkpoint name, get a btree handle for it, load that into the session, and if requested with
 *     non-null pointers, also resolve a matching history store checkpoint, open a handle for it,
 *     return that, and also find and return the corresponding snapshot/timestamp metadata. The
 *     transactions array in the snapshot info is allocated and must be freed by the caller on
 *     success. If not opening a checkpoint, the history store dhandle and snapshot info is
 *     immaterial; if the return pointers are not null, send back nulls and in particular never
 *     allocate or open anything.
 */
int
__wt_session_get_btree_ckpt(WT_SESSION_IMPL *session, const char *uri, const char *cfg[],
  uint32_t flags, WT_DATA_HANDLE **hs_dhandlep, WT_CKPT_SNAPSHOT *ckpt_snapshot)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    uint64_t ds_time, first_snapshot_time, hs_time, oldest_time, snapshot_time, stable_time;
    int64_t ds_order, hs_order;
    const char *checkpoint, *hs_checkpoint;
    bool is_hs, is_unnamed_ckpt, is_reserved_name, must_resolve;

    ds_time = first_snapshot_time = hs_time = oldest_time = snapshot_time = stable_time = 0;
    ds_order = hs_order = 0;
    checkpoint = NULL;
    hs_checkpoint = NULL;
    is_hs = is_unnamed_ckpt = is_reserved_name = must_resolve = false;

    /* These should only be set together. Asking for only one doesn't make sense. */
    WT_ASSERT(session, (hs_dhandlep == NULL) == (ckpt_snapshot == NULL));

    if (hs_dhandlep != NULL)
        *hs_dhandlep = NULL;
    if (ckpt_snapshot != NULL) {
        ckpt_snapshot->ckpt_id = 0;
        ckpt_snapshot->oldest_ts = WT_TS_NONE;
        ckpt_snapshot->stable_ts = WT_TS_NONE;
        ckpt_snapshot->snapshot_write_gen = 0;
        ckpt_snapshot->snapshot_min = WT_TXN_MAX;
        ckpt_snapshot->snapshot_max = WT_TXN_MAX;
        ckpt_snapshot->snapshot_txns = NULL;
        ckpt_snapshot->snapshot_count = 0;
    }

    /*
     * This function exists to handle checkpoint configuration. Callers that never open a checkpoint
     * call the underlying function directly.
     */
    WT_RET_NOTFOUND_OK(__wt_config_gets_def(session, cfg, "checkpoint", 0, &cval));
    if (cval.len == 0) {
        /* We are not opening a checkpoint. This is the simple case; retire it immediately. */
        return (__wt_session_get_dhandle(session, uri, NULL, cfg, flags));
    }

    /*
     * Here and below is only for checkpoints.
     *
     * Ultimately, unless we're being opened from a context where we won't ever need to access the
     * history store, we need two dhandles and a set of snapshot/timestamp info that all match.
     *
     * "Match" here is a somewhat complex issue. In the simple case, it means trees and a snapshot
     * that came from the same global checkpoint. But because checkpoints skip clean trees, either
     * tree can potentially be from an earlier global checkpoint. This means we cannot readily
     * identify matching trees by looking at them (or by looking at their metadata either) -- both
     * the order numbers and the wall clock times can easily be different. Consequently we don't try
     * to actively find or check matching trees; instead we rely on the system to not produce
     * mutually inconsistent checkpoints, and read out whatever exists taking active steps to avoid
     * racing with a currently running checkpoint.
     *
     * Note that this fundamentally relies on partial checkpoints being prohibited. In the presence
     * of partial checkpoints we would have to actively find matching trees, and in many cases
     * (because old unnamed checkpoints are garbage collected) the proper matching history store
     * wouldn't exist any more and we'd be stuck.
     *
     * The scheme is as follows: 1. Read checkpoint info out of the metadata, and retry until we get
     * a consistent set; then 2. Open both dhandles and retry the whole thing if we didn't get the
     * trees we expected.
     *
     * For the first part, we look up the requested checkpoint in both the data store and history
     * store's metadata (either by name or for WiredTigerCheckpoint by picking the most recent
     * checkpoint), and look up the snapshot and timestamps in the global metadata. For all of these
     * we retrieve the wall clock time of the checkpoint, which we'll use to check for consistency.
     * For the trees we also retrieve the order numbers of the checkpoints, which we'll use to check
     * that the dhandles we open are the ones we wanted. (For unnamed checkpoints, they must be,
     * because unnamed checkpoints are never replaced, but for named checkpoints it's possible for
     * the open to race with regeneration of the checkpoint.)
     *
     * Because the snapshot information is always written by every checkpoint, and is written last,
     * we use its wall clock time as the reference. This is always the wall clock time of the most
     * recent completed global checkpoint of the same name, or the most recent completed unnamed
     * checkpoint, as appropriate. We read this time twice, once at the very beginning and again
     * along with the snapshot information itself at the end after the other items. If these two
     * times don't match, a global checkpoint completed while we were reading. In this case we
     * cannot tell for sure if we read one of the trees' metadata before the checkpoint updated it;
     * if the tree's wall clock time is older than the snapshot's, it might be because that tree was
     * skipped, but it might also be because there was an update but we read before the update
     * happened. Therefore, we need to retry.
     *
     * If the two copies of the snapshot time match, we check the other wall clock times against the
     * snapshot time. If any of the items are newer, they were written by a currently running
     * checkpoint that hasn't finished yet, and we need to retry.
     *
     * (For the timestamps it is slightly easier; either timestamp might not be present, in which
     * case both the timestamp and its associated time will read back as zero. We take advantage of
     * the knowledge that for both these timestamps the system cannot transition from a state with
     * the timestamp set to one where it is not, and therefore once any checkpoint includes either
     * timestamp, every subsequent checkpoint will too. Therefore, the timestamps' wall times should
     * either match the snapshot or be zero; and if they're zero, it doesn't matter if they were
     * actually zero in a newer, currently running checkpoint, because then they must have always
     * been zero.)
     *
     * This scheme relies on the fact that the checkpoint wall clock time always moves forward. Each
     * checkpoint is given a wall clock time at least one second greater than the previous
     * checkpoint. Before recovery, we load the time of the last successful checkpoint in the
     * previous database so we can ensure checkpoint times increase across restarts. This avoids
     * trouble if the system clock moves backwards between runs, and also avoids possible issues if
     * the checkpoint clock runs forward. (See comment about that in
     * __txn_checkpoint_establish_time().) When reading from a previous database, the checkpoint
     * time in the snapshot and timestamp metadata default to zero if not present, avoiding
     * confusion caused by older versions that don't include these values.
     *
     * Also note that only the exact name "WiredTigerCheckpoint" needs to be resolved. Requests to
     * open specific versions, such as "WiredTigerCheckpoint.6", is forbidden.
     *
     * It is also at least theoretically possible for there to be no matching history store
     * checkpoint. If looking up the checkpoint names finds no history store checkpoint, its name
     * will come back as null and we must avoid trying to open it, either here or later on in the
     * life of the checkpoint cursor.
     */

    is_hs = strcmp(uri, WT_HS_URI) == 0;
    if (is_hs)
        /* We're opening the history store directly, so don't open it twice. */
        hs_dhandlep = NULL;

    /*
     * Applications can use the internal reserved name "WiredTigerCheckpoint" to open the latest
     * checkpoint, but they are not allowed to directly open specific checkpoint versions, such as
     * "WiredTigerCheckpoint.6". However, internally it is allowed to open the history store with a
     * specific version.
     */
    is_reserved_name = cval.len > strlen(WT_CHECKPOINT) && WT_PREFIX_MATCH(cval.str, WT_CHECKPOINT);
    if (is_reserved_name && (!is_hs || session->hs_checkpoint == NULL))
        WT_RET_MSG(
          session, EINVAL, "the prefix \"%s\" for checkpoint cursors is reserved", WT_CHECKPOINT);

    /*
     * Test for the internal checkpoint name (WiredTigerCheckpoint). Note: must_resolve is true in a
     * subset of the cases where is_unnamed_ckpt is true.
     */
    must_resolve = WT_STRING_MATCH(WT_CHECKPOINT, cval.str, cval.len);
    is_unnamed_ckpt = cval.len >= strlen(WT_CHECKPOINT) && WT_PREFIX_MATCH(cval.str, WT_CHECKPOINT);

    /* This is the top of a retry loop. */
    do {
        ret = 0;

        if (!must_resolve)
            /* Copy the checkpoint name first because we may need it to get the first wall time. */
            WT_RET(__wt_strndup(session, cval.str, cval.len, &checkpoint));

        if (ckpt_snapshot != NULL) {
            /* We're about to re-fetch this; discard the prior version. No effect the first time. */
            __wt_free(session, ckpt_snapshot->snapshot_txns);

            /*
             * Now, as the first step of the retrieval process, get the wall-clock time of the
             * snapshot metadata (only). If we need the name, we'll have copied it already.
             */
            WT_RET(__session_fetch_checkpoint_snapshot_wall_time(
              session, is_unnamed_ckpt ? NULL : checkpoint, &first_snapshot_time));
        }

        if (must_resolve)
            /* Look up the most recent data store checkpoint. This fetches the exact name to use. */
            WT_RET(__wt_meta_checkpoint_last_name(session, uri, &checkpoint, &ds_order, &ds_time));
        else
            /* Look up the checkpoint by name and get its time and order information. */
            WT_RET(__wt_meta_checkpoint_by_name(session, uri, checkpoint, &ds_order, &ds_time));

        /* Look up the history store checkpoint. */
        if (hs_dhandlep != NULL) {
            if (must_resolve)
                WT_RET_NOTFOUND_OK(__wt_meta_checkpoint_last_name(
                  session, WT_HS_URI, &hs_checkpoint, &hs_order, &hs_time));
            else {
                ret =
                  __wt_meta_checkpoint_by_name(session, WT_HS_URI, checkpoint, &hs_order, &hs_time);
                WT_RET_NOTFOUND_OK(ret);
                if (ret == WT_NOTFOUND)
                    ret = 0;
                else
                    WT_RET(__wt_strdup(session, checkpoint, &hs_checkpoint));
            }
        }

        /*
         * If we were asked for snapshot metadata, fetch it now, including the time (comparable to
         * checkpoint times) for each element.
         */
        if (ckpt_snapshot != NULL) {
            WT_RET(__session_fetch_checkpoint_meta(session, is_unnamed_ckpt ? NULL : checkpoint,
              ckpt_snapshot, &snapshot_time, &stable_time, &oldest_time));

            /*
             * If we have not raced with a checkpoint, we still may have an inconsistency in a
             * specific scenario that involves bulk operations. When a bulk operation finishes, it
             * generates a single file checkpoint which is different from a system wide checkpoint.
             * The single file checkpoint only bumps the data store time which makes it ahead of the
             * last system wide checkpoint time and leads to the inconsistency.
             */
            if (first_snapshot_time == snapshot_time && ds_time > snapshot_time &&
              hs_time <= snapshot_time) {
                /*
                 * If a system wide checkpoint is running, the inconsistency should be resolved, it
                 * is worth retrying.
                 */
                if (S2C(session)->txn_global.checkpoint_running)
                    ret = __wt_set_return(session, EBUSY);
                else {
                    __wt_verbose_warning(session, WT_VERB_DEFAULT,
                      "Session (@: 0x%p name: %s) could not open the checkpoint '%s' (config: %s) "
                      "on the file '%s'.",
                      (void *)session, session->name == NULL ? "EMPTY" : session->name, checkpoint,
                      cval.str, uri);
                    ret = __wt_set_return(session, WT_NOTFOUND);
                    goto err;
                }
            }
            /*
             * Check if we raced with a running checkpoint.
             *
             * If the two copies of the snapshot don't match, or if any of the other metadata items'
             * time is newer than the snapshot, we read in the middle of that material being updated
             * and we need to retry.
             *
             * Otherwise we have successfully gotten a matching set, as described above.
             *
             * If there is no history store checkpoint, its time will be zero, which will be
             * accepted.
             *
             * We skip the test entirely if we aren't trying to return a snapshot (and therefore not
             * history either) because there's nothing to check, and if we didn't retrieve the
             * snapshot its time will be 0 and the check will fail gratuitously and lead to retrying
             * forever.
             */
            else if (first_snapshot_time != snapshot_time || ds_time > snapshot_time ||
              hs_time > snapshot_time || stable_time > snapshot_time || oldest_time > snapshot_time)
                ret = __wt_set_return(session, EBUSY);
            else {
                /* Crosscheck that we didn't somehow get an older timestamp. */
                WT_ASSERT(session, stable_time == snapshot_time || stable_time == 0);
                WT_ASSERT(session, oldest_time == snapshot_time || oldest_time == 0);
            }

            /*
             * Return the snapshot's wall time as the (global) checkpoint ID. The ID is a 64-bit
             * value of unspecified semantics such that if you open the same checkpoint name and get
             * different IDs, the cursors you got are looking at different versions of that
             * checkpoint, which usually isn't what you want. Test code uses this to check whether a
             * collection of checkpoint cursors they opened on different files all came from the
             * same global checkpoint or not. This is the same problem as checking if the history
             * store checkpoint and data store checkpoint match, so the wall time is the right thing
             * to use for it.
             */
            ckpt_snapshot->ckpt_id = snapshot_time;
        }

        if (ret == 0) {
            /* Get a handle for the data store. */
            ret = __wt_session_get_dhandle(session, uri, checkpoint, cfg, flags);
            if (ret == 0 && session->dhandle->checkpoint_order != ds_order) {
                /* The tree we opened is newer than the one we expected; need to retry. */
                WT_TRET(__wt_session_release_dhandle(session));
                WT_TRET(__wt_set_return(session, EBUSY));
            }
        }

        if (ret == 0 && hs_checkpoint != NULL) {
            /* Get a handle for the history store. */
            WT_ASSERT(session, hs_dhandlep != NULL);
            WT_WITHOUT_DHANDLE(session,
              ret =
                __session_open_hs_ckpt(session, hs_checkpoint, cfg, flags, hs_order, hs_dhandlep));
            if (ret != 0)
                WT_TRET(__wt_session_release_dhandle(session));
        }

        /* Drop the names; we don't need them any more. Nulls the pointers; retry relies on that. */
        __wt_free(session, checkpoint);
        __wt_free(session, hs_checkpoint);

        /*
         * There's a potential race: we get the name of the most recent unnamed checkpoint, but if
         * it's discarded (or locked so it can be discarded) by the time we try to open it, we'll
         * fail the open. Retry in those cases; a new checkpoint version should surface, and we
         * can't return an error. The application will be justifiably upset if we can't open the
         * last checkpoint instance of an object.
         *
         * The WT_NOTFOUND condition will eventually clear; some unnamed checkpoint existed when we
         * looked up the name (otherwise we would have failed then) so a new one must be in
         * progress.
         *
         * At this point we should either have ret == 0 and the handles we were asked for, or ret !=
         * 0 and no handles.
         *
         * For named checkpoints, we don't retry, I guess because the application ought not to try
         * to open its checkpoints while regenerating them.
         */

    } while (is_unnamed_ckpt && (ret == WT_NOTFOUND || ret == EBUSY));

err:
    __wt_free(session, checkpoint);
    __wt_free(session, hs_checkpoint);
    return (ret);
}

/*
 * __wt_session_close_cache --
 *     Close any cached handles in a session.
 */
void
__wt_session_close_cache(WT_SESSION_IMPL *session)
{
    WT_DATA_HANDLE_CACHE *dhandle_cache, *dhandle_cache_tmp;

    WT_TAILQ_SAFE_REMOVE_BEGIN(dhandle_cache, &session->dhandles, q, dhandle_cache_tmp)
    {
        __session_discard_dhandle(session, dhandle_cache);
    }
    WT_TAILQ_SAFE_REMOVE_END
}

/*
 * __wt_session_dhandle_sweep --
 *     Discard any session dhandles that are not open.
 */
void
__wt_session_dhandle_sweep(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DATA_HANDLE *dhandle;
    WT_DATA_HANDLE_CACHE *dhandle_cache, *dhandle_cache_tmp;
    uint64_t now;

    conn = S2C(session);

    /*
     * Periodically sweep for dead handles; if we've swept recently, don't do it again.
     */
    __wt_seconds(session, &now);
    if (now - session->last_sweep < conn->sweep_interval)
        return;
    session->last_sweep = now;

    WT_STAT_CONN_INCR(session, dh_session_sweeps);

    TAILQ_FOREACH_SAFE(dhandle_cache, &session->dhandles, q, dhandle_cache_tmp)
    {
        dhandle = dhandle_cache->dhandle;

        /*
         * Only discard handles that are dead or dying and, in the case of btrees, have been
         * evicted. These checks are not done with any locks in place, other than the data handle
         * reference, so we cannot peer past what is in the dhandle directly.
         */
        if (dhandle != session->dhandle && dhandle->session_inuse == 0 &&
          (WT_DHANDLE_INACTIVE(dhandle) ||
            (dhandle->timeofdeath != 0 && now - dhandle->timeofdeath > conn->sweep_idle_time)) &&
          (!WT_DHANDLE_BTREE(dhandle) || F_ISSET(dhandle, WT_DHANDLE_EVICTED))) {
            WT_STAT_CONN_INCR(session, dh_session_handles);
            WT_ASSERT(session, !WT_IS_METADATA(dhandle));
            __session_discard_dhandle(session, dhandle_cache);
        }
    }
}

/*
 * __session_find_shared_dhandle --
 *     Search for a data handle in the connection and add it to a session's cache. We must increment
 *     the handle's reference count while holding the handle list lock.
 */
static int
__session_find_shared_dhandle(WT_SESSION_IMPL *session, const char *uri, const char *checkpoint)
{
    WT_DECL_RET;

    WT_WITH_HANDLE_LIST_READ_LOCK(session,
      if ((ret = __wt_conn_dhandle_find(session, uri, checkpoint)) == 0)
        WT_DHANDLE_ACQUIRE(session->dhandle));

    if (ret != WT_NOTFOUND)
        return (ret);

    WT_WITH_HANDLE_LIST_WRITE_LOCK(session,
      if ((ret = __wt_conn_dhandle_alloc(session, uri, checkpoint)) == 0)
        WT_DHANDLE_ACQUIRE(session->dhandle));

    return (ret);
}

/*
 * __session_get_dhandle --
 *     Search for a data handle, first in the session cache, then in the connection.
 */
static int
__session_get_dhandle(WT_SESSION_IMPL *session, const char *uri, const char *checkpoint)
{
    WT_DATA_HANDLE_CACHE *dhandle_cache;
    WT_DECL_RET;

    __session_find_dhandle(session, uri, checkpoint, &dhandle_cache);
    if (dhandle_cache != NULL) {
        session->dhandle = dhandle_cache->dhandle;
        return (0);
    }

    /* Sweep the handle list to remove any dead handles. */
    __wt_session_dhandle_sweep(session);

    /*
     * We didn't find a match in the session cache, search the shared handle list and cache the
     * handle we find.
     */
    WT_RET(__session_find_shared_dhandle(session, uri, checkpoint));

    /*
     * Fixup the reference count on failure (we incremented the reference count while holding the
     * handle-list lock).
     */
    if ((ret = __session_add_dhandle(session)) != 0) {
        WT_DHANDLE_RELEASE(session->dhandle);
        session->dhandle = NULL;
    }

    return (ret);
}

/*
 * __wt_session_dhandle_readlock --
 *     Acquire read lock for the session's current dhandle.
 */
void
__wt_session_dhandle_readlock(WT_SESSION_IMPL *session)
{
    WT_ASSERT(session, session->dhandle != NULL);
    __wt_readlock(session, &session->dhandle->rwlock);
}

/*
 * __wt_session_dhandle_readunlock --
 *     Release read lock for the session's current dhandle.
 */
void
__wt_session_dhandle_readunlock(WT_SESSION_IMPL *session)
{
    WT_ASSERT(session, session->dhandle != NULL);
    __wt_readunlock(session, &session->dhandle->rwlock);
}

/*
 * __wt_session_dhandle_writeunlock --
 *     Release write lock for the session's current dhandle.
 */
void
__wt_session_dhandle_writeunlock(WT_SESSION_IMPL *session)
{
    WT_ASSERT(session, session->dhandle != NULL);
    WT_ASSERT(session, FLD_ISSET(session->dhandle->lock_flags, WT_DHANDLE_LOCK_WRITE));
    FLD_CLR(session->dhandle->lock_flags, WT_DHANDLE_LOCK_WRITE);
    __wt_writeunlock(session, &session->dhandle->rwlock);
}

/*
 * __wt_session_dhandle_try_writelock --
 *     Try to acquire write lock for the session's current dhandle.
 */
int
__wt_session_dhandle_try_writelock(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;

    WT_ASSERT(session, session->dhandle != NULL);
    if ((ret = __wt_try_writelock(session, &session->dhandle->rwlock)) == 0)
        FLD_SET(session->dhandle->lock_flags, WT_DHANDLE_LOCK_WRITE);

    return (ret);
}

/*
 * __wt_session_get_dhandle --
 *     Get a data handle for the given name, set session->dhandle. Optionally if we opened a
 *     checkpoint return its checkpoint order number.
 */
int
__wt_session_get_dhandle(WT_SESSION_IMPL *session, const char *uri, const char *checkpoint,
  const char *cfg[], uint32_t flags)
{
    WT_DATA_HANDLE *dhandle;
    WT_DECL_RET;
    bool is_dead;

    WT_ASSERT(session, !F_ISSET(session, WT_SESSION_NO_DATA_HANDLES));

    for (;;) {
        WT_RET(__session_get_dhandle(session, uri, checkpoint));
        dhandle = session->dhandle;

        /* Try to lock the handle. */
        WT_RET(__wt_session_lock_dhandle(session, flags, &is_dead));
        if (is_dead)
            continue;

        /* If the handle is open in the mode we want, we're done. */
        if (LF_ISSET(WT_DHANDLE_LOCK_ONLY) ||
          (F_ISSET(dhandle, WT_DHANDLE_OPEN) && !LF_ISSET(WT_BTREE_SPECIAL_FLAGS)))
            break;

        WT_ASSERT(session, F_ISSET(dhandle, WT_DHANDLE_EXCLUSIVE));

        /*
         * For now, we need the schema lock and handle list locks to open a file for real.
         *
         * Code needing exclusive access (such as drop or verify) assumes that it can close all open
         * handles, then open an exclusive handle on the active tree and no other threads can reopen
         * handles in the meantime. A combination of the schema and handle list locks are used to
         * enforce this.
         */
        if (!FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_SCHEMA)) {
            dhandle->excl_session = NULL;
            dhandle->excl_ref = 0;
            F_CLR(dhandle, WT_DHANDLE_EXCLUSIVE);
            WT_WITH_DHANDLE(session, dhandle, __wt_session_dhandle_writeunlock(session));

            WT_WITH_SCHEMA_LOCK(
              session, ret = __wt_session_get_dhandle(session, uri, checkpoint, cfg, flags));

            return (ret);
        }

        /* Open the handle. */
        if ((ret = __wt_conn_dhandle_open(session, cfg, flags)) == 0 &&
          LF_ISSET(WT_DHANDLE_EXCLUSIVE))
            break;

        /*
         * If we got the handle exclusive to open it but only want ordinary access, drop our lock
         * and retry the open.
         */
        dhandle->excl_session = NULL;
        dhandle->excl_ref = 0;
        F_CLR(dhandle, WT_DHANDLE_EXCLUSIVE);
        WT_WITH_DHANDLE(session, dhandle, __wt_session_dhandle_writeunlock(session));
        WT_RET(ret);
    }

    WT_ASSERT(session, !F_ISSET(dhandle, WT_DHANDLE_DEAD));
    WT_ASSERT(session, LF_ISSET(WT_DHANDLE_LOCK_ONLY) || F_ISSET(dhandle, WT_DHANDLE_OPEN));

    WT_ASSERT(session,
      LF_ISSET(WT_DHANDLE_EXCLUSIVE) == F_ISSET(dhandle, WT_DHANDLE_EXCLUSIVE) ||
        dhandle->excl_ref > 1);

    return (0);
}

/*
 * __wt_session_lock_checkpoint --
 *     Lock the btree handle for the given checkpoint name.
 */
int
__wt_session_lock_checkpoint(WT_SESSION_IMPL *session, const char *checkpoint)
{
    WT_DATA_HANDLE *saved_dhandle;
    WT_DECL_RET;

    WT_ASSERT(session, WT_META_TRACKING(session));
    saved_dhandle = session->dhandle;

    /*
     * Get the checkpoint handle exclusive, so no one else can access it while we are creating the
     * new checkpoint. Hold the lock until the checkpoint completes.
     */
    WT_ERR(__wt_session_get_dhandle(
      session, saved_dhandle->name, checkpoint, NULL, WT_DHANDLE_EXCLUSIVE | WT_DHANDLE_LOCK_ONLY));
    if ((ret = __wt_meta_track_handle_lock(session, false)) != 0) {
        WT_TRET(__wt_session_release_dhandle(session));
        goto err;
    }

    /*
     * Get exclusive access to the handle and then flush any pages in this checkpoint from the cache
     * (we are about to re-write the checkpoint which will mean cached pages no longer have valid
     * contents). This is especially noticeable with memory mapped files, since changes to the
     * underlying file are visible to the in-memory pages.
     */
    WT_ERR(__wt_evict_file_exclusive_on(session));
    ret = __wt_evict_file(session, WT_SYNC_DISCARD);
    __wt_evict_file_exclusive_off(session);
    WT_ERR(ret);

    /*
     * We lock checkpoint handles that we are overwriting, so the handle must be closed when we
     * release it.
     */
    F_SET(session->dhandle, WT_DHANDLE_DISCARD);

/* Restore the original data handle in the session. */
err:
    session->dhandle = saved_dhandle;
    return (ret);
}
