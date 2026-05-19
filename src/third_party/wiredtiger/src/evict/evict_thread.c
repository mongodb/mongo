/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Eviction thread lifecycle and the main server loop. Covers three concerns:
 *
 * Thread management -- __wt_evict_threads_create and __wt_evict_threads_destroy own the thread
 * group that holds the eviction server plus any worker threads. __wt_evict_server_wake signals the
 * server's condition variable to interrupt its sleep when urgent work arrives.
 *
 * Server loop -- __evict_server runs continuously, calling __evict_update_work to recompute cache
 * pressure flags (dirty, updates, hard), then either sleeping or triggering a pass. __evict_pass
 * coordinates one full round of walking (__wti_evict_lru_walk) and draining (__wti_evict_lru_pages
 * across all worker threads). __evict_update_work also sets the aggressive-eviction score when the
 * server detects the cache is stuck.
 *
 * Worker tuning -- __evict_tune_workers samples eviction throughput every EVICT_TUNE_PERIOD ms and
 * adds or removes worker threads to maximize pages evicted per second, stabilizing once the rate
 * stops improving or the configured maximum is reached.
 */
#include "wt_internal.h"

#define EVICT_TUNE_BATCH 1 /* Max workers to add each period */
                           /*
                            * Data points needed before deciding if we should keep adding workers or
                            * settle on an earlier value.
                            */
#define EVICT_TUNE_DATAPT_MIN 8
#define EVICT_TUNE_PERIOD 60 /* Tune period in milliseconds */

/*
 * We will do a fresh re-tune every that many milliseconds to adjust to significant phase changes.
 */
#define EVICT_FORCE_RETUNE (25 * WT_THOUSAND)

static bool __evict_thread_chk(WT_SESSION_IMPL *);
static int __evict_thread_run(WT_SESSION_IMPL *, WT_THREAD *);
static int __evict_thread_stop(WT_SESSION_IMPL *, WT_THREAD *);
static int __evict_server(WT_SESSION_IMPL *, bool *);
static int __evict_pass(WT_SESSION_IMPL *);
static void __evict_tune_workers(WT_SESSION_IMPL *);
static int __evict_update_work(WT_SESSION_IMPL *, bool *);

/*
 * __evict_thread_chk --
 *     Check to decide if the eviction thread should continue running.
 */
static bool
__evict_thread_chk(WT_SESSION_IMPL *session)
{
    return (FLD_ISSET(S2C(session)->server_flags, WT_CONN_SERVER_EVICTION));
}

/*
 * __evict_thread_run --
 *     Entry function for an eviction thread. This is called repeatedly from the thread group code
 *     so it does not need to loop itself.
 */
static int
__evict_thread_run(WT_SESSION_IMPL *session, WT_THREAD *thread)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_EVICT *evict;
    bool did_work, was_intr;

    conn = S2C(session);
    evict = conn->evict;

    /* Mark the session as an eviction thread session. */
    F_SET(session, WT_SESSION_EVICTION);

    /*
     * Cache a history store cursor to avoid deadlock: if an eviction thread marks a file busy and
     * then opens a different file (in this case, the HS file), it can deadlock with a thread
     * waiting for the first file to drain from the eviction queue.
     */
    WT_ERR(__wt_curhs_cache(session));
    if (__wt_atomic_load_bool_relaxed(&conn->evict_config.server_running) &&
      __wt_spin_trylock(session, &evict->evict_pass_lock) == 0) {
        /*
         * Cannot use WTI_WITH_PASS_LOCK because this is a try lock. Fix when that is supported. We
         * set the flag on both sessions because we may call clear_walk when we are walking with the
         * walk session, locked.
         */
        FLD_SET(session->lock_flags, WT_SESSION_LOCKED_PASS);
        FLD_SET(evict->walk_session->lock_flags, WT_SESSION_LOCKED_PASS);
        ret = __evict_server(session, &did_work);
        FLD_CLR(evict->walk_session->lock_flags, WT_SESSION_LOCKED_PASS);
        FLD_CLR(session->lock_flags, WT_SESSION_LOCKED_PASS);
        was_intr = __wt_atomic_load_uint32_v_relaxed(&evict->pass_intr) != 0;
        __wt_spin_unlock(session, &evict->evict_pass_lock);
        WT_ERR(ret);

        /*
         * If the eviction server was interrupted, wait until requests have been processed: the
         * system may otherwise be busy so don't go to sleep.
         */
        if (was_intr)
            while (__wt_atomic_load_uint32_v_relaxed(&evict->pass_intr) != 0 &&
              FLD_ISSET(conn->server_flags, WT_CONN_SERVER_EVICTION) &&
              F_ISSET(thread, WT_THREAD_RUN))
                __wt_yield();
        else {
            __wt_verbose_debug2(session, WT_VERB_EVICTION, "%s", "sleeping");

            /* Don't rely on signals: check periodically. */
            __wt_cond_auto_wait(session, evict->evict_cond, did_work, NULL);
            __wt_verbose_debug2(session, WT_VERB_EVICTION, "%s", "waking");
        }
    } else
        WT_ERR(__wti_evict_lru_pages(session, false));

    if (0) {
err:
        WT_RET_PANIC(session, ret, "eviction thread error");
    }
    return (ret);
}

