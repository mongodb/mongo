/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static void __checkpoint_timing_stress(WT_SESSION_IMPL *, uint64_t, struct timespec *);
static int __checkpoint_lock_dirty_tree(WT_SESSION_IMPL *, bool, bool, bool, const char *[]);
static int __checkpoint_mark_skip(WT_SESSION_IMPL *, WT_CKPT *, bool);
static int __checkpoint_presync(WT_SESSION_IMPL *, const char *[]);
static int __checkpoint_tree_helper(WT_SESSION_IMPL *, const char *[]);
static int __drop_list_execute(WT_SESSION_IMPL *session, WT_ITEM *drop_list);

/*
 * __checkpoint_flush_tier_wait --
 *     Wait for all previous work units queued to be processed.
 */
static int
__checkpoint_flush_tier_wait(WT_SESSION_IMPL *session, const char **cfg)
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    uint64_t now, start, timeout;
    int yield_count;

    conn = S2C(session);
    yield_count = 0;
    now = start = 0;
    /*
     * The internal thread needs the schema lock to perform its operations and flush tier also
     * acquires the schema lock. We cannot be waiting in this function while holding that lock or no
     * work will get done.
     */
    WT_ASSERT(session, !FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_SCHEMA));
    WT_RET(__wt_config_gets(session, cfg, "flush_tier.timeout", &cval));
    timeout = (uint64_t)cval.val;
    if (timeout != 0)
        __wt_seconds(session, &start);

    /*
     * It may be worthwhile looking at the add and decrement values and make choices of whether to
     * yield or wait based on how much of the workload has been performed. Flushing operations could
     * take a long time so yielding may not be effective.
     */
    while (!WT_FLUSH_STATE_DONE(conn->flush_state)) {
        if (start != 0) {
            __wt_seconds(session, &now);
            if (now - start > timeout)
                return (EBUSY);
        }
        if (++yield_count < WT_THOUSAND)
            __wt_yield();
        else {
            __wt_cond_signal(session, conn->tiered_cond);
            __wt_cond_wait(session, conn->flush_cond, 200, NULL);
        }
    }
    return (0);
}

/*
 * __checkpoint_flush_tier --
 *     Perform one iteration of tiered storage maintenance.
 */
static int
__checkpoint_flush_tier(WT_SESSION_IMPL *session, bool force)
{
    WT_CKPT ckpt;
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    uint64_t ckpt_time;
    const char *key, *value;

    __wt_verbose(session, WT_VERB_TIERED, "CKPT_FLUSH_TIER: Called force %d", force);

    WT_STAT_CONN_INCR(session, flush_tier);
    conn = S2C(session);
    cursor = NULL;
    /*
     * For supporting splits and merge:
     * - See if there is any merging work to do to prepare and create an object that is
     *   suitable for placing onto tiered storage.
     * - Do the work to create said objects.
     * - Move the objects.
     */
    conn->flush_state = 0;

    WT_ASSERT(session, FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_CHECKPOINT));
    conn->flush_ckpt_complete = false;
    conn->flush_most_recent = conn->ckpt_most_recent;
    conn->flush_ts = conn->txn_global.last_ckpt_timestamp;

    /*
     * Walk the metadata cursor to find tiered tables to flush. This should be optimized to avoid
     * flushing tables that haven't changed.
     */
    WT_RET(__wt_metadata_cursor(session, &cursor));
    while (cursor->next(cursor) == 0) {
        cursor->get_key(cursor, &key);
        cursor->get_value(cursor, &value);
        /* For now just switch tiers which just does metadata manipulation. */
        if (WT_PREFIX_MATCH(key, "tiered:")) {
            __wt_verbose(
              session, WT_VERB_TIERED, "CKPT_FLUSH_TIER: %s %s force %d", key, value, force);
            if (!force) {
                /*
                 * Check the table's last checkpoint time and only flush trees that have a
                 * checkpoint more recent than the last flush time.
                 */
                WT_ERR(__wt_meta_checkpoint(session, key, NULL, &ckpt));
                ckpt_time = ckpt.sec;
                __wt_meta_checkpoint_free(session, &ckpt);
                WT_ERR(__wt_config_getones(session, value, "flush_time", &cval));

                /* If nothing has changed, there's nothing to do. */
                if (ckpt_time == 0 || (uint64_t)cval.val > ckpt_time) {
                    WT_STAT_CONN_INCR(session, flush_tier_skipped);
                    continue;
                }
            }
            /* Only instantiate the handle if we need to flush. */
            WT_ERR(__wt_session_get_dhandle(session, key, NULL, NULL, 0));
            /*
             * When we call wt_tiered_switch the session->dhandle points to the tiered: entry and
             * the arg is the config string that is currently in the metadata.
             */
            WT_ERR(__wt_tiered_switch(session, value));
            WT_STAT_CONN_INCR(session, flush_tier_switched);
            WT_ERR(__wt_session_release_dhandle(session));
        }
    }
    WT_ERR(__wt_metadata_cursor_release(session, &cursor));

    /* Clear the flag on success. */
    F_CLR(conn, WT_CONN_TIERED_FIRST_FLUSH);
    return (0);

err:
    WT_TRET(__wt_session_release_dhandle(session));
    WT_TRET(__wt_metadata_cursor_release(session, &cursor));
    WT_STAT_CONN_INCR(session, flush_tier_fail);
    return (ret);
}
/*
 * __checkpoint_name_ok --
 *     Complain if the checkpoint name isn't acceptable.
 */
static int
__checkpoint_name_ok(WT_SESSION_IMPL *session, const char *name, size_t len, bool allow_all)
{
    /* Check for characters we don't want to see in a metadata file. */
    WT_RET(__wt_name_check(session, name, len, true));

    /*
     * The internal checkpoint name is special, applications aren't allowed to use it. Be aggressive
     * and disallow any matching prefix, it makes things easier when checking in other places.
     */
    if (len >= strlen(WT_CHECKPOINT) && WT_PREFIX_MATCH(name, WT_CHECKPOINT))
        WT_RET_MSG(session, EINVAL, "the checkpoint name \"%s\" is reserved", WT_CHECKPOINT);

    /* The name "all" is also special. */
    if (!allow_all && WT_STRING_MATCH("all", name, len))
        WT_RET_MSG(session, EINVAL, "the checkpoint name \"all\" is reserved");

    return (0);
}

/*
 * __checkpoint_name_check --
 *     Check for an attempt to name a checkpoint that includes anything other than a file object.
 */
static int
__checkpoint_name_check(WT_SESSION_IMPL *session, const char *uri)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    const char *fail;

    cursor = NULL;
    fail = NULL;

    /*
     * This function exists as a place for this comment: named checkpoints are only supported on
     * file objects, and not on LSM trees. If a target list is configured for the checkpoint, this
     * function is called with each target list entry; check the entry to make sure it's backed by a
     * file. If no target list is configured, confirm the metadata file contains no non-file
     * objects. Skip any internal system objects. We don't want spurious error messages, other code
     * will skip over them and the user has no control over their existence.
     */
    if (uri == NULL) {
        WT_RET(__wt_metadata_cursor(session, &cursor));
        while ((ret = cursor->next(cursor)) == 0) {
            WT_ERR(cursor->get_key(cursor, &uri));
            if (!WT_PREFIX_MATCH(uri, "colgroup:") && !WT_PREFIX_MATCH(uri, "file:") &&
              !WT_PREFIX_MATCH(uri, "index:") && !WT_PREFIX_MATCH(uri, WT_SYSTEM_PREFIX) &&
              !WT_PREFIX_MATCH(uri, "table:") && !WT_PREFIX_MATCH(uri, "tiered:")) {
                fail = uri;
                break;
            }
        }
        WT_ERR_NOTFOUND_OK(ret, false);
    } else if (!WT_PREFIX_MATCH(uri, "colgroup:") && !WT_PREFIX_MATCH(uri, "file:") &&
      !WT_PREFIX_MATCH(uri, "index:") && !WT_PREFIX_MATCH(uri, "table:") &&
      !WT_PREFIX_MATCH(uri, "tiered:"))
        fail = uri;

    if (fail != NULL)
        WT_ERR_MSG(session, EINVAL, "%s object does not support named checkpoints", fail);

err:
    WT_TRET(__wt_metadata_cursor_release(session, &cursor));
    return (ret);
}

/*
 * __checkpoint_update_generation --
 *     Update the checkpoint generation of the current tree. This indicates that the tree will not
 *     be visited again by the current checkpoint.
 */
static void
__checkpoint_update_generation(WT_SESSION_IMPL *session)
{
    WT_BTREE *btree;

    btree = S2BT(session);

    /*
     * Updates to the metadata are made by the checkpoint transaction, so the metadata tree's
     * checkpoint generation should never be updated.
     */
    if (WT_IS_METADATA(session->dhandle))
        return;

    WT_PUBLISH(btree->checkpoint_gen, __wt_gen(session, WT_GEN_CHECKPOINT));
    WT_STAT_DATA_SET(session, btree_checkpoint_generation, btree->checkpoint_gen);
}

/*
 * __checkpoint_apply_operation --
 *     Apply a preliminary operation to all files involved in a checkpoint.
 */
