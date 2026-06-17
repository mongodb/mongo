/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Eviction dispatch: selecting candidate pages from the LRU and urgent queues and driving their
 * eviction through the page and reconciliation layers.
 *
 * __evict_get_ref pulls the next candidate from the queue (preferring the urgent queue when
 * present) and transitions its ref state to WT_REF_LOCKED before handing it off. __wti_evict_page
 * runs one page through the full eviction path. __wti_evict_app_assist_worker is the entry point
 * for application threads that are required to perform eviction themselves when cache dirty or
 * update thresholds are exceeded. __wt_evict_page_urgent inserts a specific page into the urgent
 * queue to force its eviction ahead of normal LRU ordering. __wt_evict_priority_set and
 * __wt_evict_priority_clear control a per-tree priority that causes the walk to skip a tree
 * unless eviction is in an aggressive state.
 */
#include "wt_internal.h"

/*
 * __evict_get_ref --
 *     Get a page for eviction.
 */
static int
__evict_get_ref(WT_SESSION_IMPL *session, bool is_server, WT_BTREE **btreep, WT_REF **refp,
  WT_REF_STATE *previous_statep)
{
    WT_EVICT *evict;
    WTI_EVICT_ENTRY *evict_entry;
    WTI_EVICT_QUEUE *queue, *other_queue, *urgent_queue;
    WT_REF_STATE previous_state;
    uint32_t candidates;
    bool is_app, server_only, urgent_ok;

    *btreep = NULL;

    /*
     * It is polite to initialize output variables, but it isn't safe for callers to use the
     * previous state if we don't return a locked ref.
     */
    *previous_statep = WT_REF_MEM;
    *refp = NULL;

    evict = S2C(session)->evict;
    is_app = !F_ISSET(session, WT_SESSION_INTERNAL);
    server_only = is_server && !WT_EVICT_HAS_WORKERS(session);
    /* Application threads do eviction when cache is full of dirty data */
    urgent_ok = (!is_app && !is_server) || !WT_EVICT_HAS_WORKERS(session) ||
      (is_app && F_ISSET(evict, WT_EVICT_CACHE_DIRTY_HARD));
    urgent_queue = evict->evict_urgent_queue;

    /* Avoid the LRU lock if no pages are available. */
    if (__evict_queue_empty(evict->evict_current_queue, is_server) &&
      __evict_queue_empty(evict->evict_other_queue, is_server) &&
      (!urgent_ok || __evict_queue_empty(urgent_queue, false))) {
        WT_STAT_CONN_INCR(session, eviction_get_ref_empty);
        return (WT_NOTFOUND);
    }

    /*
     * The server repopulates whenever the other queue is not full, as long as at least one page has
     * been evicted out of the current queue.
     *
     * Note that there are pathological cases where there are only enough eviction candidates in the
     * cache to fill one queue. In that case, we will continually evict one page and attempt to
     * refill the queues. Such cases are extremely rare in real applications.
     */
    if (is_server && (!urgent_ok || __evict_queue_empty(urgent_queue, false)) &&
      !__evict_queue_full(evict->evict_current_queue) &&
      !__evict_queue_full(evict->evict_fill_queue) &&
      (evict->evict_empty_score > WT_EVICT_SCORE_CUTOFF ||
        __evict_queue_empty(evict->evict_fill_queue, false)))
        return (WT_NOTFOUND);

    uint64_t lock_wait_start, lock_wait_end;
    /* Track time spent waiting for the evict queue lock */
    lock_wait_start = __wt_clock(session);
    __wt_spin_lock(session, &evict->evict_queue_lock);
    lock_wait_end = __wt_clock(session);

    /* Only track lock wait time for eviction worker threads */
    if (F_ISSET(session, WT_SESSION_INTERNAL))
        __wt_atomic_add_uint64_v(
          &evict->evict_lock_wait_time, WT_CLOCKDIFF_US(lock_wait_end, lock_wait_start));

    /* Check the urgent queue first. */
    if (urgent_ok && !__evict_queue_empty(urgent_queue, false))
        queue = urgent_queue;
    else {
        /*
         * Check if the current queue needs to change.
         *
         * The server will only evict half of the pages before looking for more, but should only
         * switch queues if there are no other eviction workers.
         */
        queue = evict->evict_current_queue;
        other_queue = evict->evict_other_queue;
        if (__evict_queue_empty(queue, server_only) &&
          !__evict_queue_empty(other_queue, server_only)) {
            evict->evict_current_queue = other_queue;
            evict->evict_other_queue = queue;
        }
    }

    __wt_spin_unlock(session, &evict->evict_queue_lock);

    /*
     * We got the queue lock, which should be fast, and chose a queue. Now we want to get the lock
     * on the individual queue.
     */
    for (;;) {
        /* Verify there are still pages available. */
        if (__evict_queue_empty(queue, is_server && queue != urgent_queue)) {
            WT_STAT_CONN_INCR(session, eviction_get_ref_empty2);
            return (WT_NOTFOUND);
        }
        if (!is_server) {
            lock_wait_start = __wt_clock(session);
            __wt_spin_lock(session, &queue->evict_lock);
            lock_wait_end = __wt_clock(session);
            if (F_ISSET(session, WT_SESSION_INTERNAL))
                __wt_atomic_add_uint64_v(
                  &evict->evict_lock_wait_time, WT_CLOCKDIFF_US(lock_wait_end, lock_wait_start));
        } else if (__wt_spin_trylock(session, &queue->evict_lock) != 0)
            continue;
        break;
    }

    /*
     * Only evict half of the pages before looking for more. The remainder are left to eviction
     * workers (if configured), or application thread if necessary.
     */
    candidates = queue->evict_candidates;
    if (is_server && queue != urgent_queue && candidates > 1)
        candidates /= 2;

    /* Get the next page queued for eviction. */
    for (evict_entry = queue->evict_current;
      evict_entry >= queue->evict_queue && evict_entry < queue->evict_queue + candidates;
      ++evict_entry) {
        if (evict_entry->ref == NULL)
            continue;
        WT_ASSERT(session, evict_entry->btree != NULL);

        /*
         * Evicting a dirty page in the server thread could stall during a write and prevent
         * eviction from finding new work.
         *
         * However, we can't skip entries in the urgent queue or they may never be found again.
         *
         * Don't force application threads to evict dirty pages if they aren't stalled by the amount
         * of dirty data in cache.
         */
        if (!urgent_ok &&
          (is_server || !F_ISSET(evict, WT_EVICT_CACHE_DIRTY_HARD | WT_EVICT_CACHE_UPDATES_HARD)) &&
          __wt_page_is_modified(evict_entry->ref->page)) {
            --evict_entry;
            break;
        }

        /*
         * Lock the page while holding the eviction mutex to prevent multiple attempts to evict it.
         * For pages that are already being evicted, this operation will fail and we will move on.
         */
        if ((previous_state = WT_REF_GET_STATE(evict_entry->ref)) != WT_REF_MEM ||
          !WT_REF_CAS_STATE(session, evict_entry->ref, previous_state, WT_REF_LOCKED)) {
            __evict_list_clear(session, evict_entry);
            continue;
        }

        /*
         * Increment the busy count in the btree handle to prevent it from being closed under us.
         */
        (void)__wt_atomic_add_uint32_v(&evict_entry->btree->evict_busy, 1);

        *btreep = evict_entry->btree;
        *refp = evict_entry->ref;
        *previous_statep = previous_state;

        /*
         * Remove the entry so we never try to reconcile the same page on reconciliation error.
         */
        __evict_list_clear(session, evict_entry);
        break;
    }

    /* Move to the next item. */
    if (evict_entry != NULL && evict_entry + 1 < queue->evict_queue + queue->evict_candidates)
        queue->evict_current = evict_entry + 1;
    else /* Clear the current pointer if there are no more candidates. */
        queue->evict_current = NULL;

    __wt_spin_unlock(session, &queue->evict_lock);

    return (*refp == NULL ? WT_NOTFOUND : 0);
}