/*
 * __evict_thread_stop --
 *     Shutdown function for an eviction thread.
 */
static int
__evict_thread_stop(WT_SESSION_IMPL *session, WT_THREAD *thread)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_EVICT *evict;

    if (thread->id != 0)
        return (0);

    conn = S2C(session);
    evict = conn->evict;
    /*
     * The only time the first eviction thread is stopped is on shutdown: in case any trees are
     * still open, clear all walks now so that they can be closed.
     */
    WTI_WITH_PASS_LOCK(session, ret = __wti_evict_clear_all_walks_and_saved_tree(session));
    WT_ERR(ret);
    /*
     * The only cases when the eviction server is expected to stop are when recovery is finished,
     * when the connection is closing or when an error has occurred and connection panic flag is
     * set.
     */
    WT_ASSERT(session,
      F_ISSET_ATOMIC_32(conn, WT_CONN_CLOSING | WT_CONN_PANIC) ||
        F_ISSET(conn, WT_CONN_RECOVERING));

    /* Clear the eviction thread session flag. */
    F_CLR(session, WT_SESSION_EVICTION);

    __wt_verbose_info(session, WT_VERB_EVICTION, "%s", "eviction thread exiting");

    if (0) {
err:
        WT_RET_PANIC(session, ret, "eviction thread error");
    }
    return (ret);
}

/*
 * __evict_server --
 *     Thread to evict pages from the cache.
 */
static int
__evict_server(WT_SESSION_IMPL *session, bool *did_work)
{
    struct timespec now;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_EVICT *evict;
    uint64_t time_diff_ms;

    /* Assume there has been no progress. */
    *did_work = false;

    conn = S2C(session);
    evict = conn->evict;

    WT_ASSERT_SPINLOCK_OWNED(session, &evict->evict_pass_lock);

    /*
     * Copy the connection setting for use in the current run of Eviction Server. This ensures that
     * no hazard pointers are leaked in case the setting is reconfigured while eviction pass is
     * running.
     */
    evict->use_npos_in_pass = __wt_atomic_load_bool_relaxed(&conn->evict_config.use_npos);

    /* Evict pages from the cache as needed. */
    WT_RET(__evict_pass(session));

    if (!FLD_ISSET(conn->server_flags, WT_CONN_SERVER_EVICTION) ||
      __wt_atomic_load_uint32_v_relaxed(&evict->pass_intr) != 0)
        return (0);

    if (!__wt_evict_cache_stuck(session)) {
        if (evict->use_npos_in_pass)
            __wti_evict_set_saved_walk_tree(session, NULL);
        else {
            /*
             * Try to get the handle list lock: if we give up, that indicates a session is waiting
             * for us to clear walks. Do that as part of a normal pass (without the handle list
             * lock) to avoid deadlock.
             */
            if ((ret = __wti_evict_lock_handle_list(session)) == EBUSY)
                return (0);
            WT_RET(ret);

            /*
             * Clear the walks so we don't pin pages while asleep, otherwise we can block
             * applications evicting large pages.
             */
            ret = __wti_evict_clear_all_walks_and_saved_tree(session);

            __wt_readunlock(session, &conn->dhandle_lock);
            WT_RET(ret);
        }
        /* Make sure we'll notice next time we're stuck. */
        evict->last_eviction_progress = 0;
        return (0);
    }

    /* Track if work was done. */
    *did_work =
      __wt_atomic_load_uint64_v_relaxed(&evict->eviction_progress) != evict->last_eviction_progress;
    evict->last_eviction_progress = __wt_atomic_load_uint64_v_relaxed(&evict->eviction_progress);

    /* Eviction is stuck, check if we have made progress. */
    if (*did_work) {
#if !defined(HAVE_DIAGNOSTIC)
        /* Need verbose check only if not in diagnostic build */
        if (WT_VERBOSE_ISSET(session, WT_VERB_EVICTION))
#endif
            __wt_epoch(session, &evict->stuck_time);
        return (0);
    }

#if !defined(HAVE_DIAGNOSTIC)
    /* Need verbose check only if not in diagnostic build */
    if (!WT_VERBOSE_ISSET(session, WT_VERB_EVICTION))
        return (0);
#endif
    /*
     * If we're stuck for 5 minutes in diagnostic mode, or the verbose eviction flag is configured,
     * log the cache and transaction state.
     *
     * If we're stuck for 5 minutes in diagnostic mode, give up.
     *
     * We don't do this check for in-memory workloads because application threads are not blocked by
     * the cache being full. If the cache becomes full of clean pages, we can be servicing reads
     * while the cache appears stuck to eviction.
     */
    if (F_ISSET(conn, WT_CONN_IN_MEMORY))
        return (0);

    __wt_epoch(session, &now);

    /* The checks below should only be executed when a cache timeout has been set. */
    if (evict->cache_stuck_timeout_ms > 0) {
        time_diff_ms = WT_TIMEDIFF_MS(now, evict->stuck_time);
#ifdef HAVE_DIAGNOSTIC
        /* Enable extra logs 20ms before timing out. */
        if (evict->cache_stuck_timeout_ms < 20 ||
          (time_diff_ms > evict->cache_stuck_timeout_ms - 20))
            WT_SET_VERBOSE_LEVEL(session, WT_VERB_EVICTION, WT_VERBOSE_DEBUG_1);
#endif

        if (time_diff_ms >= evict->cache_stuck_timeout_ms) {
#ifdef HAVE_DIAGNOSTIC
            __wt_err(session, ETIMEDOUT, "Cache stuck for too long, giving up");
            WT_RET(__wt_verbose_dump_txn(session));
            WT_RET(__wt_verbose_dump_cache(session));
            WT_RET(__wt_verbose_dump_metadata(session));
            return (__wt_set_return(session, ETIMEDOUT));
#else
            if (WT_VERBOSE_ISSET(session, WT_VERB_EVICTION)) {
                WT_RET(__wt_verbose_dump_txn(session));
                WT_RET(__wt_verbose_dump_cache(session));

                /* Reset the timer. */
                __wt_epoch(session, &evict->stuck_time);
            }
#endif
        }
    }
    return (0);
}