static int
__checkpoint_apply_operation(
  WT_SESSION_IMPL *session, const char *cfg[], int (*op)(WT_SESSION_IMPL *, const char *[]))
{
    WT_CONFIG targetconf;
    WT_CONFIG_ITEM cval, k, v;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    bool ckpt_closed, named, target_list;

    target_list = false;

    /* Flag if this is a named checkpoint, and check if the name is OK. */
    WT_RET(__wt_config_gets(session, cfg, "name", &cval));
    named = cval.len != 0;
    if (named)
        WT_RET(__checkpoint_name_ok(session, cval.str, cval.len, false));

    /* Step through the targets and optionally operate on each one. */
    WT_ERR(__wt_config_gets(session, cfg, "target", &cval));
    __wt_config_subinit(session, &targetconf, &cval);
    while ((ret = __wt_config_next(&targetconf, &k, &v)) == 0) {
        if (!target_list) {
            WT_ERR(__wt_scr_alloc(session, 512, &tmp));
            target_list = true;
        }

        if (v.len != 0)
            WT_ERR_MSG(session, EINVAL, "invalid checkpoint target %.*s: URIs may require quoting",
              (int)cval.len, (char *)cval.str);

        /* Some objects don't support named checkpoints. */
        if (named)
            WT_ERR(__checkpoint_name_check(session, k.str));

        if (op == NULL)
            continue;
        WT_ERR(__wt_buf_fmt(session, tmp, "%.*s", (int)k.len, k.str));
        if ((ret = __wt_schema_worker(session, tmp->data, op, NULL, cfg, 0)) != 0)
            WT_ERR_MSG(session, ret, "%s", (const char *)tmp->data);
    }
    WT_ERR_NOTFOUND_OK(ret, false);

    if (!target_list && named)
        /* Some objects don't support named checkpoints. */
        WT_ERR(__checkpoint_name_check(session, NULL));

    if (!target_list && op != NULL) {
        /*
         * If the checkpoint is named or we're dropping checkpoints, we checkpoint both open and
         * closed files; else, only checkpoint open files.
         *
         * XXX We don't optimize unnamed checkpoints of a list of targets, we open the targets and
         * checkpoint them even if they are quiescent and don't need a checkpoint, believing
         * applications unlikely to checkpoint a list of closed targets.
         */
        ckpt_closed = named;
        if (!ckpt_closed) {
            WT_ERR(__wt_config_gets(session, cfg, "drop", &cval));
            ckpt_closed = cval.len != 0;
        }
        WT_ERR(ckpt_closed ? __wt_meta_apply_all(session, op, NULL, cfg) :
                             __wt_conn_btree_apply(session, NULL, op, NULL, cfg));
    }

err:
    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * __checkpoint_apply_to_dhandles --
 *     Apply an operation to all handles locked for a checkpoint.
 */
static int
__checkpoint_apply_to_dhandles(
  WT_SESSION_IMPL *session, const char *cfg[], int (*op)(WT_SESSION_IMPL *, const char *[]))
{
    WT_DECL_RET;
    u_int i;

    /* If we have already locked the handles, apply the operation. */
    for (i = 0; i < session->ckpt_handle_next; ++i) {
        if (session->ckpt_handle[i] == NULL)
            continue;
        WT_WITH_DHANDLE(session, session->ckpt_handle[i], ret = (*op)(session, cfg));
        WT_RET(ret);
    }

    return (0);
}

/*
 * __checkpoint_data_source --
 *     Checkpoint all data sources.
 */
static int
__checkpoint_data_source(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_DATA_SOURCE *dsrc;
    WT_NAMED_DATA_SOURCE *ndsrc;

    /*
     * A place-holder, to support data sources: we assume calling the underlying data-source session
     * checkpoint function is sufficient to checkpoint all objects in the data source, open or
     * closed, and we don't attempt to optimize the checkpoint of individual targets. Those
     * assumptions are not necessarily going to be true for all data sources.
     *
     * It's not difficult to support data-source checkpoints of individual targets
     * (__wt_schema_worker is the underlying function that will do the work, and it's already
     * written to support data-sources, although we'd probably need to pass the URI of the object to
     * the data source checkpoint function which we don't currently do). However, doing a full data
     * checkpoint is trickier: currently, the connection code is written to ignore all objects other
     * than "file:", and that code will require significant changes to work with data sources.
     */
    TAILQ_FOREACH (ndsrc, &S2C(session)->dsrcqh, q) {
        dsrc = ndsrc->dsrc;
        if (dsrc->checkpoint != NULL)
            WT_RET(dsrc->checkpoint(dsrc, (WT_SESSION *)session, (WT_CONFIG_ARG *)cfg));
    }
    return (0);
}

/*
 * __wt_checkpoint_get_handles --
 *     Get a list of handles to flush.
 */
int
__wt_checkpoint_get_handles(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_BTREE *btree;
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    const char *name;
    bool force;

    /* Find out if we have to force a checkpoint. */
    WT_RET(__wt_config_gets_def(session, cfg, "force", 0, &cval));
    force = cval.val != 0;
    if (!force) {
        WT_RET(__wt_config_gets_def(session, cfg, "name", 0, &cval));
        force = cval.len != 0;
    }

    /* Should not be called with anything other than a live btree handle. */
    WT_ASSERT(session, WT_DHANDLE_BTREE(session->dhandle) && !WT_READING_CHECKPOINT(session));

    btree = S2BT(session);

    /*
     * Skip files that are never involved in a checkpoint. Skip the history store file as it is,
     * checkpointed manually later.
     */
    if (F_ISSET(btree, WT_BTREE_NO_CHECKPOINT) || WT_IS_HS(btree->dhandle))
        return (0);

    /*
     * We may have raced between starting the checkpoint transaction and some operation completing
     * on the handle that updated the metadata (e.g., closing a bulk load cursor). All such
     * operations either have exclusive access to the handle or hold the schema lock. We are now
     * holding the schema lock and have an open btree handle, so if we can't update the metadata,
     * then there has been some state change invisible to the checkpoint transaction.
     */
    if (!WT_IS_METADATA(session->dhandle)) {
        WT_CURSOR *meta_cursor;

        WT_ASSERT(session, !F_ISSET(session->txn, WT_TXN_ERROR));
        WT_RET(__wt_metadata_cursor(session, &meta_cursor));
        meta_cursor->set_key(meta_cursor, session->dhandle->name);
        ret = __wt_curfile_insert_check(meta_cursor);
        if (ret == WT_ROLLBACK) {
            /*
             * If create or drop or any schema operation of a table is with in an user transaction
             * then checkpoint can see the dhandle before the commit, which will lead to the
             * rollback error. We will ignore this dhandle as part of this checkpoint by returning
             * from here.
             */
            __wt_verbose_notice(session, WT_VERB_CHECKPOINT, "%s",
              "WT_ROLLBACK: checkpoint raced with transaction operating on dhandle");
            WT_TRET(__wt_metadata_cursor_release(session, &meta_cursor));
            return (0);
        }
        WT_TRET(__wt_metadata_cursor_release(session, &meta_cursor));
        WT_RET(ret);
    }

    /*
     * Decide whether the tree needs to be included in the checkpoint and if so, acquire the
     * necessary locks.
     */
    WT_SAVE_DHANDLE(session, ret = __checkpoint_lock_dirty_tree(session, true, force, true, cfg));
    WT_RET(ret);
    if (F_ISSET(btree, WT_BTREE_SKIP_CKPT)) {
        __checkpoint_update_generation(session);
        return (0);
    }

    /*
     * Make sure there is space for the new entry: do this before getting the handle to avoid
     * cleanup if we can't allocate the memory.
     */
    WT_RET(__wt_realloc_def(session, &session->ckpt_handle_allocated, session->ckpt_handle_next + 1,
      &session->ckpt_handle));

    /*
     * The current tree will be included: get it again because the handle we have is only valid for
     * the duration of this function.
     */
    name = session->dhandle->name;
    session->dhandle = NULL;

    if ((ret = __wt_session_get_dhandle(session, name, NULL, NULL, 0)) != 0)
        return (ret == EBUSY ? 0 : ret);

    /*
     * Save the current eviction walk setting: checkpoint can interfere with eviction and we don't
     * want to unfairly penalize (or promote) eviction in trees due to checkpoints.
     */
    btree->evict_walk_saved = btree->evict_walk_period;

    session->ckpt_handle[session->ckpt_handle_next++] = session->dhandle;
    return (0);
}

/*
 * __checkpoint_wait_reduce_dirty_cache --
 *     Try to reduce the amount of dirty data in cache so there is less work do during the critical
 *     section of the checkpoint.
 */
static void
__checkpoint_wait_reduce_dirty_cache(WT_SESSION_IMPL *session)
{
    WT_CACHE *cache;
    WT_CONNECTION_IMPL *conn;
    double current_dirty, prev_dirty;
    uint64_t bytes_written_start, bytes_written_total;
    uint64_t cache_size, max_write;
    uint64_t time_start, time_stop;
    uint64_t total_ms;

    conn = S2C(session);
    cache = conn->cache;

    /* Give up if scrubbing is disabled. */
    if (cache->eviction_checkpoint_target < DBL_EPSILON)
        return;

    time_start = __wt_clock(session);
    bytes_written_start = cache->bytes_written;

    /*
     * If the cache size is zero or very small, we're done. The cache size can briefly become zero
     * if we're transitioning to a shared cache via reconfigure. This avoids potential divide by
     * zero.
     */
    if ((cache_size = conn->cache_size) < 10 * WT_MEGABYTE)
        return;

    current_dirty = (100.0 * __wt_cache_dirty_leaf_inuse(cache)) / cache_size;
    if (current_dirty <= cache->eviction_checkpoint_target)
        return;

    /* Stop if we write as much dirty data as is currently in cache. */
    max_write = __wt_cache_dirty_leaf_inuse(cache);

    /* Set the dirty trigger to the target value. */
    cache->eviction_scrub_target = cache->eviction_checkpoint_target;
    WT_STAT_CONN_SET(session, txn_checkpoint_scrub_target, 0);

    /* Wait while the dirty level is going down. */
    for (;;) {
        __wt_sleep(0, 100 * WT_THOUSAND);

        prev_dirty = current_dirty;
        current_dirty = (100.0 * __wt_cache_dirty_leaf_inuse(cache)) / cache_size;
        if (current_dirty <= cache->eviction_checkpoint_target || current_dirty >= prev_dirty)
            break;

        /*
         * We haven't reached the current target.
         *
         * Don't wait indefinitely: there might be dirty pages that can't be evicted. If we can't
         * meet the target, give up and start the checkpoint for real.
         */
        bytes_written_total = cache->bytes_written - bytes_written_start;
        if (bytes_written_total > max_write)
            break;
    }

    time_stop = __wt_clock(session);
    total_ms = WT_CLOCKDIFF_MS(time_stop, time_start);
    WT_STAT_CONN_SET(session, txn_checkpoint_scrub_time, total_ms);
}

/*
 * __wt_checkpoint_progress --
 *     Output a checkpoint progress message.
 */
void
__wt_checkpoint_progress(WT_SESSION_IMPL *session, bool closing)
{
    struct timespec cur_time;
    WT_CONNECTION_IMPL *conn;
    uint64_t time_diff;

    conn = S2C(session);
    __wt_epoch(session, &cur_time);

    /* Time since the full database checkpoint started */
    time_diff = WT_TIMEDIFF_SEC(cur_time, conn->ckpt_timer_start);

    if (closing || (time_diff / WT_PROGRESS_MSG_PERIOD) > conn->ckpt_progress_msg_count) {
        __wt_verbose(session, WT_VERB_CHECKPOINT_PROGRESS,
          "Checkpoint %s for %" PRIu64 " seconds and wrote: %" PRIu64 " pages (%" PRIu64 " MB)",
          closing ? "ran" : "has been running", time_diff, conn->ckpt_write_pages,
          conn->ckpt_write_bytes / WT_MEGABYTE);
        conn->ckpt_progress_msg_count++;
    }
}

/*
 * __checkpoint_stats --
 *     Update checkpoint timer stats.
 */
static void
__checkpoint_stats(WT_SESSION_IMPL *session)
{
    struct timespec stop;
    WT_CONNECTION_IMPL *conn;
    uint64_t msec;

    conn = S2C(session);

    /* Output a verbose progress message for long running checkpoints. */
    if (conn->ckpt_progress_msg_count > 0)
        __wt_checkpoint_progress(session, true);

    /* Compute end-to-end timer statistics for checkpoint. */
    __wt_epoch(session, &stop);
    msec = WT_TIMEDIFF_MS(stop, conn->ckpt_timer_scrub_end);

    if (msec > conn->ckpt_time_max)
        conn->ckpt_time_max = msec;
    if (msec < conn->ckpt_time_min)
        conn->ckpt_time_min = msec;
    conn->ckpt_time_recent = msec;
    conn->ckpt_time_total += msec;

    /* Compute timer statistics for the checkpoint prepare. */
    msec = WT_TIMEDIFF_MS(conn->ckpt_prep_end, conn->ckpt_prep_start);

    if (msec > conn->ckpt_prep_max)
        conn->ckpt_prep_max = msec;
    if (msec < conn->ckpt_prep_min)
        conn->ckpt_prep_min = msec;
    conn->ckpt_prep_recent = msec;
    conn->ckpt_prep_total += msec;
}

/*
 * __checkpoint_verbose_track --
 *     Output a verbose message with timing information
 */
static void
__checkpoint_verbose_track(WT_SESSION_IMPL *session, const char *msg)
{
    struct timespec stop;
    WT_CONNECTION_IMPL *conn;
    uint64_t msec;

    if (!WT_VERBOSE_ISSET(session, WT_VERB_CHECKPOINT))
        return;

    conn = S2C(session);
    __wt_epoch(session, &stop);

    /* Get time diff in milliseconds. */
    msec = WT_TIMEDIFF_MS(stop, conn->ckpt_timer_start);
    __wt_verbose(session, WT_VERB_CHECKPOINT,
      "time: %" PRIu64 " ms, gen: %" PRIu64 ": Full database checkpoint %s", msec,
      __wt_gen(session, WT_GEN_CHECKPOINT), msg);
}

/*
 * __checkpoint_fail_reset --
 *     Reset fields when a failure occurs.
 */
static void
__checkpoint_fail_reset(WT_SESSION_IMPL *session)
{
    WT_BTREE *btree;

    btree = S2BT(session);
    btree->modified = true;
    __wt_meta_ckptlist_free(session, &btree->ckpt);
}

/*
 * __checkpoint_prepare --
 *     Start the transaction for a checkpoint and gather handles.
 */
static int
__checkpoint_prepare(WT_SESSION_IMPL *session, bool *trackingp, const char *cfg[])
{
    struct timespec tsp;
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_TXN *txn;
    WT_TXN_GLOBAL *txn_global;
    WT_TXN_SHARED *txn_shared;
    uint64_t original_snap_min;
    const char *txn_cfg[] = {
      WT_CONFIG_BASE(session, WT_SESSION_begin_transaction), "isolation=snapshot", NULL};
    bool flush, flush_force, use_timestamp;

    conn = S2C(session);
    txn = session->txn;
    txn_global = &conn->txn_global;
    txn_shared = WT_SESSION_TXN_SHARED(session);

    WT_RET(__wt_config_gets(session, cfg, "use_timestamp", &cval));
    use_timestamp = (cval.val != 0);
    WT_RET(__wt_config_gets(session, cfg, "flush_tier.enabled", &cval));
    flush = cval.val;
    WT_RET(__wt_config_gets(session, cfg, "flush_tier.force", &cval));
    flush_force = cval.val;

    /*
     * Start a snapshot transaction for the checkpoint.
     *
     * Note: we don't go through the public API calls because they have side effects on cursors,
     * which applications can hold open across calls to checkpoint.
     */
    WT_STAT_CONN_SET(session, txn_checkpoint_prep_running, 1);
    __wt_epoch(session, &conn->ckpt_prep_start);

    WT_RET(__wt_txn_begin(session, txn_cfg));
    /* Wait 1000 microseconds to simulate slowdown in checkpoint prepare. */
    tsp.tv_sec = 0;
    tsp.tv_nsec = WT_MILLION;
    __checkpoint_timing_stress(session, WT_TIMING_STRESS_PREPARE_CHECKPOINT_DELAY, &tsp);
    original_snap_min = session->txn->snap_min;

    WT_DIAGNOSTIC_YIELD;

    /* Ensure a transaction ID is allocated prior to sharing it globally */
    WT_RET(__wt_txn_id_check(session));

    /* Keep track of handles acquired for locking. */
    WT_RET(__wt_meta_track_on(session));
    *trackingp = true;

    /*
     * Mark the connection as clean. If some data gets modified after generating checkpoint
     * transaction id, connection will be reset to dirty when reconciliation marks the btree dirty
     * on encountering the dirty page.
     */
    conn->modified = false;

    /*
     * Save the checkpoint session ID.
     *
     * We never do checkpoints in the default session (with id zero).
     */
    WT_ASSERT(session, session->id != 0 && txn_global->checkpoint_id == 0);
    txn_global->checkpoint_id = session->id;

    /*
     * Remove the checkpoint transaction from the global table.
     *
     * This allows ordinary visibility checks to move forward because checkpoints often take a long
     * time and only write to the metadata.
     */
    __wt_writelock(session, &txn_global->rwlock);
    txn_global->checkpoint_txn_shared = *txn_shared;
    txn_global->checkpoint_txn_shared.pinned_id = txn->snap_min;

    /*
     * Sanity check that the oldest ID hasn't moved on before we have cleared our entry.
     */
    WT_ASSERT(session,
      WT_TXNID_LE(txn_global->oldest_id, txn_shared->id) &&
        WT_TXNID_LE(txn_global->oldest_id, txn_shared->pinned_id));

    /*
     * Clear our entry from the global transaction session table. Any operation that needs to know
     * about the ID for this checkpoint will consider the checkpoint ID in the global structure.
     * Most operations can safely ignore the checkpoint ID (see the visible all check for details).
     */
    txn_shared->id = txn_shared->pinned_id = txn_shared->metadata_pinned = WT_TXN_NONE;

    /*
     * Set the checkpoint transaction's timestamp, if requested.
     *
     * We rely on having the global transaction data locked so the oldest timestamp can't move past
     * the stable timestamp.
     */
    WT_ASSERT(session,
      !F_ISSET(txn, WT_TXN_HAS_TS_COMMIT | WT_TXN_SHARED_TS_DURABLE | WT_TXN_SHARED_TS_READ));

    if (use_timestamp) {
        /*
         * If the user wants timestamps then set the metadata checkpoint timestamp based on whether
         * or not a stable timestamp is actually in use. Only set it when we're not running recovery
         * because recovery doesn't set the recovery timestamp until its checkpoint is complete.
         */
        if (txn_global->has_stable_timestamp) {
            txn_global->checkpoint_timestamp = txn_global->stable_timestamp;
            if (!F_ISSET(conn, WT_CONN_RECOVERING))
                txn_global->meta_ckpt_timestamp = txn_global->checkpoint_timestamp;
        } else if (!F_ISSET(conn, WT_CONN_RECOVERING))
            txn_global->meta_ckpt_timestamp = txn_global->recovery_timestamp;
    } else {
        if (!F_ISSET(conn, WT_CONN_RECOVERING))
            txn_global->meta_ckpt_timestamp = WT_TS_NONE;
        txn_shared->read_timestamp = WT_TS_NONE;
    }

    __wt_writeunlock(session, &txn_global->rwlock);

    /*
     * Refresh our snapshot here without publishing our shared ids to the world, doing so prevents
     * us from racing with the stable timestamp moving ahead of current snapshot. i.e. if the stable
     * timestamp moves after we begin the checkpoint transaction but before we set the checkpoint
     * timestamp we can end up missing updates in our checkpoint.
     */
    __wt_txn_bump_snapshot(session);

    /* Assert that our snapshot min didn't somehow move backwards. */
    WT_ASSERT(session, session->txn->snap_min >= original_snap_min);
    /* Flag as unused for non diagnostic builds. */
    WT_UNUSED(original_snap_min);

    if (use_timestamp)
        __wt_verbose_timestamp(
          session, txn_global->checkpoint_timestamp, "Checkpoint requested at stable timestamp");

    /*
     * If we are doing a flush_tier, do the metadata naming switch now while holding the schema lock
     * in this function.
     */
    if (flush)
        WT_RET(__checkpoint_flush_tier(session, flush_force));

    /*
     * Get a list of handles we want to sync; for named checkpoints this may pull closed objects
     * into the session cache.
     *
     * First, gather all handles, then start the checkpoint transaction, then release any clean
     * handles.
     */
    WT_ASSERT(session, session->ckpt_handle_next == 0);
    WT_WITH_TABLE_READ_LOCK(
      session, ret = __checkpoint_apply_operation(session, cfg, __wt_checkpoint_get_handles));

    __wt_epoch(session, &conn->ckpt_prep_end);
    WT_STAT_CONN_SET(session, txn_checkpoint_prep_running, 0);

    return (ret);
}

/*
 * __txn_checkpoint_can_skip --
 *     Determine whether it's safe to skip taking a checkpoint.
 */
static int
__txn_checkpoint_can_skip(
  WT_SESSION_IMPL *session, const char *cfg[], bool *fullp, bool *use_timestampp, bool *can_skipp)
{
    WT_CONFIG targetconf;
    WT_CONFIG_ITEM cval, k, v;
    WT_CONNECTION_IMPL *conn;
    WT_TXN_GLOBAL *txn_global;
    bool full, use_timestamp;

    /*
     * Default to not skipping - also initialize the other output parameters - even though they will
     * always be initialized unless there is an error and callers need to ignore the results on
     * error.
     */
    *can_skipp = *fullp = *use_timestampp = false;

    conn = S2C(session);
    txn_global = &conn->txn_global;

    /*
     * This function also parses out some configuration options and hands them back to the caller -
     * make sure it does that parsing regardless of the result.
     *
     * Determine if this is going to be a full checkpoint, that is a checkpoint that applies to all
     * data tables in a database.
     */
    WT_RET(__wt_config_gets(session, cfg, "target", &cval));
    __wt_config_subinit(session, &targetconf, &cval);
    *fullp = full = __wt_config_next(&targetconf, &k, &v) != 0;

    WT_RET(__wt_config_gets(session, cfg, "use_timestamp", &cval));
    *use_timestampp = use_timestamp = cval.val != 0;

    /* Never skip non-full checkpoints */
    if (!full)
        return (0);

    /* Never skip if force is configured. */
    WT_RET(__wt_config_gets_def(session, cfg, "force", 0, &cval));
    if (cval.val != 0)
        return (0);

    /* Never skip named checkpoints. */
    WT_RET(__wt_config_gets(session, cfg, "name", &cval));
    if (cval.len != 0)
        return (0);

    /*
     * If the checkpoint is using timestamps, and the stable timestamp hasn't been updated since the
     * last checkpoint there is nothing more that could be written. Except when a non timestamped
     * file has been modified, as such if the connection has been modified it is currently unsafe to
     * skip checkpoints.
     */
    if (!conn->modified && use_timestamp && txn_global->has_stable_timestamp &&
      txn_global->last_ckpt_timestamp != WT_TS_NONE &&
      txn_global->last_ckpt_timestamp == txn_global->stable_timestamp) {
        *can_skipp = true;
        return (0);
    }

    /*
     * Skip checkpointing the database if nothing has been dirtied since the last checkpoint. That
     * said there can be short instances when a btree gets marked dirty and the connection is yet to
     * be. We might skip a checkpoint in that short instance, which is okay because by the next time
     * we get to checkpoint, the connection would have been marked dirty and hence the checkpoint
     * will not be skipped again.
     *
     * If we are using timestamps then we shouldn't skip as the stable timestamp must have moved,
     * and as such we still need to run checkpoint to update the checkpoint timestamp and the
     * metadata.
     */
    if (!use_timestamp && !conn->modified)
        *can_skipp = true;

    return (0);
}

/*
 * __txn_checkpoint_establish_time --
 *     Get a time (wall time, not a timestamp) for this checkpoint. The time is left in the session.
 */
static void
__txn_checkpoint_establish_time(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    uint64_t ckpt_sec, most_recent;

    conn = S2C(session);

    /*
     * If tiered storage is in use, move the time up to at least the most recent flush first. NOTE:
     * reading the most recent flush time is not an ordered read (or repeated on retry) because
     * currently checkpoint and flush tier are mutually exclusive.
     *
     * Update the global value that tracks the most recent checkpoint, and use it to make sure the
     * most recent checkpoint time doesn't move backwards. Also make sure that this checkpoint time
     * is not the same as the previous one, by running the clock forwards as needed.
     *
     * Note that while it's possible to run the clock a good long way forward if one tries (e.g. by
     * doing a large number of schema operations that are fast and generate successive checkpoints
     * of the metadata) and some tests (e.g. f_ops) do, this is not expected to happen in real use
     * or lead to significant deviations from wall clock time. In a real database of any size full
     * checkpoints take more than one second and schema operations are rare. Furthermore, though
     * these times are saved on disk and displayed by 'wt list' they are not used operationally
     * except in restricted ways:
     *    - to manage the interaction between hot backups and checkpointing, where the absolute time
     *      does not matter;
     *    - to track when tiered storage was last flushed in order to avoid redoing work, where the
     *      absolute time does not matter;
     *    - to detect and retry races between opening checkpoint cursors and checkpoints in progress
     *      (which only cares about ordering and only since the last database open).
     *
     * Currently the checkpoint time can move backwards if something has run it forward and a crash
     * (or shutdown) and restart happens quickly enough that the wall clock hasn't caught up yet.
     * This is a property of the way it gets initialized at startup, which is naive, and if issues
     * arise where this matters it can get adjusted during startup in much the way the base write
     * generation does. The checkpoint cursor opening code was set up specifically so that this does
     * not matter.
     *
     * It is possible to race here, so use atomic CAS. This code relies on the fact that anyone we
     * race with will only increase (never decrease) the most recent checkpoint time value.
     *
     * We store the time in the session rather than passing it around explicitly because passing it
     * around explicitly runs afoul of the type signatures of the functions passed to schema_worker.
     */

    __wt_seconds(session, &ckpt_sec);
    ckpt_sec = WT_MAX(ckpt_sec, conn->flush_most_recent);

    for (;;) {
        WT_ORDERED_READ(most_recent, conn->ckpt_most_recent);
        if (ckpt_sec <= most_recent)
            ckpt_sec = most_recent + 1;
        if (__wt_atomic_cas64(&conn->ckpt_most_recent, most_recent, ckpt_sec))
            break;
    }

    WT_ASSERT(session, session->current_ckpt_sec == 0);
    session->current_ckpt_sec = ckpt_sec;
}

/*
 * __txn_checkpoint_clear_time --
 *     Clear the current checkpoint time in the session.
 */
static void
__txn_checkpoint_clear_time(WT_SESSION_IMPL *session)
{
    WT_ASSERT(session, session->current_ckpt_sec > 0);
    session->current_ckpt_sec = 0;
}

/*
 * __txn_checkpoint --
 *     Checkpoint a database or a list of objects in the database.
 */
static int
__txn_checkpoint(WT_SESSION_IMPL *session, const char *cfg[])
{
    struct timespec tsp;
    WT_CACHE *cache;
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_DATA_HANDLE *hs_dhandle;
    WT_DECL_RET;
    WT_TXN *txn;
    WT_TXN_GLOBAL *txn_global;
    WT_TXN_ISOLATION saved_isolation;
    wt_off_t hs_size;
    wt_timestamp_t ckpt_tmp_ts;
    size_t namelen;
    uint64_t fsync_duration_usecs, generation, hs_ckpt_duration_usecs;
    uint64_t time_start_fsync, time_start_hs, time_stop_fsync, time_stop_hs;
    u_int i;
    const char *name;
    bool can_skip, failed, full, idle, logging, tracking, use_timestamp;
    void *saved_meta_next;

    conn = S2C(session);
    cache = conn->cache;
    hs_size = 0;
    hs_dhandle = NULL;
    txn = session->txn;
    txn_global = &conn->txn_global;
    saved_isolation = session->isolation;
    full = idle = tracking = use_timestamp = false;

    /* Avoid doing work if possible. */
    WT_RET(__txn_checkpoint_can_skip(session, cfg, &full, &use_timestamp, &can_skip));
    if (can_skip) {
        WT_STAT_CONN_INCR(session, txn_checkpoint_skipped);
        return (0);
    }

    /* Check if this is a named checkpoint. */
    WT_RET(__wt_config_gets(session, cfg, "name", &cval));
    if (cval.len != 0) {
        name = cval.str;
        namelen = cval.len;
    } else {
        name = NULL;
        namelen = 0;
    }

    /*
     * Do a pass over the configuration arguments and figure out what kind of checkpoint this is.
     */
    WT_RET(__checkpoint_apply_operation(session, cfg, NULL));

    logging = FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED);

    /* Reset the statistics tracked per checkpoint. */
    cache->evict_max_page_size = 0;
    cache->evict_max_seconds = 0;
    conn->rec_maximum_hs_wrapup_seconds = 0;
    conn->rec_maximum_image_build_seconds = 0;
    conn->rec_maximum_seconds = 0;

    /* Initialize the verbose tracking timer */
    __wt_epoch(session, &conn->ckpt_timer_start);

    /* Initialize the checkpoint progress tracking data */
    conn->ckpt_progress_msg_count = 0;
    conn->ckpt_write_bytes = 0;
    conn->ckpt_write_pages = 0;

    /*
     * Get a time (wall time, not a timestamp) for this checkpoint. This will be applied to all the
     * trees so they match. The time is left in the session.
     */
    __txn_checkpoint_establish_time(session);

    /*
     * Update the global oldest ID so we do all possible cleanup.
     *
     * This is particularly important for compact, so that all dirty pages can be fully written.
     */
    WT_ERR(__wt_txn_update_oldest(session, WT_TXN_OLDEST_STRICT | WT_TXN_OLDEST_WAIT));

    /* Flush data-sources before we start the checkpoint. */
    WT_ERR(__checkpoint_data_source(session, cfg));

    /*
     * Try to reduce the amount of dirty data in cache so there is less work do during the critical
     * section of the checkpoint.
     */
    __checkpoint_wait_reduce_dirty_cache(session);

    /* Tell logging that we are about to start a database checkpoint. */
    if (full && logging)
        WT_ERR(__wt_txn_checkpoint_log(session, full, WT_TXN_LOG_CKPT_PREPARE, NULL));

    __checkpoint_verbose_track(session, "starting transaction");

    if (full)
        __wt_epoch(session, &conn->ckpt_timer_scrub_end);

    /*
     * Start the checkpoint for real.
     *
     * Bump the global checkpoint generation, used to figure out whether checkpoint has visited a
     * tree. Use an atomic increment even though we are single-threaded because readers of the
     * checkpoint generation don't hold the checkpoint lock.
     *
     * We do need to update it before clearing the checkpoint's entry out of the transaction table,
     * or a thread evicting in a tree could ignore the checkpoint's transaction.
     */
    __wt_gen_next(session, WT_GEN_CHECKPOINT, &generation);
    WT_STAT_CONN_SET(session, txn_checkpoint_generation, generation);

    /*
     * We want to skip checkpointing clean handles whenever possible. That is, when the checkpoint
     * is not named or forced. However, we need to take care about ordering with respect to the
     * checkpoint transaction.
     *
     * We can't skip clean handles before starting the transaction or the checkpoint can miss
     * updates in trees that become dirty as the checkpoint is starting. If we wait until the
     * transaction has started before locking a handle, there could be a metadata-changing operation
     * in between (e.g., salvage) that will cause a write conflict when the checkpoint goes to write
     * the metadata.
     *
     * Hold the schema lock while starting the transaction and gathering handles so the set we get
     * is complete and correct.
     */
    WT_WITH_SCHEMA_LOCK(session, ret = __checkpoint_prepare(session, &tracking, cfg));
    WT_ERR(ret);

    /*
     * Save the checkpoint timestamp in a temporary variable, when we release our snapshot it'll be
     * reset to zero.
     */
    WT_ORDERED_READ(ckpt_tmp_ts, txn_global->checkpoint_timestamp);

    WT_ASSERT(session, txn->isolation == WT_ISO_SNAPSHOT);

    /*
     * Unblock updates -- we can figure out that any updates to clean pages after this point are too
     * new to be written in the checkpoint.
     */
    cache->eviction_scrub_target = 0.0;
    WT_STAT_CONN_SET(session, txn_checkpoint_scrub_target, 0);

    /* Tell logging that we have started a database checkpoint. */
    if (full && logging)
        WT_ERR(__wt_txn_checkpoint_log(session, full, WT_TXN_LOG_CKPT_START, NULL));

    /* Add a ten second wait to simulate checkpoint slowness. */
    tsp.tv_sec = 10;
    tsp.tv_nsec = 0;
    __checkpoint_timing_stress(session, WT_TIMING_STRESS_CHECKPOINT_SLOW, &tsp);
    WT_ERR(__checkpoint_apply_to_dhandles(session, cfg, __checkpoint_tree_helper));

    /* Wait prior to checkpointing the history store to simulate checkpoint slowness. */
    __checkpoint_timing_stress(session, WT_TIMING_STRESS_HS_CHECKPOINT_DELAY, &tsp);

    /*
     * Get a history store dhandle. If the history store file is opened for a special operation this
     * will return EBUSY which we treat as an error. In scenarios where the history store is not
     * part of the metadata file (performing recovery on backup folder where no checkpoint
     * occurred), this will return ENOENT which we ignore and continue.
     */
    WT_ERR_ERROR_OK(__wt_session_get_dhandle(session, WT_HS_URI, NULL, NULL, 0), ENOENT, false);
    hs_dhandle = session->dhandle;

    /*
     * It is possible that we don't have a history store file in certain recovery scenarios. As such
     * we could get a dhandle that is not opened.
     */
    if (F_ISSET(hs_dhandle, WT_DHANDLE_OPEN)) {
        time_start_hs = __wt_clock(session);
        conn->txn_global.checkpoint_running_hs = true;
        WT_STAT_CONN_SET(session, txn_checkpoint_running_hs, 1);

        WT_WITH_DHANDLE(session, hs_dhandle, ret = __wt_checkpoint(session, cfg));

        WT_STAT_CONN_SET(session, txn_checkpoint_running_hs, 0);
        conn->txn_global.checkpoint_running_hs = false;
        WT_ERR(ret);

        /*
         * Once the history store checkpoint is complete, we increment the checkpoint generation of
         * the associated b-tree. The checkpoint generation controls whether we include the
         * checkpoint transaction in our calculations of the pinned and oldest_ids for a given
         * btree. We increment it here to ensure that the visibility checks performed on updates in
         * the history store do not include the checkpoint transaction.
         */
        __checkpoint_update_generation(session);

        time_stop_hs = __wt_clock(session);
        hs_ckpt_duration_usecs = WT_CLOCKDIFF_US(time_stop_hs, time_start_hs);
        WT_STAT_CONN_SET(session, txn_hs_ckpt_duration, hs_ckpt_duration_usecs);
    }

    /*
     * As part of recovery, rollback to stable may have left out clearing stale transaction ids.
     * Update the connection base write generation based on the latest checkpoint write generations
     * to reset these transaction ids present on the pages when reading them.
     */
    if (F_ISSET(conn, WT_CONN_RECOVERING))
        WT_ERR(__wt_metadata_correct_base_write_gen(session));

    /*
     * Clear the dhandle so the visibility check doesn't get confused about the snap min. Don't
     * bother restoring the handle since it doesn't make sense to carry a handle across a
     * checkpoint.
     */
    session->dhandle = NULL;

    /*
     * We have to update the system information before we release the snapshot. Drop the system
     * information for checkpoints we're dropping first in case the names overlap.
     */
    if (session->ckpt_drop_list != NULL) {
        __drop_list_execute(session, session->ckpt_drop_list);
        __wt_scr_free(session, &session->ckpt_drop_list);
    }
    if (full || name != NULL)
        WT_ERR(__wt_meta_sysinfo_set(session, full, name, namelen));

    /* Release the snapshot so we aren't pinning updates in cache. */
    __wt_txn_release_snapshot(session);

    /* Mark all trees as open for business (particularly eviction). */
    WT_ERR(__checkpoint_apply_to_dhandles(session, cfg, __checkpoint_presync));

    __checkpoint_verbose_track(session, "committing transaction");

    /*
     * Checkpoints have to hit disk (it would be reasonable to configure for lazy checkpoints, but
     * we don't support them yet).
     */
    time_start_fsync = __wt_clock(session);

    WT_ERR(__checkpoint_apply_to_dhandles(session, cfg, __wt_checkpoint_sync));

    /* Sync the history store file. */
    if (F_ISSET(hs_dhandle, WT_DHANDLE_OPEN))
        WT_WITH_DHANDLE(session, hs_dhandle, ret = __wt_checkpoint_sync(session, NULL));

    time_stop_fsync = __wt_clock(session);
    fsync_duration_usecs = WT_CLOCKDIFF_US(time_stop_fsync, time_start_fsync);
    WT_STAT_CONN_INCR(session, txn_checkpoint_fsync_post);
    WT_STAT_CONN_SET(session, txn_checkpoint_fsync_post_duration, fsync_duration_usecs);

    __checkpoint_verbose_track(session, "sync completed");

    /* If the history store file exists on disk, update its statistic. */
    if (F_ISSET(hs_dhandle, WT_DHANDLE_OPEN)) {
        WT_ERR(__wt_block_manager_named_size(session, WT_HS_FILE, &hs_size));
        WT_STAT_CONN_SET(session, cache_hs_ondisk, hs_size);
    }

    /*
     * Commit the transaction now that we are sure that all files in the checkpoint have been
     * flushed to disk. It's OK to commit before checkpointing the metadata since we know that all
     * files in the checkpoint are now in a consistent state.
     */
    WT_ERR(__wt_txn_commit(session, NULL));

    /*
     * Flush all the logs that are generated during the checkpoint. It is possible that checkpoint
     * may include the changes that are written in parallel by an eviction. To have a consistent
     * view of the data, make sure that all the logs are flushed to disk before the checkpoint is
     * complete.
     */
    if (FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED))
        WT_ERR(__wt_log_flush(session, WT_LOG_FSYNC));

    /*
     * Ensure that the metadata changes are durable before the checkpoint is resolved. Do this by
     * either checkpointing the metadata or syncing the log file. Recovery relies on the checkpoint
     * LSN in the metadata only being updated by full checkpoints so only checkpoint the metadata
     * for full or non-logged checkpoints.
     *
     * This is very similar to __wt_meta_track_off, ideally they would be merged.
     */
    if (full || !logging) {
        session->isolation = txn->isolation = WT_ISO_READ_UNCOMMITTED;
        /* Disable metadata tracking during the metadata checkpoint. */
        saved_meta_next = session->meta_track_next;
        session->meta_track_next = NULL;
        WT_WITH_DHANDLE(session, WT_SESSION_META_DHANDLE(session),
          WT_WITH_METADATA_LOCK(session, ret = __wt_checkpoint(session, cfg)));
        session->meta_track_next = saved_meta_next;
        WT_ERR(ret);

        WT_WITH_DHANDLE(
          session, WT_SESSION_META_DHANDLE(session), ret = __wt_checkpoint_sync(session, NULL));
        WT_ERR(ret);

        __checkpoint_verbose_track(session, "metadata sync completed");
    } else
        WT_WITH_DHANDLE(session, WT_SESSION_META_DHANDLE(session),
          ret = __wt_txn_checkpoint_log(session, false, WT_TXN_LOG_CKPT_SYNC, NULL));

    WT_STAT_CONN_SET(session, txn_checkpoint_stop_stress_active, 1);
    /* Wait prior to flush the checkpoint stop log record. */
    __checkpoint_timing_stress(session, WT_TIMING_STRESS_CHECKPOINT_STOP, &tsp);
    WT_STAT_CONN_SET(session, txn_checkpoint_stop_stress_active, 0);

    /*
     * Now that the metadata is stable, re-open the metadata file for regular eviction by clearing
     * the checkpoint_pinned flag.
     */
    txn_global->checkpoint_txn_shared.pinned_id = WT_TXN_NONE;

    if (full) {
        __checkpoint_stats(session);

        /*
         * If timestamps defined the checkpoint's content, set the saved last checkpoint timestamp,
         * otherwise clear it. We clear it for a couple of reasons: applications can query it and we
         * don't want to lie, and we use it to decide if WT_CONNECTION.rollback_to_stable is an
         * allowed operation. For the same reason, don't set it to WT_TS_NONE when the checkpoint
         * timestamp is WT_TS_NONE, set it to 1 so we can tell the difference.
         */
        if (use_timestamp) {
            conn->txn_global.last_ckpt_timestamp = ckpt_tmp_ts;
            /*
             * MongoDB assumes the checkpoint timestamp will be initialized with WT_TS_NONE. In such
             * cases it queries the recovery timestamp to determine the last stable recovery
             * timestamp. So, if the recovery timestamp is valid, set the last checkpoint timestamp
             * to recovery timestamp. This should never be a problem, as checkpoint timestamp should
             * never be less than recovery timestamp. This could potentially avoid MongoDB making
             * two calls to determine last stable recovery timestamp.
             */
            if (conn->txn_global.last_ckpt_timestamp == WT_TS_NONE)
                conn->txn_global.last_ckpt_timestamp = conn->txn_global.recovery_timestamp;
        } else
            conn->txn_global.last_ckpt_timestamp = WT_TS_NONE;
    }