/*
 * __wti_evict_page --
 *     Called by both eviction and application threads to evict a page.
 */
int
__wti_evict_page(WT_SESSION_IMPL *session, bool is_server)
{
    WT_BTREE *btree;
    WT_DECL_RET;
    WT_REF *ref;
    WT_REF_STATE previous_state;
    WT_TRACK_OP_DECL;
    uint64_t time_start, time_stop;
    uint32_t flags;
    bool page_is_modified;

    WT_TRACK_OP_INIT(session);

    WT_RET_TRACK(__evict_get_ref(session, is_server, &btree, &ref, &previous_state));
    WT_ASSERT(session, WT_REF_GET_STATE(ref) == WT_REF_LOCKED);

    time_start = 0;

    flags = 0;
    page_is_modified = false;

    /*
     * An internal session flags either the server itself or an eviction worker thread.
     */
    if (is_server)
        WT_STAT_CONN_INCR(session, eviction_server_evict_attempt);
    else if (F_ISSET(session, WT_SESSION_INTERNAL))
        WT_STAT_CONN_INCR(session, eviction_worker_evict_attempt);
    else {
        if (__wt_page_is_modified(ref->page)) {
            page_is_modified = true;
            WT_STAT_CONN_INCR(session, eviction_app_dirty_attempt);
        }
        WT_STAT_CONN_INCR(session, eviction_app_attempt);
        __wt_tsan_suppress_add_uint64(&S2C(session)->evict->app_evicts, 1);
        time_start = WT_STAT_ENABLED(session) ? __wt_clock(session) : 0;
    }

    /*
     * In case something goes wrong, don't pick the same set of pages every time.
     *
     * We used to bump the page's read generation only if eviction failed, but that isn't safe: at
     * that point, eviction has already unlocked the page and some other thread may have evicted it
     * by the time we look at it.
     */
    __wti_evict_read_gen_bump(session, ref->page);

    WT_WITH_BTREE(session, btree, ret = __wt_evict(session, ref, previous_state, flags));

    (void)__wt_atomic_sub_uint32_v(&btree->evict_busy, 1);

    if (time_start != 0) {
        time_stop = __wt_clock(session);
        WT_STAT_CONN_INCRV(session, eviction_app_time, WT_CLOCKDIFF_US(time_stop, time_start));
    }

    if (WT_UNLIKELY(ret != 0)) {
        if (is_server)
            WT_STAT_CONN_INCR(session, eviction_server_evict_fail);
        else if (F_ISSET(session, WT_SESSION_INTERNAL))
            WT_STAT_CONN_INCR(session, eviction_worker_evict_fail);
        else {
            if (page_is_modified)
                WT_STAT_CONN_INCR(session, eviction_app_dirty_fail);
            WT_STAT_CONN_INCR(session, eviction_app_fail);
        }
    }

    WT_TRACK_OP_END(session);
    return (ret);
}