/* !!!
 * __wt_evict_server_wake --
 *     Wake up the eviction server thread. The eviction server typically sleeps for some time when
 *     cache usage is below the target thresholds. When the cache is expected to exceed these
 *     thresholds, callers can nudge the eviction server to wake up and resume its work.
 *
 *     This function is called in situations where pages are queued for urgent eviction or when
 *     application threads request eviction assistance.
 */
void
__wt_evict_server_wake(WT_SESSION_IMPL *session)
{
    WT_CACHE *cache;
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);
    cache = conn->cache;

    if (WT_VERBOSE_LEVEL_ISSET(session, WT_VERB_EVICTION, WT_VERBOSE_DEBUG_2)) {
        uint64_t bytes_dirty, bytes_inuse, bytes_max, bytes_updates;

        bytes_inuse = __wt_cache_bytes_inuse(cache);
        bytes_max = conn->cache_size;
        bytes_dirty = __wt_cache_dirty_inuse(cache);
        bytes_updates = __wt_cache_bytes_updates(cache);
        __wt_verbose_debug2(session, WT_VERB_EVICTION,
          "waking, bytes inuse %s max (%" PRIu64 "MB %s %" PRIu64 "MB), bytes dirty %" PRIu64
          "(bytes), bytes updates %" PRIu64 "(bytes)",
          bytes_inuse <= bytes_max ? "<=" : ">", bytes_inuse / WT_MEGABYTE,
          bytes_inuse <= bytes_max ? "<=" : ">", bytes_max / WT_MEGABYTE, bytes_dirty,
          bytes_updates);
    }

    __wt_cond_signal(session, conn->evict->evict_cond);
}

/* !!!
 * __wt_evict_threads_create --
 *     Initiate the eviction process by creating and launching the eviction threads.
 *
 *     The `threads_max` and `threads_min` configurations in `api_data.py` control the maximum and
 *     minimum number of eviction worker threads in WiredTiger. One of the threads acts as the
 *     eviction server, responsible for identifying evictable pages and placing them in eviction
 *     queues. The remaining threads are eviction workers, responsible for evicting pages from these
 *     eviction queues.
 *
 *     This function is called once during `wiredtiger_open` or recovery.
 *
 *     Return an error code if the thread group creation fails.
 */
int
__wt_evict_threads_create(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    uint32_t session_flags;

    conn = S2C(session);
    __wt_verbose_info(session, WT_VERB_EVICTION, "%s", "starting eviction threads");

    /*
     * In case recovery has allocated some transaction IDs, bump to the current state. This will
     * prevent eviction threads from pinning anything as they start up and read metadata in order to
     * open cursors.
     */
    WT_RET(__wt_txn_update_oldest(session, WT_TXN_OLDEST_STRICT | WT_TXN_OLDEST_WAIT));

    WT_ASSERT(session, conn->evict_config.threads_min > 0);
    /* Set first, the thread might run before we finish up. */
    FLD_SET(conn->server_flags, WT_CONN_SERVER_EVICTION);

    /*
     * Create the eviction thread group. Set the group size to the maximum allowed sessions.
     */
    session_flags = WT_THREAD_CAN_WAIT | WT_THREAD_PANIC_FAIL;
    WT_RET(__wt_thread_group_create(session, &conn->evict_config.threads, "eviction-server",
      conn->evict_config.threads_min, conn->evict_config.threads_max, session_flags,
      __evict_thread_chk, __evict_thread_run, __evict_thread_stop));

/*
 * Ensure the cache stuck timer is initialized when starting eviction.
 */
#if !defined(HAVE_DIAGNOSTIC)
    /* Need verbose check only if not in diagnostic build */
    if (WT_VERBOSE_ISSET(session, WT_VERB_EVICTION))
#endif
        __wt_epoch(session, &conn->evict->stuck_time);

    /*
     * Allow queues to be populated now that the eviction threads are running.
     */
    __wt_atomic_store_bool_relaxed(&conn->evict_config.server_running, true);

    return (0);
}