err:
    /*
     * Reset the timer so that next checkpoint tracks the progress only if configured.
     */
    conn->ckpt_timer_start.tv_sec = 0;

    /*
     * XXX Rolling back the changes here is problematic.
     *
     * If we unroll here, we need a way to roll back changes to the avail list for each tree that
     * was successfully synced before the error occurred. Otherwise, the next time we try this
     * operation, we will try to free an old checkpoint again.
     *
     * OTOH, if we commit the changes after a failure, we have partially overwritten the checkpoint,
     * so what ends up on disk is not consistent.
     */
    failed = ret != 0;
    if (failed)
        conn->modified = true;

    session->isolation = txn->isolation = WT_ISO_READ_UNCOMMITTED;
    if (tracking)
        WT_TRET(__wt_meta_track_off(session, false, failed));

    cache->eviction_scrub_target = 0.0;
    WT_STAT_CONN_SET(session, txn_checkpoint_scrub_target, 0);

    if (F_ISSET(txn, WT_TXN_RUNNING)) {
        /*
         * Clear the dhandle so the visibility check doesn't get confused about the snap min. Don't
         * bother restoring the handle since it doesn't make sense to carry a handle across a
         * checkpoint.
         */
        session->dhandle = NULL;
        WT_TRET(__wt_txn_rollback(session, NULL));
    }

    /*
     * Tell logging that we have finished a database checkpoint. Do not write a log record if the
     * database was idle.
     */
    if (full && logging) {
        if (ret == 0 && F_ISSET(CUR2BT(session->meta_cursor), WT_BTREE_SKIP_CKPT))
            idle = true;
        WT_TRET(__wt_txn_checkpoint_log(session, full,
          (ret == 0 && !idle) ? WT_TXN_LOG_CKPT_STOP : WT_TXN_LOG_CKPT_CLEANUP, NULL));
    }

    for (i = 0; i < session->ckpt_handle_next; ++i) {
        if (session->ckpt_handle[i] == NULL)
            continue;
        /*
         * If the operation failed, mark all trees dirty so they are included if a future checkpoint
         * can succeed.
         */
        if (failed)
            WT_WITH_DHANDLE(session, session->ckpt_handle[i], __checkpoint_fail_reset(session));
        WT_WITH_DHANDLE(
          session, session->ckpt_handle[i], WT_TRET(__wt_session_release_dhandle(session)));
    }

    if (session->ckpt_drop_list != NULL)
        __wt_scr_free(session, &session->ckpt_drop_list);

    __txn_checkpoint_clear_time(session);

    __wt_free(session, session->ckpt_handle);
    session->ckpt_handle_allocated = session->ckpt_handle_next = 0;

    session->isolation = txn->isolation = saved_isolation;
    return (ret);
}