/*
 * __wti_evict_app_assist_worker --
 *     Worker function for __wt_evict_app_assist_worker_check: evict pages if the cache crosses
 *     eviction trigger thresholds.
 *
 * The function returns an error code from either __wti_evict_page or __wt_txn_is_blocking.
 */
int
__wti_evict_app_assist_worker(
  WT_SESSION_IMPL *session, bool busy, bool readonly, bool interruptible)
{
    WT_DECL_RET;
    WT_TRACK_OP_DECL;

    WT_TRACK_OP_INIT(session);

    WT_CONNECTION_IMPL *conn = S2C(session);
    WT_EVICT *evict = conn->evict;
    uint64_t time_start = 0;
    WT_TXN_GLOBAL *txn_global = &conn->txn_global;
    WT_TXN_SHARED *txn_shared = WT_SESSION_TXN_SHARED(session);

    uint64_t cache_max_wait_us =
      session->cache_max_wait_us != 0 ? session->cache_max_wait_us : evict->cache_max_wait_us;

    /*
     * Before we enter the eviction generation, make sure this session has a cached history store
     * cursor, otherwise we can deadlock with a session wanting exclusive access to a handle: that
     * session will have a handle list write lock and will be waiting on eviction to drain, we'll be
     * inside eviction waiting on a handle list read lock to open a history store cursor. The
     * eviction server should be started at this point so it is safe to open the history store.
     */
    WT_ERR(__wt_curhs_cache(session));

    /* Wake the eviction server if we need to do work. */
    __wt_evict_server_wake(session);

    /* Track how long application threads spend doing eviction. */
    if (!F_ISSET(session, WT_SESSION_INTERNAL))
        time_start = __wt_clock(session);

    /*
     * Note that this for loop is designed to reset expected eviction error codes before exiting,
     * namely, the busy return and empty eviction queue. We do not need the calling functions to
     * have to deal with internal eviction return codes.
     */
    for (uint64_t initial_progress = __wt_atomic_load_uint64_v_relaxed(&evict->eviction_progress);;
      ret = 0) {
        /*
         * If eviction is stuck, check if this thread is likely causing problems and should be
         * rolled back. Ignore if in recovery, those transactions can't be rolled back.
         */
        if (!F_ISSET(conn, WT_CONN_RECOVERING) && __wt_evict_cache_stuck(session)) {
            ret = __wt_txn_is_blocking(session);
            if (ret == WT_ROLLBACK) {
                __wt_atomic_decrement_if_positive(&evict->evict_aggressive_score);
                if (F_ISSET(session, WT_SESSION_SAVE_ERRORS))
                    __wt_verbose_debug1(session, WT_VERB_TRANSACTION, "rollback reason: %s",
                      session->err_info.err_msg);
            }
            WT_ERR(ret);
        }

        /*
         * Check if we've exceeded our operation timeout, this would also get called from the
         * previous txn is blocking call, however it won't pickup transactions that have been
         * committed or rolled back as their mod count is 0, and that txn needs to be the oldest.
         *
         * Additionally we don't return rollback which could confuse the caller.
         */
        if (__wt_op_timer_fired(session))
            break;

        /* Check if we have exceeded the global or the session timeout for waiting on the cache. */
        if (time_start != 0 && cache_max_wait_us != 0) {
            uint64_t time_stop = __wt_clock(session);
            if (session->cache_wait_us + WT_CLOCKDIFF_US(time_stop, time_start) > cache_max_wait_us)
                break;
        }

        /*
         * Check if we have become busy.
         *
         * If we're busy (because of the transaction check we just did or because our caller is
         * waiting on a longer-than-usual event such as a page read), and the cache level drops
         * below 100%, limit the work to 5 evictions and return. If that's not the case, we can do
         * more.
         */
        if (!busy && __wt_atomic_load_uint64_v_relaxed(&txn_shared->pinned_id) != WT_TXN_NONE &&
          __wt_atomic_load_uint64_v_relaxed(&txn_global->current) !=
            __wt_atomic_load_uint64_v_relaxed(&txn_global->oldest_id))
            busy = true;
        uint64_t max_progress = busy ? 5 : 20;

        /* See if eviction is still needed. */
        double pct_full;
        if (!__wt_evict_needed(session, busy, readonly, true, &pct_full) ||
          (pct_full < 100.0 &&
            (__wt_atomic_load_uint64_v_relaxed(&evict->eviction_progress) >
              initial_progress + max_progress)))
            break;

        if (!__evict_check_user_ok_with_eviction(session, interruptible))
            break;

        /* Evict a page. */
        ret = __wti_evict_page(session, false);
        if (ret == 0) {
            /* If the caller holds resources, we can stop after a successful eviction. */
            if (busy)
                break;
        } else if (ret == WT_NOTFOUND) {
            /* Allow the queue to re-populate before retrying. */
            __wt_cond_wait(session, conn->evict_config.threads.wait_cond, 10 * WT_THOUSAND, NULL);
            __wt_tsan_suppress_add_uint64(&evict->app_waits, 1);
        } else if (ret != EBUSY)
            WT_ERR(ret);

        /* Update elapsed cache metrics. */
        if (time_start != 0) {
            uint64_t time_stop = __wt_clock(session);
            uint64_t elapsed = WT_CLOCKDIFF_US(time_stop, time_start);
            WT_STAT_CONN_INCRV(session, application_cache_time, elapsed);
            if (!interruptible) {
                WT_STAT_CONN_INCRV(session, application_cache_uninterruptible_time, elapsed);
                WT_STAT_SESSION_INCRV(session, cache_time_mandatory, elapsed);
            } else {
                WT_STAT_CONN_INCRV(session, application_cache_interruptible_time, elapsed);
                WT_STAT_SESSION_INCRV(session, cache_time_interruptible, elapsed);
            }
            session->cache_wait_us += elapsed;
            time_start = time_stop;
        }
    }

err:
    if (time_start != 0) {
        uint64_t time_stop = __wt_clock(session);
        uint64_t elapsed = WT_CLOCKDIFF_US(time_stop, time_start);
        WT_STAT_CONN_INCR(session, application_cache_ops);
        WT_STAT_CONN_INCRV(session, application_cache_time, elapsed);
        WT_STAT_SESSION_INCRV(session, cache_time, elapsed);
        if (!interruptible) {
            WT_STAT_CONN_INCR(session, application_cache_uninterruptible_ops);
            WT_STAT_CONN_INCRV(session, application_cache_uninterruptible_time, elapsed);
            WT_STAT_SESSION_INCRV(session, cache_time_mandatory, elapsed);
        } else {
            WT_STAT_CONN_INCR(session, application_cache_interruptible_ops);
            WT_STAT_CONN_INCRV(session, application_cache_interruptible_time, elapsed);
            WT_STAT_SESSION_INCRV(session, cache_time_interruptible, elapsed);
        }
        session->cache_wait_us += elapsed;
        /*
         * Check if a rollback is required only if there has not been an error. Returning an error
         * takes precedence over asking for a rollback. We can not do both.
         */
        if (ret == 0 && cache_max_wait_us != 0 && session->cache_wait_us > cache_max_wait_us) {
            ret = WT_ROLLBACK;
            __wt_session_set_last_error(
              session, ret, WT_CACHE_OVERFLOW, WT_TXN_ROLLBACK_REASON_CACHE_OVERFLOW);
            __wt_atomic_decrement_if_positive(&evict->evict_aggressive_score);

            WT_STAT_CONN_INCR(session, eviction_timed_out_ops);
            if (F_ISSET(session, WT_SESSION_SAVE_ERRORS))
                __wt_verbose_notice(
                  session, WT_VERB_TRANSACTION, "rollback reason: %s", session->err_info.err_msg);
        }
    }

    WT_TRACK_OP_END(session);

    return (ret);
}