/* !!!
 * __wt_evict_threads_destroy --
 *     Stop and destroy the eviction threads. It must be called exactly once during
 *     `WT_CONNECTION::close` or recovery to ensure all eviction threads are properly terminated.
 *
 *     Return an error code if the thread group destruction fails.
 */
int
__wt_evict_threads_destroy(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    /* We are done if the eviction server didn't start successfully. */
    if (!__wt_atomic_load_bool_relaxed(&conn->evict_config.server_running))
        return (0);

    __wt_verbose_info(session, WT_VERB_EVICTION, "%s", "stopping eviction threads");

    /* Wait for any eviction thread group changes to stabilize. */
    __wt_writelock(session, &conn->evict_config.threads.lock);

    /*
     * Signal the threads to finish and stop populating the queue.
     */
    FLD_CLR(conn->server_flags, WT_CONN_SERVER_EVICTION);
    __wt_atomic_store_bool_relaxed(&conn->evict_config.server_running, false);
    __wt_evict_server_wake(session);

    __wt_verbose_info(session, WT_VERB_EVICTION, "%s", "waiting for eviction threads to stop");

    /*
     * We call the destroy function still holding the write lock. It assumes it is called locked.
     */
    WT_RET(__wt_thread_group_destroy(session, &conn->evict_config.threads));

    return (0);
}

/*
 * __evict_update_work --
 *     Configure eviction work state.
 */