/*
 * __txn_checkpoint_wrapper --
 *     Checkpoint wrapper.
 */
static int
__txn_checkpoint_wrapper(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_TXN_GLOBAL *txn_global;

    conn = S2C(session);
    txn_global = &conn->txn_global;

    WT_STAT_CONN_SET(session, txn_checkpoint_running, 1);
    txn_global->checkpoint_running = true;

    ret = __txn_checkpoint(session, cfg);

    WT_STAT_CONN_SET(session, txn_checkpoint_running, 0);
    txn_global->checkpoint_running = false;

    /*
     * Signal the tiered storage thread because it waits for the checkpoint to complete to process
     * flush units. Indicate that the checkpoint has completed.
     */
    if (conn->tiered_cond != NULL) {
        conn->flush_ckpt_complete = true;
        __wt_cond_signal(session, conn->tiered_cond);
    }

    return (ret);
}

/*
 * __wt_txn_checkpoint --
 *     Checkpoint a database or a list of objects in the database.
 */
int
__wt_txn_checkpoint(WT_SESSION_IMPL *session, const char *cfg[], bool waiting)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    uint32_t orig_flags;
    bool flush, flush_sync;

    /*
     * Reset open cursors. Do this explicitly, even though it will happen implicitly in the call to
     * begin_transaction for the checkpoint, the checkpoint code will acquire the schema lock before
     * we do that, and some implementation of WT_CURSOR::reset might need the schema lock.
     */
    WT_RET(__wt_session_reset_cursors(session, false));

    /* Ensure the metadata table is open before taking any locks. */
    WT_RET(__wt_metadata_cursor(session, NULL));