/* !!!
 * __wt_evict_page_urgent --
 *     Push a page into the urgent eviction queue.
 *
 *     It is called by the eviction server if pages require immediate eviction or by the application
 *     threads as part of forced eviction when directly evicting pages is not feasible.
 *
 *     Input parameters:
 *       `ref`: A reference to the page that is being added to the urgent eviction queue.
 *
 *     Return `true` if the page has been successfully added to the urgent queue, or `false` is
 *     already marked for eviction.
 */
bool
__wt_evict_page_urgent(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_EVICT *evict;
    WTI_EVICT_ENTRY *evict_entry;
    WTI_EVICT_QUEUE *urgent_queue;
    WT_PAGE *page;
    bool queued;

    /* Root pages should never be evicted via LRU. */
    WT_ASSERT(session, !__wt_ref_is_root(ref));

    page = ref->page;
    if (S2BT(session)->evict_disabled > 0 || F_ISSET_ATOMIC_16(page, WT_PAGE_EVICT_LRU_URGENT)) {
        WT_STAT_CONN_INCR(session, eviction_server_skip_pages_already_in_urgent_queue);
        return (false);
    }

    evict = S2C(session)->evict;
    if (F_ISSET_ATOMIC_16(page, WT_PAGE_EVICT_LRU) && F_ISSET(evict, WT_EVICT_CACHE_ALL))
        return (false);

    /* Append to the urgent queue if we can. */
    urgent_queue = &evict->evict_queues[WTI_EVICT_URGENT_QUEUE];
    queued = false;

    __wt_spin_lock(session, &evict->evict_queue_lock);

    /* Check again, in case we raced with another thread. */
    if (S2BT(session)->evict_disabled > 0 || F_ISSET_ATOMIC_16(page, WT_PAGE_EVICT_LRU_URGENT))
        goto done;

    /*
     * If the page is already in the LRU eviction list, clear it from the list if eviction server is
     * not running.
     */
    if (F_ISSET_ATOMIC_16(page, WT_PAGE_EVICT_LRU)) {
        if (!F_ISSET(evict, WT_EVICT_CACHE_ALL)) {
            __wti_evict_queue_clear_page_locked(session, ref, true);
            WT_STAT_CONN_INCR(session, eviction_clear_ordinary);
        } else
            goto done;
    }

    __wt_spin_lock(session, &urgent_queue->evict_lock);
    if (__evict_queue_empty(urgent_queue, false)) {
        urgent_queue->evict_current = urgent_queue->evict_queue;
        urgent_queue->evict_candidates = 0;
    }
    evict_entry = urgent_queue->evict_queue + urgent_queue->evict_candidates;
    if (evict_entry < urgent_queue->evict_queue + evict->evict_slots &&
      __wti_evict_push_candidate(session, urgent_queue, evict_entry, ref)) {
        ++urgent_queue->evict_candidates;
        queued = true;
        FLD_SET(page->flags_atomic, WT_PAGE_EVICT_LRU_URGENT);
    }
    __wt_spin_unlock(session, &urgent_queue->evict_lock);

done:
    __wt_spin_unlock(session, &evict->evict_queue_lock);
    if (queued) {
        WT_STAT_CONN_INCR(session, eviction_pages_queued_urgent);
        if (WT_EVICT_HAS_WORKERS(session))
            __wt_cond_signal(session, S2C(session)->evict_config.threads.wait_cond);
        else
            __wt_evict_server_wake(session);
    }

    return (queued);
}

/* !!!
 * __wt_evict_priority_set --
 *     Set a tree's eviction priority. A higher priority indicates less likelihood for the tree to
 *     be considered for eviction. The eviction server skips the eviction of trees with a non-zero
 *     priority unless eviction is in an aggressive state and the Btree is significantly utilizing
 *     the cache.
 *
 *     At present, it is exclusively called for metadata file as this is meant to be retained in the
 *     cache.
 *
 *     Input parameter:
 *       `v`: An integer that denotes the priority level.
 */
void
__wt_evict_priority_set(WT_SESSION_IMPL *session, uint64_t v)
{
    S2BT(session)->evict_priority = v;
}

/*
 * __wt_evict_priority_clear --
 *     Clear a tree's eviction priority to zero. It is called during the closure of the
 *     dhandle/btree.
 */
void
__wt_evict_priority_clear(WT_SESSION_IMPL *session)
{
    S2BT(session)->evict_priority = 0;
}