static int
__evict_update_work(WT_SESSION_IMPL *session, bool *eviction_needed)
{
    WT_BTREE *hs_tree;
    WT_CACHE *cache;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_EVICT *evict;
    double dirty_target, dirty_trigger, target, trigger, updates_target, updates_trigger;
    uint64_t bytes_dirty, bytes_inuse, bytes_max, bytes_updates, total_dirty, total_inmem,
      total_updates;
    uint32_t flags, hs_id;

    conn = S2C(session);
    cache = conn->cache;
    evict = conn->evict;

    dirty_target = __wti_evict_dirty_target(evict);
    dirty_trigger = __wt_atomic_load_double_relaxed(&evict->eviction_dirty_trigger);
    target = evict->eviction_target;
    trigger = evict->eviction_trigger;
    updates_target = evict->eviction_updates_target;
    updates_trigger = __wt_atomic_load_double_relaxed(&evict->eviction_updates_trigger);

    /* Build up the new state. */
    flags = 0;

    if (!FLD_ISSET(conn->server_flags, WT_CONN_SERVER_EVICTION)) {
        __wt_atomic_store_uint32_relaxed(&evict->flags, 0);
        *eviction_needed = false;
        return (0);
    }

    if (!__evict_queue_empty(evict->evict_urgent_queue, false))
        LF_SET(WT_EVICT_CACHE_URGENT);

    /*
     * TODO: We are caching the cache usage values associated with the history store because the
     * history store dhandle isn't always available to eviction. Keeping potentially out-of-date
     * values could lead to surprising bugs in the future.
     */
    if (F_ISSET_ATOMIC_32(conn, WT_CONN_HS_OPEN)) {
        total_dirty = total_inmem = total_updates = 0;
        hs_id = 0;
        for (;;) {
            WT_RET_NOTFOUND_OK(ret = __wt_curhs_next_hs_id(session, hs_id, &hs_id));
            if (ret == WT_NOTFOUND) {
                ret = 0;
                (void)ret; /* Keep the assignment to 0 just in case, but suppress clang warnings. */
                break;
            }
            /*
             * At this point, we are under the evict pass lock and should only attempt to read from
             * the cursors dhandle cache to obtain the HS. If it is not present in the cursors
             * dhandle cache, we bail out. We must not proceed to acquire a connection dhandle read
             * lock or a schema lock to acquire the HS dhandle while holding the pass lock, as this
             * could lead to a deadlock. There are several places in the code where a pass lock is
             * taken after a schema lock, which makes this sequence unsafe.
             */
            WT_RET_NOTFOUND_OK(ret = __wt_curhs_get_cached(session, hs_id, &hs_tree));
            if (ret == 0) {
                total_inmem += __wt_atomic_load_uint64_relaxed(&hs_tree->bytes_inmem);
                total_dirty += __wt_atomic_load_uint64_relaxed(&hs_tree->bytes_dirty_intl) +
                  __wt_atomic_load_uint64_relaxed(&hs_tree->bytes_dirty_leaf);
                total_updates += __wt_atomic_load_uint64_relaxed(&hs_tree->bytes_updates);
            } else {
                if (hs_id == WT_HS_ID)
                    WT_STAT_CONN_INCR(session, cache_eviction_hs_cursor_not_cached);
                else if (hs_id == WT_HS_ID_SHARED)
                    WT_STAT_CONN_INCR(session, cache_eviction_hs_shared_cursor_not_cached);
            }
        }
        __wt_atomic_store_uint64_relaxed(&cache->bytes_hs, total_inmem);
        __wt_atomic_store_uint64_relaxed(&cache->bytes_hs_dirty, total_dirty);
        __wt_atomic_store_uint64_relaxed(&cache->bytes_hs_updates, total_updates);
    }

    /*
     * If we need space in the cache, try to find clean pages to evict.
     *
     * Avoid division by zero if the cache size has not yet been set in a shared cache.
     */
    bytes_max = __wt_tsan_suppress_load_uint64_v(&conn->cache_size) + 1;
    bytes_inuse = __wt_cache_bytes_inuse(cache);
    if (__wt_evict_clean_needed(session, NULL)) {
        LF_SET(WT_EVICT_CACHE_CLEAN | WT_EVICT_CACHE_CLEAN_HARD);
        WT_STAT_CONN_INCR(session, cache_eviction_trigger_reached);
    } else if (bytes_inuse > (target * bytes_max) / 100) {
        LF_SET(WT_EVICT_CACHE_CLEAN);
    }

    bytes_dirty = __wt_cache_dirty_leaf_inuse(cache);
    if (__wt_evict_dirty_needed(session, NULL)) {
        LF_SET(WT_EVICT_CACHE_DIRTY | WT_EVICT_CACHE_DIRTY_HARD);
        WT_STAT_CONN_INCR(session, cache_eviction_trigger_dirty_reached);
    } else if (bytes_dirty > (uint64_t)(dirty_target * bytes_max) / 100) {
        LF_SET(WT_EVICT_CACHE_DIRTY);
    }

    bytes_updates = __wt_cache_bytes_updates(cache);
    if (__wti_evict_updates_needed(session, NULL)) {
        LF_SET(WT_EVICT_CACHE_UPDATES | WT_EVICT_CACHE_UPDATES_HARD);
        WT_STAT_CONN_INCR(session, cache_eviction_trigger_updates_reached);
    } else if (bytes_updates > (uint64_t)(updates_target * bytes_max) / 100) {
        LF_SET(WT_EVICT_CACHE_UPDATES);
    }

    /*
     * If application threads are blocked by data in cache, track the fill ratio.
     *
     */
    uint64_t cache_fill_ratio = bytes_inuse / bytes_max;
    bool evict_is_hard = LF_ISSET(WT_EVICT_CACHE_HARD);
    if (evict_is_hard) {
        if (cache_fill_ratio < 0.25)
            WT_STAT_CONN_INCR(session, cache_eviction_app_threads_fill_ratio_lt_25);
        else if (cache_fill_ratio < 0.50)
            WT_STAT_CONN_INCR(session, cache_eviction_app_threads_fill_ratio_25_50);
        else if (cache_fill_ratio < 0.75)
            WT_STAT_CONN_INCR(session, cache_eviction_app_threads_fill_ratio_50_75);
        else
            WT_STAT_CONN_INCR(session, cache_eviction_app_threads_fill_ratio_gt_75);
    }

    /*
     * If application threads are blocked by the total volume of data in cache or we cannot find
     * enough pages to evict, try dirty pages as well.
     */
    if (LF_ISSET(WT_EVICT_CACHE_CLEAN_HARD) &&
      (__wt_evict_aggressive(session) || evict->evict_empty_score > WT_EVICT_SCORE_CUTOFF))
        LF_SET(WT_EVICT_CACHE_DIRTY);

    /*
     * Configure scrub - which reinstates clean equivalents of reconciled dirty pages. This is
     * useful because an evicted dirty page isn't necessarily a good proxy for knowing if the page
     * will be accessed again soon. Be more aggressive about scrubbing in disaggregated storage
     * because the cost of retrieving a recently reconciled page is higher in that configuration. In
     * the local storage case scrub dirty pages and keep them in cache if we are less than half way
     * to the clean, dirty and updates triggers.
     *
     * There's an experimental flag WT_CACHE_PREFER_SCRUB_EVICTION that can be turned on to enable
     * scrub eviction as long as cache usage overall is under half way to the trigger limit.
     */
    if (__wt_conn_is_disagg(session) && bytes_inuse < (uint64_t)(trigger * bytes_max) / 100)
        LF_SET(WT_EVICT_CACHE_SCRUB);
    else if (bytes_inuse < (uint64_t)((target + trigger) * bytes_max) / 200) {
        if (F_ISSET_ATOMIC_32(
              &(conn->cache->cache_eviction_controls), WT_CACHE_PREFER_SCRUB_EVICTION)) {
            LF_SET(WT_EVICT_CACHE_SCRUB);
        } else if (bytes_dirty < (uint64_t)((dirty_target + dirty_trigger) * bytes_max) / 200 &&
          bytes_updates < (uint64_t)((updates_target + updates_trigger) * bytes_max) / 200) {
            LF_SET(WT_EVICT_CACHE_SCRUB);
        }

    } else
        LF_SET(WT_EVICT_CACHE_NOKEEP);

    if (FLD_ISSET(conn->debug.flags, WT_CONN_DEBUG_UPDATE_RESTORE_EVICT)) {
        LF_SET(WT_EVICT_CACHE_SCRUB);
        LF_CLR(WT_EVICT_CACHE_NOKEEP);
    }

    /*
     * With an in-memory cache, we only do dirty eviction in order to scrub pages.
     */
    if (F_ISSET(conn, WT_CONN_IN_MEMORY)) {
        if (LF_ISSET(WT_EVICT_CACHE_CLEAN))
            LF_SET(WT_EVICT_CACHE_DIRTY);
        if (LF_ISSET(WT_EVICT_CACHE_CLEAN_HARD))
            LF_SET(WT_EVICT_CACHE_DIRTY_HARD);
        LF_CLR(WT_EVICT_CACHE_CLEAN | WT_EVICT_CACHE_CLEAN_HARD);
    }

    /* Update the global eviction state. */
    __wt_atomic_store_uint32_relaxed(&evict->flags, flags);

    *eviction_needed = F_ISSET(evict, WT_EVICT_CACHE_ALL | WT_EVICT_CACHE_URGENT);
    return (0);
}