/*
 * Don't highjack the session checkpoint thread for eviction.
 *
 * Application threads are not generally available for potentially slow operations, but checkpoint
 * does enough I/O it may be called upon to perform slow operations for the block manager.
 *
 * Application checkpoints wait until the checkpoint lock is available, compaction checkpoints
 * don't.
 *
 * Checkpoints should always use a separate session for history store updates, otherwise those
 * updates are pinned until the checkpoint commits. Also, there are unfortunate interactions between
 * the special rules for history store eviction and the special handling of the checkpoint
 * transaction.
 */
#undef WT_CHECKPOINT_SESSION_FLAGS
#define WT_CHECKPOINT_SESSION_FLAGS (WT_SESSION_CAN_WAIT | WT_SESSION_IGNORE_CACHE_SIZE)
    orig_flags = F_MASK(session, WT_CHECKPOINT_SESSION_FLAGS);
    F_SET(session, WT_CHECKPOINT_SESSION_FLAGS);

    /*
     * If this checkpoint includes a flush_tier then this call also must wait for any earlier
     * flush_tier to have completed all of its copying of objects. This happens if the user chose to
     * not wait for sync on the previous call.
     */
    WT_RET(__wt_config_gets(session, cfg, "flush_tier.enabled", &cval));
    flush = cval.val;
    WT_RET(__wt_config_gets(session, cfg, "flush_tier.sync", &cval));
    flush_sync = cval.val;
    if (flush)
        WT_ERR(__checkpoint_flush_tier_wait(session, cfg));

    /*
     * Only one checkpoint can be active at a time, and checkpoints must run in the same order as
     * they update the metadata. It's probably a bad idea to run checkpoints out of multiple
     * threads, but as compaction calls checkpoint directly, it can be tough to avoid. Serialize
     * here to ensure we don't get into trouble.
     */
    if (waiting)
        WT_WITH_CHECKPOINT_LOCK(session, ret = __txn_checkpoint_wrapper(session, cfg));
    else
        WT_WITH_CHECKPOINT_LOCK_NOWAIT(session, ret, ret = __txn_checkpoint_wrapper(session, cfg));
    WT_ERR(ret);
    if (flush && flush_sync)
        WT_ERR(__checkpoint_flush_tier_wait(session, cfg));
err:
    F_CLR(session, WT_CHECKPOINT_SESSION_FLAGS);
    F_SET(session, orig_flags);

    return (ret);
}

/*
 * __drop_list_execute --
 *     Clear the system info (snapshot and timestamp info) for the named checkpoints on the drop
 *     list.
 */
static int
__drop_list_execute(WT_SESSION_IMPL *session, WT_ITEM *drop_list)
{
    WT_CONFIG dropconf;
    WT_CONFIG_ITEM k, v;
    WT_DECL_RET;

    /* The list has the form (name, name, ...,) so we can read it with the config parser. */
    __wt_config_init(session, &dropconf, drop_list->data);
    while ((ret = __wt_config_next(&dropconf, &k, &v)) == 0) {
        WT_RET(__wt_meta_sysinfo_clear(session, k.str, k.len));
    }
    WT_RET_NOTFOUND_OK(ret);

    return (0);
}

/*
 * __drop_list_add --
 *     Add a checkpoint name to the list of (named) checkpoints being dropped. The list is produced
 *     by the first tree in the checkpoint (it must be the same in every tree, so it only needs to
 *     be produced once) and used at the top level to drop the snapshot and timestamp metadata for
 *     those checkpoints. Note that while there are several places in this file where WT_CKPT_DELETE
 *     is cleared on the fly, meaning the checkpoint won't actually be dropped, none of these apply
 *     to named checkpoints.
 */
static int
__drop_list_add(WT_SESSION_IMPL *session, WT_ITEM *drop_list, const char *name)
{
    return (__wt_buf_catfmt(session, drop_list, "%s,", name));
}

/*
 * __drop --
 *     Drop all checkpoints with a specific name.
 */
static int
__drop(
  WT_SESSION_IMPL *session, WT_ITEM *drop_list, WT_CKPT *ckptbase, const char *name, size_t len)
{
    WT_CKPT *ckpt;

    /*
     * If we're dropping internal checkpoints, match to the '.' separating the checkpoint name from
     * the generational number, and take all that we can find. Applications aren't allowed to use
     * any variant of this name, so the test is still pretty simple, if the leading bytes match,
     * it's one we want to drop.
     */
    if (strncmp(WT_CHECKPOINT, name, len) == 0) {
        WT_CKPT_FOREACH (ckptbase, ckpt)
            if (WT_PREFIX_MATCH(ckpt->name, WT_CHECKPOINT))
                F_SET(ckpt, WT_CKPT_DELETE);
    } else
        WT_CKPT_FOREACH (ckptbase, ckpt)
            if (WT_STRING_MATCH(ckpt->name, name, len)) {
                /* Remember the names of named checkpoints we're dropping. */
                if (drop_list != NULL)
                    WT_RET(__drop_list_add(session, drop_list, ckpt->name));
                F_SET(ckpt, WT_CKPT_DELETE);
            }

    return (0);
}

/*
 * __drop_from --
 *     Drop all checkpoints after, and including, the named checkpoint.
 */