/*
 * __evict_pass --
 *     Evict pages from memory.
 */
static int
__evict_pass(WT_SESSION_IMPL *session)
{
    WT_CACHE *cache;
    WT_CONNECTION_IMPL *conn;
    WT_EVICT *evict;
    WT_TXN_GLOBAL *txn_global;
    uint64_t eviction_progress, oldest_id, prev_oldest_id;
    uint64_t time_now, time_prev;
    u_int loop;
    bool eviction_needed;

    conn = S2C(session);
    cache = conn->cache;
    evict = conn->evict;
    eviction_needed = false;
    txn_global = &conn->txn_global;
    time_prev = 0; /* [-Wconditional-uninitialized] */

    /* Track whether pages are being evicted and progress is made. */
    eviction_progress = __wt_atomic_load_uint64_v_relaxed(&evict->eviction_progress);
    prev_oldest_id = __wt_atomic_load_uint64_v_relaxed(&txn_global->oldest_id);

    /* Evict pages from the cache. */
    for (loop = 0; __wt_atomic_load_uint32_v_relaxed(&evict->pass_intr) == 0; loop++) {
        time_now = __wt_clock(session);
        if (loop == 0)
            time_prev = time_now;

        __evict_tune_workers(session);
        /*
         * Increment the shared read generation. Do this occasionally even if eviction is not
         * currently required, so that pages have some relative read generation when the eviction
         * server does need to do some work.
         */
        __wt_atomic_add_uint64(&evict->read_gen, 1);
        __wt_atomic_add_uint64(&evict->evict_pass_gen, 1);

        /*
         * Update the oldest ID: we use it to decide whether pages are candidates for eviction.
         * Without this, if all threads are blocked after a long-running transaction (such as a
         * checkpoint) completes, we may never start evicting again.
         *
         * Do this every time the eviction server wakes up, regardless of whether the cache is full,
         * to prevent the oldest ID falling too far behind. Don't wait to lock the table: with
         * highly threaded workloads, that creates a bottleneck.
         */
        WT_RET(__wt_txn_update_oldest(session, WT_TXN_OLDEST_STRICT));

        WT_RET(__evict_update_work(session, &eviction_needed));
        if (!eviction_needed)
            break;

        __wt_verbose_debug2(session, WT_VERB_EVICTION,
          "Eviction pass with: Max: %" PRIu64 " In use: %" PRIu64 " Dirty: %" PRIu64
          " Updates: %" PRIu64,
          conn->cache_size, __wt_atomic_load_uint64_relaxed(&cache->bytes_inmem),
          __wt_atomic_load_uint64_relaxed(&cache->bytes_dirty_intl) +
            __wt_atomic_load_uint64_relaxed(&cache->bytes_dirty_leaf),
          __wt_atomic_load_uint64_relaxed(&cache->bytes_updates));

        if (F_ISSET(evict, WT_EVICT_CACHE_ALL))
            WT_RET(__wti_evict_lru_walk(session));

        /*
         * If the queue has been empty recently, keep queuing more pages to evict. If the rate of
         * queuing pages is high enough, this score will go to zero, in which case the eviction
         * server might as well help out with eviction.
         *
         * Also, if there is a single eviction server thread with no workers, it must service the
         * urgent queue in case all application threads are busy.
         */
        if (!WT_EVICT_HAS_WORKERS(session) &&
          (evict->evict_empty_score < WT_EVICT_SCORE_CUTOFF ||
            !__evict_queue_empty(evict->evict_urgent_queue, false)))
            WT_RET(__wti_evict_lru_pages(session, true));

        if (__wt_atomic_load_uint32_v_relaxed(&evict->pass_intr) != 0)
            break;

        /*
         * If we're making progress, keep going; if we're not making any progress at all, mark the
         * cache "stuck" and go back to sleep, it's not something we can fix.
         *
         * We check for progress every 20ms, the idea being that the aggressive score will reach 10
         * after 200ms if we aren't making progress and eviction will start considering more pages.
         * If there is still no progress after 2s, we will treat the cache as stuck and start
         * rolling back transactions and writing updates to the history store table.
         */
        if (eviction_progress == __wt_atomic_load_uint64_v_relaxed(&evict->eviction_progress)) {
            if (WT_CLOCKDIFF_MS(time_now, time_prev) >= 20 && F_ISSET(evict, WT_EVICT_CACHE_HARD)) {
                if (__wt_atomic_load_uint32_relaxed(&evict->evict_aggressive_score) <
                  WT_EVICT_SCORE_MAX)
                    (void)__wt_atomic_add_uint32_v(&evict->evict_aggressive_score, 1);
                oldest_id = __wt_atomic_load_uint64_v_relaxed(&txn_global->oldest_id);
                if (prev_oldest_id == oldest_id &&
                  __wt_atomic_load_uint64_v_relaxed(&txn_global->current) != oldest_id &&
                  __wt_atomic_load_uint32_relaxed(&evict->evict_aggressive_score) <
                    WT_EVICT_SCORE_MAX)
                    (void)__wt_atomic_add_uint32_v(&evict->evict_aggressive_score, 1);
                time_prev = time_now;
                prev_oldest_id = oldest_id;
            }

            /*
             * Keep trying for long enough that we should be able to evict a page if the server
             * isn't interfering.
             */
            if (loop < 100 ||
              __wt_atomic_load_uint32_relaxed(&evict->evict_aggressive_score) <
                WT_EVICT_SCORE_MAX) {
                /*
                 * Back off if we aren't making progress: walks hold the handle list lock, blocking
                 * other operations that can free space in cache.
                 *
                 * Allow this wait to be interrupted (e.g. if a checkpoint completes): make sure we
                 * wait for a non-zero number of microseconds).
                 */
                WT_STAT_CONN_INCR(session, eviction_server_slept);
                __wt_cond_wait(session, evict->evict_cond, WT_THOUSAND, NULL);
                continue;
            }

            WT_STAT_CONN_INCR(session, eviction_slow);
            __wt_verbose_debug1(session, WT_VERB_EVICTION, "%s", "unable to reach eviction goal");
            break;
        }
        __wt_atomic_decrement_if_positive(&evict->evict_aggressive_score);
        loop = 0;
        eviction_progress = __wt_atomic_load_uint64_v_relaxed(&evict->eviction_progress);
    }
    return (0);
}

/*
 * __evict_tune_workers --
 *     Find the right number of eviction workers. Gradually ramp up the number of workers increasing
 *     the number in batches indicated by the setting above. Store the number of workers that gave
 *     us the best throughput so far and the number of data points we have tried. Every once in a
 *     while when we have the minimum number of data points we check whether the eviction throughput
 *     achieved with the current number of workers is the best we have seen so far. If so, we will
 *     keep increasing the number of workers. If not, we are past the infliction point on the
 *     eviction throughput curve. In that case, we will set the number of workers to the best
 *     observed so far and settle into a stable state.
 */
static void
__evict_tune_workers(WT_SESSION_IMPL *session)
{
    struct timespec current_time;
    WT_CONNECTION_IMPL *conn;
    WT_EVICT *evict;
    uint64_t delta_msec, delta_pages;
    uint64_t eviction_progress, eviction_progress_rate, time_diff;
    uint32_t current_threads;
    int32_t cur_threads, i, target_threads, thread_surplus;

    conn = S2C(session);
    evict = conn->evict;

    /*
     * If we have a fixed number of eviction threads, there is no value in calculating if we should
     * do any tuning.
     */
    if (conn->evict_config.threads_max == conn->evict_config.threads_min)
        return;

    __wt_epoch(session, &current_time);
    time_diff = WT_TIMEDIFF_MS(current_time, evict->evict_tune_last_time);

    /*
     * If we have reached the stable state and have not run long enough to surpass the forced
     * re-tuning threshold, return.
     */
    if (evict->evict_tune_stable) {
        if (time_diff < EVICT_FORCE_RETUNE)
            return;

        /*
         * Stable state was reached a long time ago. Let's re-tune. Reset all the state.
         */
        evict->evict_tune_stable = false;
        evict->evict_tune_last_action_time.tv_sec = 0;
        evict->evict_tune_progress_last = 0;
        evict->evict_tune_num_points = 0;
        evict->evict_tune_progress_rate_max = 0;

        /* Reduce the number of eviction workers by one */
        thread_surplus =
          (int32_t)__wt_atomic_load_uint32_relaxed(&conn->evict_config.threads.current_threads) -
          (int32_t)conn->evict_config.threads_min;

        if (thread_surplus > 0)
            __wt_thread_group_stop_one(session, &conn->evict_config.threads);

    } else if (time_diff < EVICT_TUNE_PERIOD)
        /*
         * If we have not reached stable state, don't do anything unless enough time has passed
         * since the last time we have taken any action in this function.
         */
        return;

    /*
     * Measure the evicted progress so far. Eviction rate correlates to performance, so this is our
     * metric of success.
     */
    eviction_progress = __wt_atomic_load_uint64_v_relaxed(&evict->eviction_progress);

    /*
     * If we have recorded the number of pages evicted at the end of the previous measurement
     * interval, we can compute the eviction rate in evicted pages per second achieved during the
     * current measurement interval. Otherwise, we just record the number of evicted pages and
     * return.
     */
    if (evict->evict_tune_progress_last == 0)
        goto done;

    delta_msec = WT_TIMEDIFF_MS(current_time, evict->evict_tune_last_time);
    delta_pages = eviction_progress - evict->evict_tune_progress_last;
    eviction_progress_rate = (delta_pages * WT_THOUSAND) / delta_msec;
    evict->evict_tune_num_points++;

    /*
     * Keep track of the maximum eviction throughput seen and the number of workers corresponding to
     * that throughput.
     */
    if (eviction_progress_rate > evict->evict_tune_progress_rate_max) {
        evict->evict_tune_progress_rate_max = eviction_progress_rate;
        current_threads =
          __wt_atomic_load_uint32_relaxed(&conn->evict_config.threads.current_threads);
        __wt_atomic_store_uint32_relaxed(&evict->evict_tune_workers_best, current_threads);
    }

    /*
     * Compare the current number of data points with the number needed variable. If they are equal,
     * we will check whether we are still going up on the performance curve, in which case we will
     * increase the number of needed data points, to provide opportunity for further increasing the
     * number of workers. Or we are past the inflection point on the curve, in which case we will go
     * back to the best observed number of workers and settle into a stable state.
     */
    if (evict->evict_tune_num_points >= evict->evict_tune_datapts_needed) {
        current_threads =
          __wt_atomic_load_uint32_relaxed(&conn->evict_config.threads.current_threads);
        if (evict->evict_tune_workers_best == current_threads &&
          current_threads < conn->evict_config.threads_max) {
            /*
             * Keep adding workers. We will check again at the next check point.
             */
            evict->evict_tune_datapts_needed += WT_MIN(EVICT_TUNE_DATAPT_MIN,
              (conn->evict_config.threads_max - current_threads) / EVICT_TUNE_BATCH);
        } else {
            /*
             * We are past the inflection point. Choose the best number of eviction workers observed
             * and settle into a stable state.
             */
            thread_surplus = (int32_t)__wt_atomic_load_uint32_relaxed(
                               &conn->evict_config.threads.current_threads) -
              (int32_t)evict->evict_tune_workers_best;

            for (i = 0; i < thread_surplus; i++)
                __wt_thread_group_stop_one(session, &conn->evict_config.threads);

            evict->evict_tune_stable = true;
            goto done;
        }
    }

    /*
     * If we have not added any worker threads in the past, we set the number of data points needed
     * equal to the number of data points that we must accumulate before deciding if we should keep
     * adding workers or settle on a previously tried stable number of workers.
     */
    if (evict->evict_tune_last_action_time.tv_sec == 0)
        evict->evict_tune_datapts_needed = EVICT_TUNE_DATAPT_MIN;

    if (F_ISSET(evict, WT_EVICT_CACHE_ALL)) {
        cur_threads =
          (int32_t)__wt_atomic_load_uint32_relaxed(&conn->evict_config.threads.current_threads);
        target_threads =
          WT_MIN(cur_threads + EVICT_TUNE_BATCH, (int32_t)conn->evict_config.threads_max);
        /*
         * Start the new threads.
         */
        for (i = cur_threads; i < target_threads; ++i) {
            __wt_thread_group_start_one(session, &conn->evict_config.threads, false);
            __wt_verbose_debug1(session, WT_VERB_EVICTION, "%s", "added worker thread");
        }
        evict->evict_tune_last_action_time = current_time;
    }

done:
    evict->evict_tune_last_time = current_time;
    evict->evict_tune_progress_last = eviction_progress;
}