static int
__drop_from(
  WT_SESSION_IMPL *session, WT_ITEM *drop_list, WT_CKPT *ckptbase, const char *name, size_t len)
{
    WT_CKPT *ckpt;
    bool matched;

    /*
     * There's a special case -- if the name is "all", then we delete all of the checkpoints.
     */
    if (WT_STRING_MATCH("all", name, len)) {
        WT_CKPT_FOREACH (ckptbase, ckpt) {
            /* Remember the names of named checkpoints we're dropping. */
            if (drop_list != NULL && !WT_PREFIX_MATCH(ckpt->name, WT_CHECKPOINT))
                WT_RET(__drop_list_add(session, drop_list, ckpt->name));
            F_SET(ckpt, WT_CKPT_DELETE);
        }
        return (0);
    }

    /*
     * We use the first checkpoint we can find, that is, if there are two checkpoints with the same
     * name in the list, we'll delete from the first match to the end.
     */
    matched = false;
    WT_CKPT_FOREACH (ckptbase, ckpt) {
        if (!matched && !WT_STRING_MATCH(ckpt->name, name, len))
            continue;

        matched = true;
        /* Remember the names of named checkpoints we're dropping. */
        if (drop_list != NULL && !WT_PREFIX_MATCH(ckpt->name, WT_CHECKPOINT))
            WT_RET(__drop_list_add(session, drop_list, ckpt->name));
        F_SET(ckpt, WT_CKPT_DELETE);
    }

    return (0);
}

/*
 * __drop_to --
 *     Drop all checkpoints before, and including, the named checkpoint.
 */
static int
__drop_to(
  WT_SESSION_IMPL *session, WT_ITEM *drop_list, WT_CKPT *ckptbase, const char *name, size_t len)
{
    WT_CKPT *ckpt, *mark;

    /*
     * We use the last checkpoint we can find, that is, if there are two checkpoints with the same
     * name in the list, we'll delete from the beginning to the second match, not the first.
     */
    mark = NULL;
    WT_CKPT_FOREACH (ckptbase, ckpt)
        if (WT_STRING_MATCH(ckpt->name, name, len))
            mark = ckpt;

    if (mark == NULL)
        return (0);

    WT_CKPT_FOREACH (ckptbase, ckpt) {
        /* Remember the names of named checkpoints we're dropping. */
        if (drop_list != NULL && !WT_PREFIX_MATCH(ckpt->name, WT_CHECKPOINT))
            WT_RET(__drop_list_add(session, drop_list, ckpt->name));
        F_SET(ckpt, WT_CKPT_DELETE);

        if (ckpt == mark)
            break;
    }

    return (0);
}

/*
 * __checkpoint_lock_dirty_tree_int --
 *     Helper for __checkpoint_lock_dirty_tree. Intended to be called while holding the hot backup
 *     lock.
 */
static int
__checkpoint_lock_dirty_tree_int(WT_SESSION_IMPL *session, bool is_checkpoint, bool force,
  WT_BTREE *btree, WT_CKPT *ckpt, WT_CKPT *ckptbase)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    u_int max_ckpt_drop;
    bool is_wt_ckpt;

    WT_UNUSED(is_checkpoint);
    conn = S2C(session);

    /* Check that it is OK to remove all the checkpoints marked for deletion. */
    max_ckpt_drop = 0;
    WT_CKPT_FOREACH (ckptbase, ckpt) {
        if (!F_ISSET(ckpt, WT_CKPT_DELETE))
            continue;
        is_wt_ckpt = WT_PREFIX_MATCH(ckpt->name, WT_CHECKPOINT);

        /*
         * If we are restarting from a backup and we're in recovery do not delete any checkpoints.
         * In the event of a crash we may need to restart from the backup and all checkpoints that
         * were in the backup file must remain.
         */
        if (F_ISSET(conn, WT_CONN_RECOVERING) && F_ISSET(conn, WT_CONN_WAS_BACKUP)) {
            F_CLR(ckpt, WT_CKPT_DELETE);
            continue;
        }
        /*
         * If there is a hot backup, don't delete any WiredTiger checkpoint that could possibly have
         * been created before the backup started. Fail if trying to delete any other named
         * checkpoint.
         */
        if (conn->hot_backup_start != 0 && ckpt->sec <= conn->hot_backup_start) {
            if (is_wt_ckpt) {
                F_CLR(ckpt, WT_CKPT_DELETE);
                continue;
            }
            WT_RET_MSG(session, EBUSY,
              "checkpoint %s blocked by hot backup: it would delete an existing named checkpoint, "
              "and such checkpoints cannot be deleted during a hot backup",
              ckpt->name);
        }
        /*
         * Dropping checkpoints involves a fair amount of work while holding locks. Limit the number
         * of WiredTiger checkpoints dropped per checkpoint.
         */
        if (is_wt_ckpt)
#define WT_MAX_CHECKPOINT_DROP 4
            if (++max_ckpt_drop >= WT_MAX_CHECKPOINT_DROP)
                F_CLR(ckpt, WT_CKPT_DELETE);
    }

    /*
     * Mark old checkpoints that are being deleted and figure out which trees we can skip in this
     * checkpoint.
     */
    WT_RET(__checkpoint_mark_skip(session, ckptbase, force));
    if (F_ISSET(btree, WT_BTREE_SKIP_CKPT)) {
        /*
         * If we decide to skip checkpointing, clear the delete flag on the checkpoints. The list of
         * checkpoints will be cached for a future access. Which checkpoints need to be deleted can
         * change in the meanwhile.
         */
        WT_CKPT_FOREACH (ckptbase, ckpt)
            if (F_ISSET(ckpt, WT_CKPT_DELETE))
                F_CLR(ckpt, WT_CKPT_DELETE);
        return (0);
    }

    /*
     * Lock the checkpoints that will be deleted.
     *
     * Checkpoints are only locked when tracking is enabled, which covers checkpoint and drop
     * operations, but not close. The reasoning is there should be no access to a checkpoint during
     * close, because any thread accessing a checkpoint will also have the current file handle open.
     */
    if (WT_META_TRACKING(session))
        WT_CKPT_FOREACH (ckptbase, ckpt) {
            if (!F_ISSET(ckpt, WT_CKPT_DELETE))
                continue;
            WT_ASSERT(session,
              !WT_PREFIX_MATCH(ckpt->name, WT_CHECKPOINT) || conn->hot_backup_start == 0 ||
                ckpt->sec > conn->hot_backup_start);
            /*
             * We can't delete checkpoints referenced by a cursor. WiredTiger checkpoints are
             * uniquely named and it's OK to have multiple in the system: clear the delete flag for
             * them, and otherwise fail.
             */
            ret = __wt_session_lock_checkpoint(session, ckpt->name);
            if (ret == 0)
                continue;
            if (ret == EBUSY && WT_PREFIX_MATCH(ckpt->name, WT_CHECKPOINT)) {
                F_CLR(ckpt, WT_CKPT_DELETE);
                continue;
            }
            WT_RET_MSG(session, ret, "checkpoint %s cannot be dropped when in-use", ckpt->name);
        }
    /*
     * There are special trees: those being bulk-loaded, salvaged, upgraded or verified during the
     * checkpoint. They should never be part of a checkpoint: we will fail to lock them because the
     * operations have exclusive access to the handles. Named checkpoints will fail in that case,
     * ordinary checkpoints skip files that cannot be opened normally.
     */
    WT_ASSERT(session, !is_checkpoint || !F_ISSET(btree, WT_BTREE_SPECIAL_FLAGS));

    return (0);
}

/*
 * __checkpoint_lock_dirty_tree --
 *     Decide whether the tree needs to be included in the checkpoint and if so, acquire the
 *     necessary locks.
 */
static int
__checkpoint_lock_dirty_tree(
  WT_SESSION_IMPL *session, bool is_checkpoint, bool force, bool need_tracking, const char *cfg[])
{
    WT_BTREE *btree;
    WT_CKPT *ckpt, *ckptbase;
    WT_CONFIG dropconf;
    WT_CONFIG_ITEM cval, k, v;
    WT_DATA_HANDLE *dhandle;
    WT_DECL_RET;
    WT_ITEM *drop_list;
    size_t ckpt_bytes_allocated;
    uint64_t now;
    char *name_alloc;
    const char *name;
    bool is_drop, is_wt_ckpt, seen_ckpt_add, skip_ckpt;

    btree = S2BT(session);
    ckpt = ckptbase = NULL;
    ckpt_bytes_allocated = 0;
    dhandle = session->dhandle;
    drop_list = NULL;
    name_alloc = NULL;
    seen_ckpt_add = false;

    /*
     * Only referenced in diagnostic builds and gcc 5.1 isn't satisfied with wrapping the entire
     * assert condition in the unused macro.
     */
    WT_UNUSED(need_tracking);

    /*
     * Most callers need meta tracking to be on here, otherwise it is
     * possible for this checkpoint to cleanup handles that are still in
     * use. The exceptions are:
     *  - Checkpointing the metadata handle itself.
     *  - On connection close when we know there can't be any races.
     */
    WT_ASSERT(session, !need_tracking || WT_IS_METADATA(dhandle) || WT_META_TRACKING(session));

    /* This may be a named checkpoint, check the configuration. */
    cval.len = 0;
    is_drop = is_wt_ckpt = false;
    if (cfg != NULL)
        WT_ERR(__wt_config_gets(session, cfg, "name", &cval));
    if (cval.len == 0) {
        name = WT_CHECKPOINT;
        is_wt_ckpt = true;
    } else {
        WT_ERR(__checkpoint_name_ok(session, cval.str, cval.len, false));
        WT_ERR(__wt_strndup(session, cval.str, cval.len, &name_alloc));
        name = name_alloc;
    }

    /*
     * Determine if a drop is part of the configuration. It usually isn't, so delay processing more
     * until we know if we need to process this tree.
     */
    if (cfg != NULL) {
        cval.len = 0;
        WT_ERR(__wt_config_gets(session, cfg, "drop", &cval));
        if (cval.len != 0)
            is_drop = true;
    }

    /*
     * This is a complicated test to determine if we can avoid the expensive call of getting the
     * list of checkpoints for this file. We want to avoid that for clean files. But on clean files
     * we want to periodically check if we need to delete old checkpoints that may have been in use
     * by an open cursor.
     */
    if (!btree->modified && !force && is_checkpoint && is_wt_ckpt && !is_drop) {
        /* In the common case of the timer set forever, don't even check the time. */
        skip_ckpt = true;
        if (btree->clean_ckpt_timer != WT_BTREE_CLEAN_CKPT_FOREVER) {
            __wt_seconds(session, &now);
            if (now > btree->clean_ckpt_timer)
                skip_ckpt = false;
        }

        /* Skip the clean btree until the btree has obsolete pages. */
        if (skip_ckpt && !F_ISSET(btree, WT_BTREE_OBSOLETE_PAGES)) {
            F_SET(btree, WT_BTREE_SKIP_CKPT);
            goto skip;
        }
    }

    /*
     * Discard the saved list of checkpoints, and slow path if this is not a WiredTiger checkpoint
     * or if checkpoint drops are involved. Also, if we do not have checkpoint array size, the
     * regular checkpoint process did not create the array. It is safer to discard the array in such
     * a case.
     */
    if (!is_wt_ckpt || is_drop || btree->ckpt_bytes_allocated == 0)
        __wt_meta_saved_ckptlist_free(session);

    /* If we have to process this btree for any reason, reset the timer and obsolete pages flag. */
    WT_BTREE_CLEAN_CKPT(session, btree, 0);
    F_CLR(btree, WT_BTREE_OBSOLETE_PAGES);

    WT_ERR(__wt_meta_ckptlist_get(session, dhandle->name, true, &ckptbase, &ckpt_bytes_allocated));

    /* We may be dropping specific checkpoints, check the configuration. */
    if (cfg != NULL) {
        cval.len = 0;
        WT_ERR(__wt_config_gets(session, cfg, "drop", &cval));
        if (cval.len != 0) {
            /* Gather the list of named checkpoints to drop (if any) from the first tree visited. */
            if (session->ckpt_drop_list == NULL) {
                WT_ERR(__wt_scr_alloc(session, cval.len + 10, &session->ckpt_drop_list));
                WT_ERR(__wt_buf_set(session, session->ckpt_drop_list, "(", 1));
                drop_list = session->ckpt_drop_list;
            }

            __wt_config_subinit(session, &dropconf, &cval);
            while ((ret = __wt_config_next(&dropconf, &k, &v)) == 0) {
                /* Disallow unsafe checkpoint names. */
                if (v.len == 0)
                    WT_ERR(__checkpoint_name_ok(session, k.str, k.len, true));
                else
                    WT_ERR(__checkpoint_name_ok(session, v.str, v.len, true));

                if (v.len == 0)
                    WT_ERR(__drop(session, drop_list, ckptbase, k.str, k.len));
                else if (WT_STRING_MATCH("from", k.str, k.len))
                    WT_ERR(__drop_from(session, drop_list, ckptbase, v.str, v.len));
                else if (WT_STRING_MATCH("to", k.str, k.len))
                    WT_ERR(__drop_to(session, drop_list, ckptbase, v.str, v.len));
                else
                    WT_ERR_MSG(session, EINVAL, "unexpected value for checkpoint key: %.*s",
                      (int)k.len, k.str);
            }
            WT_ERR_NOTFOUND_OK(ret, false);

            if (drop_list != NULL)
                WT_ERR(__wt_buf_catfmt(session, drop_list, ")"));
        }
    }

    /*
     * Drop checkpoints with the same name as the one we're taking. We don't need to add this to the
     * drop list for snapshot/timestamp metadata because the metadata will be replaced by the new
     * checkpoint.
     */
    WT_ERR(__drop(session, NULL, ckptbase, name, strlen(name)));

    /* Set the name of the new entry at the end of the list. */
    WT_CKPT_FOREACH (ckptbase, ckpt)
        ;
    WT_ERR(__wt_strdup(session, name, &ckpt->name));

    /*
     * There is some interaction between backups and checkpoints. Perform all backup related
     * operations that the checkpoint needs now, while holding the hot backup read lock.
     */
    WT_WITH_HOTBACKUP_READ_LOCK_UNCOND(session,
      ret = __checkpoint_lock_dirty_tree_int(session, is_checkpoint, force, btree, ckpt, ckptbase));
    WT_ERR(ret);

    /*
     * If we decided to skip checkpointing, we need to remove the new checkpoint entry we might have
     * appended to the list.
     */
    if (F_ISSET(btree, WT_BTREE_SKIP_CKPT)) {
        WT_CKPT_FOREACH_NAME_OR_ORDER (ckptbase, ckpt) {
            /* Checkpoint(s) to be added are always at the end of the list. */
            WT_ASSERT(session, !seen_ckpt_add || F_ISSET(ckpt, WT_CKPT_ADD));
            if (F_ISSET(ckpt, WT_CKPT_ADD)) {
                seen_ckpt_add = true;
                __wt_meta_checkpoint_free(session, ckpt);
            }
        }
    }

    if (ckptbase->name != NULL) {
        btree->ckpt = ckptbase;
        btree->ckpt_bytes_allocated = ckpt_bytes_allocated;
    } else {
        /* It is possible that we do not have any checkpoint in the list. */
err:
        __wt_meta_ckptlist_free(session, &ckptbase);
        btree->ckpt = NULL;
        btree->ckpt_bytes_allocated = 0;
    }
skip:
    __wt_free(session, name_alloc);

    WT_UNUSED(seen_ckpt_add);
    return (ret);
}

/*
 * __checkpoint_apply_obsolete --
 *     Returns true if the checkpoint is obsolete.
 */
static bool
__checkpoint_apply_obsolete(WT_SESSION_IMPL *session, WT_BTREE *btree, WT_CKPT *ckpt)
{
    wt_timestamp_t stop_ts;

    stop_ts = WT_TS_MAX;
    if (ckpt->size != 0) {
        /*
         * If the checkpoint has a valid stop timestamp, mark the btree as having obsolete pages.
         * This flag is used to avoid skipping the btree until the obsolete check is performed on
         * the checkpoints.
         */
        if (ckpt->ta.newest_stop_ts != WT_TS_MAX) {
            F_SET(btree, WT_BTREE_OBSOLETE_PAGES);
            stop_ts = ckpt->ta.newest_stop_durable_ts;
        }
        if (__wt_txn_visible_all(session, ckpt->ta.newest_stop_txn, stop_ts)) {
            WT_STAT_CONN_DATA_INCR(session, txn_checkpoint_obsolete_applied);
            return (true);
        }
    }

    return (false);
}

/*
 * __checkpoint_mark_skip --
 *     Figure out whether the checkpoint can be skipped for a tree.
 */
static int
__checkpoint_mark_skip(WT_SESSION_IMPL *session, WT_CKPT *ckptbase, bool force)
{
    WT_BTREE *btree;
    WT_CKPT *ckpt;
    uint64_t timer;
    int deleted;
    const char *name;

    btree = S2BT(session);

    /*
     * Check for clean objects not requiring a checkpoint.
     *
     * If we're closing a handle, and the object is clean, we can skip the checkpoint, whatever
     * checkpoints we have are sufficient. (We might not have any checkpoints if the object was
     * never modified, and that's OK: the object creation code doesn't mark the tree modified so we
     * can skip newly created trees here.)
     *
     * If the application repeatedly checkpoints an object (imagine hourly checkpoints using the
     * same explicit or internal name), there's no reason to repeat the checkpoint for clean
     * objects. The test is if the only checkpoint we're deleting is the last one in the list and it
     * has the same name as the checkpoint we're about to take, skip the work. (We can't skip
     * checkpoints that delete more than the last checkpoint because deleting those checkpoints
     * might free up space in the file.) This means an application toggling between two (or more)
     * checkpoint names will repeatedly take empty checkpoints, but that's not likely enough to make
     * detection worthwhile.
     *
     * Checkpoint read-only objects otherwise: the application must be able to open the checkpoint
     * in a cursor after taking any checkpoint, which means it must exist.
     */
    F_CLR(btree, WT_BTREE_SKIP_CKPT);
    if (!btree->modified && !force) {
        deleted = 0;
        WT_CKPT_FOREACH (ckptbase, ckpt) {
            /*
             * Don't skip the objects that have obsolete pages to let them to be removed as part of
             * checkpoint cleanup.
             */
            if (__checkpoint_apply_obsolete(session, btree, ckpt))
                return (0);

            if (F_ISSET(ckpt, WT_CKPT_DELETE))
                ++deleted;
        }

        /*
         * Complicated test: if the tree is clean and last two checkpoints have the same name
         * (correcting for internal checkpoint names with their generational suffix numbers), we can
         * skip the checkpoint, there's nothing to do. The exception is if we're deleting two or
         * more checkpoints: then we may save space.
         */
        name = (ckpt - 1)->name;
        if (ckpt > ckptbase + 1 && deleted < 2 &&
          (strcmp(name, (ckpt - 2)->name) == 0 ||
            (WT_PREFIX_MATCH(name, WT_CHECKPOINT) &&
              WT_PREFIX_MATCH((ckpt - 2)->name, WT_CHECKPOINT)))) {
            F_SET(btree, WT_BTREE_SKIP_CKPT);
            /*
             * If there are potentially extra checkpoints to delete, we set the timer to recheck
             * later. If there are at most two checkpoints, the current one and possibly a previous
             * one, then we know there are no additional ones to delete. In that case, set the timer
             * to forever. If the table gets dirtied or a checkpoint is forced that will clear the
             * timer.
             */
            if (ckpt - ckptbase > 2) {
                __wt_seconds(session, &timer);
                timer += WT_MINUTE * WT_BTREE_CLEAN_MINUTES;
                WT_BTREE_CLEAN_CKPT(session, btree, timer);
            } else
                WT_BTREE_CLEAN_CKPT(session, btree, WT_BTREE_CLEAN_CKPT_FOREVER);
            return (0);
        }
    }

    return (0);
}

/*
 * __wt_checkpoint_tree_reconcile_update --
 *     Update a checkpoint based on reconciliation results.
 */
void
__wt_checkpoint_tree_reconcile_update(WT_SESSION_IMPL *session, WT_TIME_AGGREGATE *ta)
{
    WT_BTREE *btree;
    WT_CKPT *ckpt, *ckptbase;

    btree = S2BT(session);

    /*
     * Reconciliation just wrote a checkpoint, everything has been written. Update the checkpoint
     * with reconciliation information. The reason for this function is the reconciliation code just
     * passes through the btree structure's checkpoint array, it doesn't know any more.
     */
    ckptbase = btree->ckpt;
    WT_CKPT_FOREACH (ckptbase, ckpt)
        if (F_ISSET(ckpt, WT_CKPT_ADD)) {
            ckpt->write_gen = btree->write_gen;
            ckpt->run_write_gen = btree->run_write_gen;
            WT_TIME_AGGREGATE_COPY(&ckpt->ta, ta);
        }
}

/*
 * __checkpoint_save_ckptlist --
 *     Post processing of the ckptlist to carry forward a cached list for the next checkpoint.
 */
static int
__checkpoint_save_ckptlist(WT_SESSION_IMPL *session, WT_CKPT *ckptbase)
{
    WT_CKPT *ckpt, *ckpt_itr;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;

    ckpt_itr = ckptbase;
    WT_ERR(__wt_scr_alloc(session, 0, &tmp));
    WT_CKPT_FOREACH (ckptbase, ckpt) {
        /* Remove any deleted checkpoints, by shifting the array. */
        if (F_ISSET(ckpt, WT_CKPT_DELETE)) {
            __wt_meta_checkpoint_free(session, ckpt);
            continue;
        }

        /* Clean up block manager information. */
        __wt_free(session, ckpt->bpriv);
        ckpt->bpriv = NULL;

        /* Update the internal checkpoints to their full names, with the generation count suffix. */
        if (strcmp(ckpt->name, WT_CHECKPOINT) == 0) {
            WT_ERR(__wt_buf_fmt(session, tmp, "%s.%" PRId64, WT_CHECKPOINT, ckpt->order));
            __wt_free(session, ckpt->name);
            WT_ERR(__wt_strdup(session, tmp->mem, &ckpt->name));
        }

        /* Reset the flags, and mark a checkpoint fake if there is no address. */
        ckpt->flags = 0;
        if (ckpt->addr.size == 0) {
            WT_ASSERT(session, ckpt->addr.data == NULL);
            F_SET(ckpt, WT_CKPT_FAKE);
        }

        /* Shift the valid checkpoints, if there are deleted checkpoints in the list. */
        if (ckpt_itr != ckpt) {
            *ckpt_itr = *ckpt;
            WT_CLEAR(*ckpt);
        }
        ckpt_itr++;
    }

    /*
     * Confirm that the last checkpoint has a metadata entry that we can use to base a new
     * checkpoint on.
     */
    ckpt_itr--;
    WT_ASSERT(session, ckpt_itr->block_metadata != NULL);

err:
    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * __checkpoint_tree --
 *     Checkpoint a single tree. Assumes all necessary locks have been acquired by the caller.
 */
static int
__checkpoint_tree(WT_SESSION_IMPL *session, bool is_checkpoint, const char *cfg[])
{
    WT_BM *bm;
    WT_BTREE *btree;
    WT_CONNECTION_IMPL *conn;
    WT_DATA_HANDLE *dhandle;
    WT_DECL_RET;
    WT_LSN ckptlsn;
    WT_TIME_AGGREGATE ta;
    bool fake_ckpt, resolve_bm;

    WT_UNUSED(cfg);

    btree = S2BT(session);
    bm = btree->bm;
    conn = S2C(session);
    dhandle = session->dhandle;
    fake_ckpt = resolve_bm = false;
    WT_TIME_AGGREGATE_INIT(&ta);

    /*
     * Set the checkpoint LSN to the maximum LSN so that if logging is disabled, recovery will never
     * roll old changes forward over the non-logged changes in this checkpoint. If logging is
     * enabled, a real checkpoint LSN will be assigned for this checkpoint and overwrite this.
     */
    WT_MAX_LSN(&ckptlsn);

    /*
     * If an object has never been used (in other words, if it could become a bulk-loaded file),
     * then we must fake the checkpoint. This is good because we don't write physical checkpoint
     * blocks for just-created files, but it's not just a good idea. The reason is because deleting
     * a physical checkpoint requires writing the file, and fake checkpoints can't write the file.
     * If you (1) create a physical checkpoint for an empty file which writes blocks, (2) start
     * bulk-loading records into the file, (3) during the bulk-load perform another checkpoint with
     * the same name; in order to keep from having two checkpoints with the same name you would have
     * to use the bulk-load's fake checkpoint to delete a physical checkpoint, and that will end in
     * tears.
     */
    if (is_checkpoint && btree->original) {
        __wt_checkpoint_tree_reconcile_update(session, &ta);

        fake_ckpt = true;
        goto fake;
    }

    /*
     * Mark the root page dirty to ensure something gets written. (If the tree is modified, we must
     * write the root page anyway, this doesn't add additional writes to the process. If the tree is
     * not modified, we have to dirty the root page to ensure something gets written.) This is
     * really about paranoia: if the tree modification value gets out of sync with the set of dirty
     * pages (modify is set, but there are no dirty pages), we perform a checkpoint without any
     * writes, no checkpoint is created, and then things get bad. While marking the root page as
     * dirty, we do not want to dirty the btree because we are marking the btree as clean just after
     * this call. Also, marking the btree dirty at this stage will unnecessarily mark the connection
     * as dirty causing checkpoint-skip code to fail.
     */
    WT_ERR(__wt_page_modify_init(session, btree->root.page));
    __wt_page_only_modify_set(session, btree->root.page);

    /*
     * Clear the tree's modified flag; any changes before we clear the flag are guaranteed to be
     * part of this checkpoint (unless reconciliation skips updates for transactional reasons), and
     * changes subsequent to the checkpoint start, which might not be included, will re-set the
     * modified flag. The "unless reconciliation skips updates" problem is handled in the
     * reconciliation code: if reconciliation skips updates, it sets the modified flag itself.
     */
    btree->modified = false;
    WT_FULL_BARRIER();

    /* Tell logging that a file checkpoint is starting. */
    if (FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED))
        WT_ERR(__wt_txn_checkpoint_log(session, false, WT_TXN_LOG_CKPT_START, &ckptlsn));

    /* Tell the block manager that a file checkpoint is starting. */
    WT_ERR(bm->checkpoint_start(bm, session));
    resolve_bm = true;

    /* Flush the file from the cache, creating the checkpoint. */
    if (is_checkpoint)
        WT_ERR(__wt_sync_file(session, WT_SYNC_CHECKPOINT));
    else
        WT_ERR(__wt_evict_file(session, WT_SYNC_CLOSE));

fake:
    /*
     * If we're faking a checkpoint and logging is enabled, recovery should roll forward any changes
     * made between now and the next checkpoint, so set the checkpoint LSN to the beginning of time.
     */
    if (fake_ckpt && FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED))
        WT_INIT_LSN(&ckptlsn);

    /*
     * Update the object's metadata.
     *
     * If the object is the metadata, the call to __wt_meta_ckptlist_set will update the turtle file
     * and swap the new one into place. We need to make sure the metadata is on disk before the
     * turtle file is updated.
     *
     * If we are doing a checkpoint in a file without a transaction (e.g., closing a dirty tree
     * before an exclusive operation like verify), the metadata update will be auto-committed. In
     * that case, we need to sync the file here or we could roll forward the metadata in recovery
     * and open a checkpoint that isn't yet durable.
     */
    if (WT_IS_METADATA(dhandle) || !F_ISSET(session->txn, WT_TXN_RUNNING))
        WT_ERR(__wt_checkpoint_sync(session, NULL));

    WT_ERR(__wt_meta_ckptlist_set(session, dhandle, btree->ckpt, &ckptlsn));

    /*
     * If we wrote a checkpoint (rather than faking one), we have to resolve it. Normally, tracking
     * is enabled and resolution deferred until transaction end. The exception is if the handle is
     * being discarded, in which case the handle will be gone by the time we try to apply or unroll
     * the meta tracking event.
     */
    if (!fake_ckpt) {
        resolve_bm = false;
        if (WT_META_TRACKING(session) && is_checkpoint)
            WT_ERR(__wt_meta_track_checkpoint(session));
        else
            WT_ERR(bm->checkpoint_resolve(bm, session, false));
    }

    /* Tell logging that the checkpoint is complete. */
    if (FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED))
        WT_ERR(__wt_txn_checkpoint_log(session, false, WT_TXN_LOG_CKPT_STOP, NULL));

err:
    /* Resolved the checkpoint for the block manager in the error path. */
    if (resolve_bm)
        WT_TRET(bm->checkpoint_resolve(bm, session, ret != 0));

    /*
     * If the checkpoint didn't complete successfully, make sure the tree is marked dirty.
     */
    if (ret != 0) {
        btree->modified = true;
        conn->modified = true;
    }

    /* For a successful checkpoint, post process the ckptlist, to keep a cached copy around. */
    if (ret != 0 || WT_IS_METADATA(session->dhandle) || F_ISSET(conn, WT_CONN_CLOSING))
        __wt_meta_saved_ckptlist_free(session);
    else {
        ret = __checkpoint_save_ckptlist(session, btree->ckpt);
        /* Discard the saved checkpoint list if processing the list did not work. */
        if (ret != 0)
            __wt_meta_saved_ckptlist_free(session);
    }

    return (ret);
}

/*
 * __checkpoint_presync --
 *     Visit all handles after the checkpoint writes are complete and before syncing. At this point,
 *     all trees should be completely open for business.
 */
static int
__checkpoint_presync(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_BTREE *btree;

    WT_UNUSED(cfg);

    btree = S2BT(session);
    WT_ASSERT(session, btree->checkpoint_gen == __wt_gen(session, WT_GEN_CHECKPOINT));
    btree->evict_walk_period = btree->evict_walk_saved;
    return (0);
}

/*
 * __checkpoint_tree_helper --
 *     Checkpoint a tree (suitable for use in *_apply functions).
 */
static int
__checkpoint_tree_helper(WT_SESSION_IMPL *session, const char *cfg[])
{
    struct timespec tsp;
    WT_BTREE *btree;
    WT_DECL_RET;
    WT_TXN *txn;
    bool with_timestamp;

    btree = S2BT(session);
    txn = session->txn;

    /* Add a two seconds wait to simulate checkpoint slowness for every handle. */
    tsp.tv_sec = 2;
    tsp.tv_nsec = 0;
    __checkpoint_timing_stress(session, WT_TIMING_STRESS_CHECKPOINT_HANDLE, &tsp);

    /* Are we using a read timestamp for this checkpoint transaction? */
    with_timestamp = F_ISSET(txn, WT_TXN_SHARED_TS_READ);

    /* Logged tables ignore any read timestamp configured for the checkpoint. */
    if (F_ISSET(btree, WT_BTREE_LOGGED))
        F_CLR(txn, WT_TXN_SHARED_TS_READ);

    ret = __checkpoint_tree(session, true, cfg);

    /* Restore the use of the timestamp for other tables. */
    if (with_timestamp)
        F_SET(txn, WT_TXN_SHARED_TS_READ);

    /*
     * Whatever happened, we aren't visiting this tree again in this checkpoint. Don't keep updates
     * pinned any longer.
     */
    __checkpoint_update_generation(session);

    /*
     * In case this tree was being skipped by the eviction server during the checkpoint, restore the
     * previous state.
     */
    btree->evict_walk_period = btree->evict_walk_saved;

    /*
     * Wake the eviction server, in case application threads have stalled while the eviction server
     * decided it couldn't make progress. Without this, application threads will be stalled until
     * the eviction server next wakes.
     */
    __wt_evict_server_wake(session);

    return (ret);
}

/*
 * __wt_checkpoint --
 *     Checkpoint a file.
 */
int
__wt_checkpoint(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    bool force, standalone;

    /* Should not be called with a checkpoint handle. */
    WT_ASSERT(session, !WT_READING_CHECKPOINT(session));

    /* We must hold the metadata lock if checkpointing the metadata. */
    WT_ASSERT(session,
      !WT_IS_METADATA(session->dhandle) ||
        FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_METADATA));

    /* If we're already in a global checkpoint, don't get a new time. Otherwise, we need one. */
    standalone = session->current_ckpt_sec == 0;
    if (standalone)
        __txn_checkpoint_establish_time(session);

    WT_RET(__wt_config_gets_def(session, cfg, "force", 0, &cval));
    force = cval.val != 0;
    WT_SAVE_DHANDLE(session, ret = __checkpoint_lock_dirty_tree(session, true, force, true, cfg));
    if (ret != 0 || F_ISSET(S2BT(session), WT_BTREE_SKIP_CKPT))
        goto done;
    ret = __checkpoint_tree(session, true, cfg);

done:
    if (standalone)
        __txn_checkpoint_clear_time(session);

    /* Do not store the cached checkpoint list when checkpointing a single file alone. */
    __wt_meta_saved_ckptlist_free(session);
    return (ret);
}

/*
 * __wt_checkpoint_sync --
 *     Sync a file that has been checkpointed, and wait for the result.
 */
int
__wt_checkpoint_sync(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_BM *bm;

    WT_UNUSED(cfg);

    bm = S2BT(session)->bm;

    /* Should not be called with a checkpoint handle. */
    WT_ASSERT(session, !WT_READING_CHECKPOINT(session));

    /* Unnecessary if checkpoint_sync has been configured "off". */
    if (!F_ISSET(S2C(session), WT_CONN_CKPT_SYNC))
        return (0);

    return (bm->sync(bm, session, true));
}

/*
 * __wt_checkpoint_close --
 *     Checkpoint a single file as part of closing the handle.
 */
int
__wt_checkpoint_close(WT_SESSION_IMPL *session, bool final)
{
    WT_BTREE *btree;
    WT_DECL_RET;
    bool bulk, metadata, need_tracking;

    btree = S2BT(session);
    bulk = F_ISSET(btree, WT_BTREE_BULK);
    metadata = WT_IS_METADATA(session->dhandle);

    /*
     * We've done the final checkpoint before the final close, subsequent writes to normal objects
     * are wasted effort. Discard the objects to validate exit accounting.
     */
    if (final && !metadata)
        return (__wt_evict_file(session, WT_SYNC_DISCARD));

    /* Closing an unmodified file. */
    if (!btree->modified && !bulk)
        return (__wt_evict_file(session, WT_SYNC_DISCARD));

    /*
     * Don't flush data from modified trees independent of system-wide checkpoint. Flushing trees
     * can lead to files that are inconsistent on disk after a crash.
     */
    if (btree->modified && !bulk && !metadata)
        return (__wt_set_return(session, EBUSY));

    /*
     * Make sure there isn't a potential race between backup copying the metadata and a checkpoint
     * changing the metadata. Backup holds both the checkpoint and schema locks. Checkpoint should
     * hold those also except on the final checkpoint during close. Confirm the caller either is the
     * final checkpoint or holds at least one of the locks.
     */
    WT_ASSERT(session,
      final ||
        (FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_CHECKPOINT) ||
          FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_SCHEMA)));
    /*
     * Turn on metadata tracking if:
     * - The session is not already doing metadata tracking.
     * - The file was not bulk loaded.
     * - The close is not during connection close.
     */
    need_tracking = !WT_META_TRACKING(session) && !bulk && !final;

    if (need_tracking)
        WT_RET(__wt_meta_track_on(session));

    __txn_checkpoint_establish_time(session);

    WT_SAVE_DHANDLE(
      session, ret = __checkpoint_lock_dirty_tree(session, false, false, need_tracking, NULL));
    WT_ASSERT(session, ret == 0);
    if (ret == 0 && !F_ISSET(btree, WT_BTREE_SKIP_CKPT))
        ret = __checkpoint_tree(session, false, NULL);

    __txn_checkpoint_clear_time(session);

    /* Do not store the cached checkpoint list when closing the handle. */
    __wt_meta_saved_ckptlist_free(session);

    if (need_tracking)
        WT_TRET(__wt_meta_track_off(session, true, ret != 0));

    return (ret);
}

/*
 * __checkpoint_timing_stress --
 *     Optionally add a delay to a checkpoint to simulate a long running checkpoint for debug
 *     purposes. The reason for this option is finding operations that can block while waiting for a
 *     checkpoint to complete.
 */
static void
__checkpoint_timing_stress(WT_SESSION_IMPL *session, uint64_t flag, struct timespec *tsp)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    /*
     * We only want to sleep if the flag is set and the checkpoint comes from the API, so check if
     * the session used is either of the two sessions set aside for internal checkpoints.
     */
    if (conn->ckpt_session != session && conn->meta_ckpt_session != session &&
      FLD_ISSET(conn->timing_stress_flags, flag))
        __wt_sleep((uint64_t)tsp->tv_sec, (uint64_t)tsp->tv_nsec / WT_THOUSAND);
}
