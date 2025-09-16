/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __evict_clear_all_walks_and_saved_tree(WT_SESSION_IMPL *);
static void __evict_list_clear_page_locked(WT_SESSION_IMPL *, WT_REF *, bool);
static int WT_CDECL __evict_lru_cmp(const void *, const void *);
static int __evict_lru_pages(WT_SESSION_IMPL *, bool);
static int __evict_lru_walk(WT_SESSION_IMPL *);
static int __evict_page(WT_SESSION_IMPL *, bool);
static int __evict_pass(WT_SESSION_IMPL *);
static int __evict_server(WT_SESSION_IMPL *, bool *);
static void __evict_tune_workers(WT_SESSION_IMPL *session);
static int __evict_walk(WT_SESSION_IMPL *, WTI_EVICT_QUEUE *);
static int __evict_walk_tree(WT_SESSION_IMPL *, WTI_EVICT_QUEUE *, u_int, u_int *);

#define WT_EVICT_HAS_WORKERS(s) (__wt_atomic_load32(&S2C(s)->evict_threads.current_threads) > 1)

/*
 * __evict_lock_handle_list --
 *     Try to get the handle list lock, with yield and sleep back off. Keep timing statistics
 *     overall.
 */
static int
__evict_lock_handle_list(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_EVICT *evict;
    WT_RWLOCK *dh_lock;
    u_int spins;

    conn = S2C(session);
    evict = conn->evict;
    dh_lock = &conn->dhandle_lock;

    /*
     * Use a custom lock acquisition back off loop so the eviction server notices any interrupt
     * quickly.
     */
    for (spins = 0; (ret = __wt_try_readlock(session, dh_lock)) == EBUSY &&
         __wt_atomic_loadv32(&evict->pass_intr) == 0;
         spins++) {
        if (spins < WT_THOUSAND)
            __wt_yield();
        else
            __wt_sleep(0, WT_THOUSAND);
    }
    return (ret);
}

/*
 * __evict_entry_priority --
 *     Get the adjusted read generation for an eviction entry.
 */
static WT_INLINE uint64_t
__evict_entry_priority(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_BTREE *btree;
    WT_PAGE *page;
    uint64_t read_gen;

    btree = S2BT(session);
    page = ref->page;

    /* Any page set to the evict_soon or wont_need generation should be discarded. */
    if (__wti_evict_readgen_is_soon_or_wont_need(&page->read_gen))
        return (WT_READGEN_EVICT_SOON);

    /* Any page from a dead tree is a great choice. */
    if (F_ISSET(btree->dhandle, WT_DHANDLE_DEAD))
        return (WT_READGEN_EVICT_SOON);

    /* Any empty page (leaf or internal), is a good choice. */
    if (__wt_page_is_empty(page))
        return (WT_READGEN_EVICT_SOON);

    /* Any large page in memory is likewise a good choice. */
    if (__wt_atomic_loadsize(&page->memory_footprint) > btree->splitmempage)
        return (WT_READGEN_EVICT_SOON);

    /*
     * The base read-generation is skewed by the eviction priority. Internal pages are also
     * adjusted, we prefer to evict leaf pages.
     */
    if (page->modify != NULL && F_ISSET(S2C(session)->evict, WT_EVICT_CACHE_DIRTY) &&
      !F_ISSET(S2C(session)->evict, WT_EVICT_CACHE_CLEAN))
        read_gen = __wt_atomic_load64(&page->modify->update_txn);
    else
        read_gen = __wt_atomic_load64(&page->read_gen);

    read_gen += btree->evict_priority;

#define WT_EVICT_INTL_SKEW WT_THOUSAND
    if (F_ISSET(ref, WT_REF_FLAG_INTERNAL))
        read_gen += WT_EVICT_INTL_SKEW;

    return (read_gen);
}

/*
 * __evict_lru_cmp_debug --
 *     Qsort function: sort the eviction array. Version for eviction debug mode.
 */
static int WT_CDECL
__evict_lru_cmp_debug(const void *a_arg, const void *b_arg)
{
    const WTI_EVICT_ENTRY *a, *b;
    uint64_t a_score, b_score;

    a = a_arg;
    b = b_arg;
    a_score = (a->ref == NULL ? UINT64_MAX : 0);
    b_score = (b->ref == NULL ? UINT64_MAX : 0);

    return ((a_score < b_score) ? -1 : (a_score == b_score) ? 0 : 1);
}

/*
 * __evict_lru_cmp --
 *     Qsort function: sort the eviction array.
 */
static int WT_CDECL
__evict_lru_cmp(const void *a_arg, const void *b_arg)
{
    const WTI_EVICT_ENTRY *a, *b;
    uint64_t a_score, b_score;

    a = a_arg;
    b = b_arg;
    a_score = (a->ref == NULL ? UINT64_MAX : a->score);
    b_score = (b->ref == NULL ? UINT64_MAX : b->score);

    return ((a_score < b_score) ? -1 : (a_score == b_score) ? 0 : 1);
}

/*
 * __evict_list_clear --
 *     Clear an entry in the LRU eviction list.
 */
static WT_INLINE void
__evict_list_clear(WT_SESSION_IMPL *session, WTI_EVICT_ENTRY *e)
{
    if (e->ref != NULL) {
        WT_ASSERT(session, F_ISSET_ATOMIC_16(e->ref->page, WT_PAGE_EVICT_LRU));
        F_CLR_ATOMIC_16(e->ref->page, WT_PAGE_EVICT_LRU | WT_PAGE_EVICT_LRU_URGENT);
    }
    e->ref = NULL;
    e->btree = WT_DEBUG_POINT;
}

/*
 * __evict_list_clear_page_locked --
 *     This function searches for the page in all the eviction queues (skipping the urgent queue if
 *     requested) and clears it if found. It does not take the eviction queue lock, so the caller
 *     should hold the appropriate locks before calling this function.
 */
static void
__evict_list_clear_page_locked(WT_SESSION_IMPL *session, WT_REF *ref, bool exclude_urgent)
{
    WT_EVICT *evict;
    WTI_EVICT_ENTRY *evict_entry;
    uint32_t elem, i, q, last_queue_idx;
    bool found;

    last_queue_idx = exclude_urgent ? WTI_EVICT_URGENT_QUEUE : WTI_EVICT_QUEUE_MAX;
    evict = S2C(session)->evict;
    found = false;

    WT_ASSERT_SPINLOCK_OWNED(session, &evict->evict_queue_lock);

    for (q = 0; q < last_queue_idx && !found; q++) {
        __wt_spin_lock(session, &evict->evict_queues[q].evict_lock);
        elem = evict->evict_queues[q].evict_max;
        for (i = 0, evict_entry = evict->evict_queues[q].evict_queue; i < elem; i++, evict_entry++)
            if (evict_entry->ref == ref) {
                found = true;
                __evict_list_clear(session, evict_entry);
                break;
            }
        __wt_spin_unlock(session, &evict->evict_queues[q].evict_lock);
    }
    WT_ASSERT(session, !F_ISSET_ATOMIC_16(ref->page, WT_PAGE_EVICT_LRU));
}

/*
 * __wti_evict_list_clear_page --
 *     Check whether a page is present in the LRU eviction list. If the page is found in the list,
 *     remove it. This is called from the page eviction code to make sure there is no attempt to
 *     evict a child page multiple times.
 */
void
__wti_evict_list_clear_page(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_EVICT *evict;

    WT_ASSERT(session, __wt_ref_is_root(ref) || WT_REF_GET_STATE(ref) == WT_REF_LOCKED);

    /* Fast path: if the page isn't in the queue, don't bother searching. */
    if (!F_ISSET_ATOMIC_16(ref->page, WT_PAGE_EVICT_LRU))
        return;
    evict = S2C(session)->evict;

    __wt_spin_lock(session, &evict->evict_queue_lock);

    /* Remove the reference from the eviction queues. */
    __evict_list_clear_page_locked(session, ref, false);

    __wt_spin_unlock(session, &evict->evict_queue_lock);
}

/*
 * __evict_queue_empty --
 *     Is the queue empty? Note that the eviction server is pessimistic and treats a half full queue
 *     as empty.
 */
static WT_INLINE bool
__evict_queue_empty(WTI_EVICT_QUEUE *queue, bool server_check)
{
    uint32_t candidates, used;

    if (queue->evict_current == NULL)
        return (true);

    /* The eviction server only considers half of the candidates. */
    candidates = queue->evict_candidates;
    if (server_check && candidates > 1)
        candidates /= 2;
    used = (uint32_t)(queue->evict_current - queue->evict_queue);
    return (used >= candidates);
}

/*
 * __evict_queue_full --
 *     Is the queue full (i.e., it has been populated with candidates and none of them have been
 *     evicted yet)?
 */
static WT_INLINE bool
__evict_queue_full(WTI_EVICT_QUEUE *queue)
{
    return (queue->evict_current == queue->evict_queue && queue->evict_candidates != 0);
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
     * waiting for the first file to drain from the eviction queue. See WT-5946 for details.
     */
    WT_ERR(__wt_curhs_cache(session));
    if (__wt_atomic_loadbool(&conn->evict_server_running) &&
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
        was_intr = __wt_atomic_loadv32(&evict->pass_intr) != 0;
        __wt_spin_unlock(session, &evict->evict_pass_lock);
        WT_ERR(ret);

        /*
         * If the eviction server was interrupted, wait until requests have been processed: the
         * system may otherwise be busy so don't go to sleep.
         */
        if (was_intr)
            while (__wt_atomic_loadv32(&evict->pass_intr) != 0 &&
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
        WT_ERR(__evict_lru_pages(session, false));

    if (0) {
err:
        WT_RET_PANIC(session, ret, "eviction thread error");
    }
    return (ret);
}

/*
 * __evict_set_saved_walk_tree --
 *     Set saved walk tree maintaining use count. Call it with NULL to clear the saved walk tree.
 */
static void
__evict_set_saved_walk_tree(WT_SESSION_IMPL *session, WT_DATA_HANDLE *new_dhandle)
{
    WT_DATA_HANDLE *old_dhandle;
    WT_EVICT *evict;

    evict = S2C(session)->evict;
    old_dhandle = evict->walk_tree;

    if (old_dhandle == new_dhandle)
        return;

    if (new_dhandle != NULL)
        (void)__wt_atomic_addi32(&new_dhandle->session_inuse, 1);

    evict->walk_tree = new_dhandle;

    if (old_dhandle != NULL) {
        WT_ASSERT(session, __wt_atomic_loadi32(&old_dhandle->session_inuse) > 0);
        (void)__wt_atomic_subi32(&old_dhandle->session_inuse, 1);
    }
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
    WTI_WITH_PASS_LOCK(session, ret = __evict_clear_all_walks_and_saved_tree(session));
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
    evict->use_npos_in_pass = __wt_atomic_loadbool(&conn->evict_use_npos);

    /* Evict pages from the cache as needed. */
    WT_RET(__evict_pass(session));

    if (!FLD_ISSET(conn->server_flags, WT_CONN_SERVER_EVICTION) ||
      __wt_atomic_loadv32(&evict->pass_intr) != 0)
        return (0);

    if (!__wt_evict_cache_stuck(session)) {
        if (evict->use_npos_in_pass)
            __evict_set_saved_walk_tree(session, NULL);
        else {
            /*
             * Try to get the handle list lock: if we give up, that indicates a session is waiting
             * for us to clear walks. Do that as part of a normal pass (without the handle list
             * lock) to avoid deadlock.
             */
            if ((ret = __evict_lock_handle_list(session)) == EBUSY)
                return (0);
            WT_RET(ret);

            /*
             * Clear the walks so we don't pin pages while asleep, otherwise we can block
             * applications evicting large pages.
             */
            ret = __evict_clear_all_walks_and_saved_tree(session);

            __wt_readunlock(session, &conn->dhandle_lock);
            WT_RET(ret);
        }
        /* Make sure we'll notice next time we're stuck. */
        evict->last_eviction_progress = 0;
        return (0);
    }

    /* Track if work was done. */
    *did_work = __wt_atomic_loadv64(&evict->eviction_progress) != evict->last_eviction_progress;
    evict->last_eviction_progress = __wt_atomic_loadv64(&evict->eviction_progress);

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

    WT_ASSERT(session, conn->evict_threads_min > 0);
    /* Set first, the thread might run before we finish up. */
    FLD_SET(conn->server_flags, WT_CONN_SERVER_EVICTION);

    /*
     * Create the eviction thread group. Set the group size to the maximum allowed sessions.
     */
    session_flags = WT_THREAD_CAN_WAIT | WT_THREAD_PANIC_FAIL;
    WT_RET(__wt_thread_group_create(session, &conn->evict_threads, "eviction-server",
      conn->evict_threads_min, conn->evict_threads_max, session_flags, __evict_thread_chk,
      __evict_thread_run, __evict_thread_stop));

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
    __wt_atomic_storebool(&conn->evict_server_running, true);

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
    if (!__wt_atomic_loadbool(&conn->evict_server_running))
        return (0);

    __wt_verbose_info(session, WT_VERB_EVICTION, "%s", "stopping eviction threads");

    /* Wait for any eviction thread group changes to stabilize. */
    __wt_writelock(session, &conn->evict_threads.lock);

    /*
     * Signal the threads to finish and stop populating the queue.
     */
    FLD_CLR(conn->server_flags, WT_CONN_SERVER_EVICTION);
    __wt_atomic_storebool(&conn->evict_server_running, false);
    __wt_evict_server_wake(session);

    __wt_verbose_info(session, WT_VERB_EVICTION, "%s", "waiting for eviction threads to stop");

    /*
     * We call the destroy function still holding the write lock. It assumes it is called locked.
     */
    WT_RET(__wt_thread_group_destroy(session, &conn->evict_threads));

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
    dirty_trigger = evict->eviction_dirty_trigger;
    target = evict->eviction_target;
    trigger = evict->eviction_trigger;
    updates_target = evict->eviction_updates_target;
    updates_trigger = evict->eviction_updates_trigger;

    /* Build up the new state. */
    flags = 0;

    if (!FLD_ISSET(conn->server_flags, WT_CONN_SERVER_EVICTION)) {
        __wt_atomic_store32(&evict->flags, 0);
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
                total_inmem += __wt_atomic_load64(&hs_tree->bytes_inmem);
                total_dirty += __wt_atomic_load64(&hs_tree->bytes_dirty_intl) +
                  __wt_atomic_load64(&hs_tree->bytes_dirty_leaf);
                total_updates += __wt_atomic_load64(&hs_tree->bytes_updates);
            } else {
                if (hs_id == WT_HS_ID)
                    WT_STAT_CONN_INCR(session, cache_eviction_hs_cursor_not_cached);
                else if (hs_id == WT_HS_ID_SHARED)
                    WT_STAT_CONN_INCR(session, cache_eviction_hs_shared_cursor_not_cached);
            }
        }
        __wt_atomic_store64(&cache->bytes_hs, total_inmem);
        __wt_atomic_store64(&cache->bytes_hs_dirty, total_dirty);
        __wt_atomic_store64(&cache->bytes_hs_updates, total_updates);
    }

    /*
     * If we need space in the cache, try to find clean pages to evict.
     *
     * Avoid division by zero if the cache size has not yet been set in a shared cache.
     */
    bytes_max = conn->cache_size + 1;
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
     * If application threads are blocked by the total volume of data in cache, try dirty pages as
     * well.
     */
    if (__wt_evict_aggressive(session) && LF_ISSET(WT_EVICT_CACHE_CLEAN_HARD))
        LF_SET(WT_EVICT_CACHE_DIRTY);

    /*
     * Configure scrub - which reinstates clean equivalents of reconciled dirty pages. This is
     * useful because an evicted dirty page isn't necessarily a good proxy for knowing if the page
     * will be accessed again soon. Be more aggressive about scrubbing in disaggregated storage
     * because the cost of retrieving a recently reconciled page is higher in that configuration. In
     * the local storage case scrub dirty pages and keep them in cache if we are less than half way
     * to the clean, dirty and updates triggers.
     *
     * There's an experimental flag WT_CACHE_EVICT_SCRUB_UNDER_TARGET that can be turned on to
     * enable scrub eviction as long as cache usage overall is under half way to the trigger limit.
     */
    if (__wt_conn_is_disagg(session) && bytes_inuse < (uint64_t)(trigger * bytes_max) / 100)
        LF_SET(WT_EVICT_CACHE_SCRUB);
    else if (bytes_inuse < (uint64_t)((target + trigger) * bytes_max) / 200) {
        if (F_ISSET_ATOMIC_32(
              &(conn->cache->cache_eviction_controls), WT_CACHE_EVICT_SCRUB_UNDER_TARGET)) {
            LF_SET(WT_EVICT_CACHE_SCRUB);
        } else if (bytes_dirty < (uint64_t)((dirty_target + dirty_trigger) * bytes_max) / 200 &&
          bytes_updates < (uint64_t)((updates_target + updates_trigger) * bytes_max) / 200) {
            LF_SET(WT_EVICT_CACHE_SCRUB);
        }

    } else
        LF_SET(WT_EVICT_CACHE_NOKEEP);

    if (FLD_ISSET(conn->debug_flags, WT_CONN_DEBUG_UPDATE_RESTORE_EVICT)) {
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
    __wt_atomic_store32(&evict->flags, flags);

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
    eviction_progress = __wt_atomic_loadv64(&evict->eviction_progress);
    prev_oldest_id = __wt_atomic_loadv64(&txn_global->oldest_id);

    /* Evict pages from the cache. */
    for (loop = 0; __wt_atomic_loadv32(&evict->pass_intr) == 0; loop++) {
        time_now = __wt_clock(session);
        if (loop == 0)
            time_prev = time_now;

        __evict_tune_workers(session);
        /*
         * Increment the shared read generation. Do this occasionally even if eviction is not
         * currently required, so that pages have some relative read generation when the eviction
         * server does need to do some work.
         */
        __wt_atomic_add64(&evict->read_gen, 1);
        __wt_atomic_add64(&evict->evict_pass_gen, 1);

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
          conn->cache_size, __wt_atomic_load64(&cache->bytes_inmem),
          __wt_atomic_load64(&cache->bytes_dirty_intl) +
            __wt_atomic_load64(&cache->bytes_dirty_leaf),
          __wt_atomic_load64(&cache->bytes_updates));

        if (F_ISSET(evict, WT_EVICT_CACHE_ALL))
            WT_RET(__evict_lru_walk(session));

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
            WT_RET(__evict_lru_pages(session, true));

        if (__wt_atomic_loadv32(&evict->pass_intr) != 0)
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
        if (eviction_progress == __wt_atomic_loadv64(&evict->eviction_progress)) {
            if (WT_CLOCKDIFF_MS(time_now, time_prev) >= 20 && F_ISSET(evict, WT_EVICT_CACHE_HARD)) {
                if (__wt_atomic_load32(&evict->evict_aggressive_score) < WT_EVICT_SCORE_MAX)
                    (void)__wt_atomic_addv32(&evict->evict_aggressive_score, 1);
                oldest_id = __wt_atomic_loadv64(&txn_global->oldest_id);
                if (prev_oldest_id == oldest_id &&
                  __wt_atomic_loadv64(&txn_global->current) != oldest_id &&
                  __wt_atomic_load32(&evict->evict_aggressive_score) < WT_EVICT_SCORE_MAX)
                    (void)__wt_atomic_addv32(&evict->evict_aggressive_score, 1);
                time_prev = time_now;
                prev_oldest_id = oldest_id;
            }

            /*
             * Keep trying for long enough that we should be able to evict a page if the server
             * isn't interfering.
             */
            if (loop < 100 ||
              __wt_atomic_load32(&evict->evict_aggressive_score) < WT_EVICT_SCORE_MAX) {
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
        eviction_progress = __wt_atomic_loadv64(&evict->eviction_progress);
    }
    return (0);
}

/*
 * __evict_clear_walk --
 *     Clear a single walk point and remember its position as a soft pointer if clear_pos is unset.
 */
static int
__evict_clear_walk(WT_SESSION_IMPL *session, bool clear_pos)
{
    WT_BTREE *btree;
    WT_DECL_RET;
    WT_EVICT *evict;
    WT_REF *ref;
#define PATH_STR_MAX 1024
    char path_str[PATH_STR_MAX];
    const char *where;
    size_t path_str_offset;
    double pos;

    btree = S2BT(session);
    evict = S2C(session)->evict;

    WT_ASSERT(session, FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_PASS));

    if ((ref = btree->evict_ref) == NULL)
        return (0);

    if (!evict->use_npos_in_pass || clear_pos)
        WT_STAT_CONN_INCR(session, eviction_walks_abandoned);

    /*
     * Clear evict_ref before releasing it in case that forces eviction (we assert that we never try
     * to evict the current eviction walk point).
     */
    btree->evict_ref = NULL;

    if (evict->use_npos_in_pass) {
        /* If soft pointers are in use, remember the page's position unless clear_pos is set. */
        if (clear_pos)
            __wt_evict_clear_npos(btree);
        else {
            /*
             * Remember the last position before clearing it so that we can restart from about the
             * same point later. evict_saved_ref_check is used as an opaque page id to compare with
             * it upon restoration for the purpose of stats.
             */
            btree->evict_saved_ref_check = (uint64_t)ref;

            if (F_ISSET(ref, WT_REF_FLAG_LEAF)) {
                /* If we're at a leaf page, use the middle of the page. */
                pos = WT_NPOS_MID;
                where = "MIDDLE";
            } else {
                /*
                 * If we're at an internal page, then we've just finished all its leafs, so get the
                 * position of the very beginning or the very end of it depending on the direction
                 * of walk.
                 */
                if (btree->evict_start_type == WT_EVICT_WALK_NEXT ||
                  btree->evict_start_type == WT_EVICT_WALK_RAND_NEXT) {
                    pos = WT_NPOS_RIGHT;
                    where = "RIGHT";
                } else {
                    pos = WT_NPOS_LEFT;
                    where = "LEFT";
                }
            }
            if (!WT_VERBOSE_LEVEL_ISSET(session, WT_VERB_EVICTION, WT_VERBOSE_DEBUG_1))
                btree->evict_pos = __wt_page_npos(session, ref, pos, NULL, NULL, 0);
            else {
                btree->evict_pos =
                  __wt_page_npos(session, ref, pos, path_str, &path_str_offset, PATH_STR_MAX);
                __wt_verbose_debug1(session, WT_VERB_EVICTION,
                  "Evict walk point memorized at position %lf %s of %s page %s ref %p",
                  btree->evict_pos, where, F_ISSET(ref, WT_REF_FLAG_INTERNAL) ? "INTERNAL" : "LEAF",
                  path_str, (void *)ref);
            }
        }
    }

    WT_WITH_DHANDLE(evict->walk_session, session->dhandle,
      (ret = __wt_page_release(evict->walk_session, ref, WT_READ_NO_EVICT)));
    return (ret);
#undef PATH_STR_MAX
}

/*
 * __evict_clear_all_walks_and_saved_tree --
 *     Clear the eviction walk points for all files a session is waiting on.
 */
static int
__evict_clear_all_walks_and_saved_tree(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DATA_HANDLE *dhandle;
    WT_DECL_RET;

    conn = S2C(session);

    TAILQ_FOREACH (dhandle, &conn->dhqh, q)
        if (WT_DHANDLE_BTREE(dhandle))
            WT_WITH_DHANDLE(session, dhandle, WT_TRET(__evict_clear_walk(session, true)));
    __evict_set_saved_walk_tree(session, NULL);
    return (ret);
}

/*
 * __evict_clear_walk_and_saved_tree_if_current_locked --
 *     Clear single walk points and clear the walk tree if it's the current session's dhandle.
 */
static int
__evict_clear_walk_and_saved_tree_if_current_locked(WT_SESSION_IMPL *session)
{
    WT_ASSERT_SPINLOCK_OWNED(session, &S2C(session)->evict->evict_pass_lock);
    if (session->dhandle == S2C(session)->evict->walk_tree)
        __evict_set_saved_walk_tree(session, NULL);
    return (__evict_clear_walk(session, false));
}

/* !!!
 * __wt_evict_file_exclusive_on --
 *     Acquire exclusive access to a file/tree making it possible to evict the entire file using
 *     `__wt_evict_file`. It does this by incrementing the `evict_disabled` counter for a
 *     tree, which disables all other means of eviction (except file eviction).
 *
 *     For the incremented `evict_disabled` value, the eviction server skips walking this tree for
 *     eviction candidates, and force-evicting or queuing pages from this tree is not allowed.
 *
 *     It is called from multiple places in the code base, such as when initiating file eviction
 *     `__wt_evict_file` or when opening or closing trees.
 *
 *     Return an error code if unable to acquire necessary locks or clear the eviction queues.
 */
int
__wt_evict_file_exclusive_on(WT_SESSION_IMPL *session)
{
    WT_BTREE *btree;
    WT_DECL_RET;
    WT_EVICT *evict;
    WTI_EVICT_ENTRY *evict_entry;
    u_int elem, i, q;

    btree = S2BT(session);
    evict = S2C(session)->evict;

    /* Hold the walk lock to turn off eviction. */
    __wt_spin_lock(session, &evict->evict_walk_lock);
    if (++btree->evict_disabled > 1) {
        __wt_spin_unlock(session, &evict->evict_walk_lock);
        return (0);
    }

    __wt_verbose_debug1(session, WT_VERB_EVICTION, "obtained exclusive eviction lock on btree %s",
      btree->dhandle->name);

    /*
     * Special operations don't enable eviction, however the underlying command (e.g. verify) may
     * choose to turn on eviction. This falls outside of the typical eviction flow, and here
     * eviction may forcibly remove pages from the cache. Consequently, we may end up evicting
     * internal pages which still have child pages present on the pre-fetch queue. Remove any refs
     * still present on the pre-fetch queue so that they are not accidentally accessed in an invalid
     * way later on.
     */
    WT_ERR(__wt_conn_prefetch_clear_tree(session, false));

    /*
     * Ensure no new pages from the file will be queued for eviction after this point, then clear
     * any existing LRU eviction walk for the file.
     */
    (void)__wt_atomic_addv32(&evict->pass_intr, 1);
    WTI_WITH_PASS_LOCK(session, ret = __evict_clear_walk_and_saved_tree_if_current_locked(session));
    (void)__wt_atomic_subv32(&evict->pass_intr, 1);
    WT_ERR(ret);

    /*
     * The eviction candidate list might reference pages from the file, clear it. Hold the evict
     * lock to remove queued pages from a file.
     */
    __wt_spin_lock(session, &evict->evict_queue_lock);

    for (q = 0; q < WTI_EVICT_QUEUE_MAX; q++) {
        __wt_spin_lock(session, &evict->evict_queues[q].evict_lock);
        elem = evict->evict_queues[q].evict_max;
        for (i = 0, evict_entry = evict->evict_queues[q].evict_queue; i < elem; i++, evict_entry++)
            if (evict_entry->btree == btree)
                __evict_list_clear(session, evict_entry);
        __wt_spin_unlock(session, &evict->evict_queues[q].evict_lock);
    }

    __wt_spin_unlock(session, &evict->evict_queue_lock);

    /*
     * We have disabled further eviction: wait for concurrent LRU eviction activity to drain.
     */
    while (btree->evict_busy > 0)
        __wt_yield();

    if (0) {
err:
        --btree->evict_disabled;
    }
    __wt_spin_unlock(session, &evict->evict_walk_lock);
    return (ret);
}

/* !!!
 * __wt_evict_file_exclusive_off --
 *     Release exclusive access to a file/tree by decrementing the `evict_disabled` count
 *     back to zero, allowing eviction to proceed for the tree.
 *
 *     It is called from multiple places in the code where exclusive eviction access is no longer
 *     needed.
 */
void
__wt_evict_file_exclusive_off(WT_SESSION_IMPL *session)
{
    WT_BTREE *btree;

    btree = S2BT(session);

    /*
     * We have seen subtle bugs with multiple threads racing to turn eviction on/off. Make races
     * more likely in diagnostic builds.
     */
    WT_DIAGNOSTIC_YIELD;

/*
 * Atomically decrement the evict-disabled count, without acquiring the eviction walk-lock. We can't
 * acquire that lock here because there's a potential deadlock. When acquiring exclusive eviction
 * access, we acquire the eviction walk-lock and then the eviction's pass-intr lock. The current
 * eviction implementation can hold the pass-intr lock and call into this function (see WT-3303 for
 * the details), which might deadlock with another thread trying to get exclusive eviction access.
 */
#if defined(HAVE_DIAGNOSTIC)
    {
        int32_t v;

        WT_ASSERT(session, btree->evict_ref == NULL);
        v = __wt_atomic_subi32(&btree->evict_disabled, 1);
        WT_ASSERT(session, v >= 0);
    }
#else
    (void)__wt_atomic_subi32(&btree->evict_disabled, 1);
#endif
    __wt_verbose_debug1(session, WT_VERB_EVICTION, "released exclusive eviction lock on btree %s",
      btree->dhandle->name);
}

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
    if (conn->evict_threads_max == conn->evict_threads_min)
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
        thread_surplus = (int32_t)__wt_atomic_load32(&conn->evict_threads.current_threads) -
          (int32_t)conn->evict_threads_min;

        if (thread_surplus > 0)
            __wt_thread_group_stop_one(session, &conn->evict_threads);

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
    eviction_progress = __wt_atomic_loadv64(&evict->eviction_progress);

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
        evict->evict_tune_workers_best = __wt_atomic_load32(&conn->evict_threads.current_threads);
    }

    /*
     * Compare the current number of data points with the number needed variable. If they are equal,
     * we will check whether we are still going up on the performance curve, in which case we will
     * increase the number of needed data points, to provide opportunity for further increasing the
     * number of workers. Or we are past the inflection point on the curve, in which case we will go
     * back to the best observed number of workers and settle into a stable state.
     */
    if (evict->evict_tune_num_points >= evict->evict_tune_datapts_needed) {
        current_threads = __wt_atomic_load32(&conn->evict_threads.current_threads);
        if (evict->evict_tune_workers_best == current_threads &&
          current_threads < conn->evict_threads_max) {
            /*
             * Keep adding workers. We will check again at the next check point.
             */
            evict->evict_tune_datapts_needed += WT_MIN(EVICT_TUNE_DATAPT_MIN,
              (conn->evict_threads_max - current_threads) / EVICT_TUNE_BATCH);
        } else {
            /*
             * We are past the inflection point. Choose the best number of eviction workers observed
             * and settle into a stable state.
             */
            thread_surplus = (int32_t)__wt_atomic_load32(&conn->evict_threads.current_threads) -
              (int32_t)evict->evict_tune_workers_best;

            for (i = 0; i < thread_surplus; i++)
                __wt_thread_group_stop_one(session, &conn->evict_threads);

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
        cur_threads = (int32_t)__wt_atomic_load32(&conn->evict_threads.current_threads);
        target_threads = WT_MIN(cur_threads + EVICT_TUNE_BATCH, (int32_t)conn->evict_threads_max);
        /*
         * Start the new threads.
         */
        for (i = cur_threads; i < target_threads; ++i) {
            __wt_thread_group_start_one(session, &conn->evict_threads, false);
            __wt_verbose_debug1(session, WT_VERB_EVICTION, "%s", "added worker thread");
        }
        evict->evict_tune_last_action_time = current_time;
    }

done:
    evict->evict_tune_last_time = current_time;
    evict->evict_tune_progress_last = eviction_progress;
}

/*
 * __evict_lru_pages --
 *     Get pages from the LRU queue to evict.
 */
static int
__evict_lru_pages(WT_SESSION_IMPL *session, bool is_server)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_TRACK_OP_DECL;

    WT_TRACK_OP_INIT(session);
    conn = S2C(session);

    /*
     * Reconcile and discard some pages: EBUSY is returned if a page fails eviction because it's
     * unavailable, continue in that case.
     */
    while (FLD_ISSET(conn->server_flags, WT_CONN_SERVER_EVICTION) && ret == 0)
        if ((ret = __evict_page(session, is_server)) == EBUSY)
            ret = 0;

    /* If any resources are pinned, release them now. */
    WT_TRET(__wt_session_release_resources(session));

    /* If a worker thread found the queue empty, pause. */
    if (ret == WT_NOTFOUND && !is_server && FLD_ISSET(conn->server_flags, WT_CONN_SERVER_EVICTION))
        __wt_cond_wait(session, conn->evict_threads.wait_cond, 10 * WT_THOUSAND, NULL);

    WT_TRACK_OP_END(session);
    return (ret == WT_NOTFOUND ? 0 : ret);
}

/*
 * __evict_lru_walk --
 *     Add pages to the LRU queue to be evicted from cache.
 */
static int
__evict_lru_walk(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_EVICT *evict;
    WTI_EVICT_QUEUE *other_queue, *queue;
    WT_TRACK_OP_DECL;
    uint64_t read_gen_oldest;
    uint32_t candidates, entries;

    WT_TRACK_OP_INIT(session);
    conn = S2C(session);
    evict = conn->evict;

    /* Age out the score of how much the queue has been empty recently. */
    if (evict->evict_empty_score > 0)
        --evict->evict_empty_score;

    /* Fill the next queue (that isn't the urgent queue). */
    queue = evict->evict_fill_queue;
    other_queue = evict->evict_queues + (1 - (queue - evict->evict_queues));
    evict->evict_fill_queue = other_queue;

    /* If this queue is full, try the other one. */
    if (__evict_queue_full(queue) && !__evict_queue_full(other_queue))
        queue = other_queue;

    /*
     * If both queues are full and haven't been empty on recent refills, we're done.
     */
    if (__evict_queue_full(queue) && evict->evict_empty_score < WT_EVICT_SCORE_CUTOFF) {
        WT_STAT_CONN_INCR(session, eviction_queue_not_empty);
        goto err;
    }
    /*
     * If the queue we are filling is empty, pages are being requested faster than they are being
     * queued.
     */
    if (__evict_queue_empty(queue, false)) {
        if (F_ISSET(evict, WT_EVICT_CACHE_HARD))
            evict->evict_empty_score =
              WT_MIN(evict->evict_empty_score + WT_EVICT_SCORE_BUMP, WT_EVICT_SCORE_MAX);
        WT_STAT_CONN_INCR(session, eviction_queue_empty);
    } else
        WT_STAT_CONN_INCR(session, eviction_queue_not_empty);

    /*
     * Get some more pages to consider for eviction.
     *
     * If the walk is interrupted, we still need to sort the queue: the next walk assumes there are
     * no entries beyond WTI_EVICT_WALK_BASE.
     */
    if ((ret = __evict_walk(evict->walk_session, queue)) == EBUSY)
        ret = 0;
    WT_ERR_NOTFOUND_OK(ret, false);

    /* Sort the list into LRU order and restart. */
    __wt_spin_lock(session, &queue->evict_lock);

    /*
     * We have locked the queue: in the (unusual) case where we are filling the current queue, mark
     * it empty so that subsequent requests switch to the other queue.
     */
    if (queue == evict->evict_current_queue)
        queue->evict_current = NULL;

    entries = queue->evict_entries;
    /*
     * Style note: __wt_qsort is a macro that can leave a dangling else. Full curly braces are
     * needed here for the compiler.
     */
    if (FLD_ISSET(conn->debug_flags, WT_CONN_DEBUG_EVICT_AGGRESSIVE_MODE)) {
        __wt_qsort(queue->evict_queue, entries, sizeof(WTI_EVICT_ENTRY), __evict_lru_cmp_debug);
    } else {
        __wt_qsort(queue->evict_queue, entries, sizeof(WTI_EVICT_ENTRY), __evict_lru_cmp);
    }

    /* Trim empty entries from the end. */
    while (entries > 0 && queue->evict_queue[entries - 1].ref == NULL)
        --entries;

    /*
     * If we have more entries than the maximum tracked between walks, clear them. Do this before
     * figuring out how many of the entries are candidates so we never end up with more candidates
     * than entries.
     */
    while (entries > WTI_EVICT_WALK_BASE)
        __evict_list_clear(session, &queue->evict_queue[--entries]);

    queue->evict_entries = entries;

    if (entries == 0) {
        /*
         * If there are no entries, there cannot be any candidates. Make sure application threads
         * don't read past the end of the candidate list, or they may race with the next walk.
         */
        queue->evict_candidates = 0;
        queue->evict_current = NULL;
        __wt_spin_unlock(session, &queue->evict_lock);
        goto err;
    }

    /* Decide how many of the candidates we're going to try and evict. */
    if (__wt_evict_aggressive(session))
        queue->evict_candidates = entries;
    else {
        /*
         * Find the oldest read generation apart that we have in the queue, used to set the initial
         * value for pages read into the system. The queue is sorted, find the first "normal"
         * generation.
         */
        read_gen_oldest = WT_READGEN_START_VALUE;
        for (candidates = 0; candidates < entries; ++candidates) {
            WT_READ_ONCE(read_gen_oldest, queue->evict_queue[candidates].score);
            if (!__wti_evict_readgen_is_soon_or_wont_need(&read_gen_oldest))
                break;
        }

        /*
         * Take all candidates if we only gathered pages with an oldest
         * read generation set.
         *
         * We normally never take more than 50% of the entries but if
         * 50% of the entries were at the oldest read generation, take
         * all of them.
         */
        if (__wti_evict_readgen_is_soon_or_wont_need(&read_gen_oldest))
            queue->evict_candidates = entries;
        else if (candidates > entries / 2)
            queue->evict_candidates = candidates;
        else {
            /*
             * Take all of the urgent pages plus a third of ordinary candidates (which could be
             * expressed as WTI_EVICT_WALK_INCR / WTI_EVICT_WALK_BASE). In the steady state, we want
             * to get as many candidates as the eviction walk adds to the queue.
             *
             * That said, if there is only one entry, which is normal when populating an empty file,
             * don't exclude it.
             */
            queue->evict_candidates = 1 + candidates + ((entries - candidates) - 1) / 3;
            if (queue->evict_candidates > entries / 2)
                queue->evict_candidates = entries / 2;

            evict->read_gen_oldest = read_gen_oldest;
        }
    }

    WT_STAT_CONN_INCRV(session, eviction_pages_queued_post_lru, queue->evict_candidates);
    /*
     * Add stats about pages that have been queued.
     */
    for (candidates = 0; candidates < queue->evict_candidates; ++candidates) {
        WT_PAGE *page = queue->evict_queue[candidates].ref->page;
        if (__wt_page_is_modified(page))
            WT_STAT_CONN_DSRC_INCR(session, cache_eviction_pages_queued_dirty);
        else if (page->modify != NULL)
            WT_STAT_CONN_DSRC_INCR(session, cache_eviction_pages_queued_updates);
        else
            WT_STAT_CONN_DSRC_INCR(session, cache_eviction_pages_queued_clean);
    }
    queue->evict_current = queue->evict_queue;
    __wt_spin_unlock(session, &queue->evict_lock);

    /*
     * Signal any application or helper threads that may be waiting to help with eviction.
     */
    __wt_cond_signal(session, conn->evict_threads.wait_cond);

err:
    WT_TRACK_OP_END(session);
    return (ret);
}

/*
 * __evict_walk_choose_dhandle --
 *     Randomly select a dhandle for the next eviction walk
 */
static void
__evict_walk_choose_dhandle(WT_SESSION_IMPL *session, WT_DATA_HANDLE **dhandle_p)
{
    WT_CONNECTION_IMPL *conn;
    WT_DATA_HANDLE *dhandle;
    u_int dh_bucket_count, rnd_bucket, rnd_dh;

    conn = S2C(session);

    WT_ASSERT(session, __wt_rwlock_islocked(session, &conn->dhandle_lock));

#undef RANDOM_DH_SELECTION_ENABLED

#ifdef RANDOM_DH_SELECTION_ENABLED
    *dhandle_p = NULL;

    /*
     * If we don't have many dhandles, most hash buckets will be empty. Just pick a random dhandle
     * from the list in that case.
     */
    if (conn->dhandle_count < conn->dh_hash_size / 4) {
        rnd_dh = __wt_random(&session->rnd_random) % conn->dhandle_count;
        dhandle = TAILQ_FIRST(&conn->dhqh);
        for (; rnd_dh > 0; rnd_dh--)
            dhandle = TAILQ_NEXT(dhandle, q);
        *dhandle_p = dhandle;
        return;
    }

    /*
     * Keep picking up a random bucket until we find one that is not empty.
     */
    do {
        rnd_bucket = __wt_random(&session->rnd_random) & (conn->dh_hash_size - 1);
    } while ((dh_bucket_count = conn->dh_bucket_count[rnd_bucket]) == 0);

    /* We can't pick up an empty bucket with a non zero bucket count. */
    WT_ASSERT(session, !TAILQ_EMPTY(&conn->dhhash[rnd_bucket]));

    /* Pick a random dhandle in the chosen bucket. */
    rnd_dh = __wt_random(&session->rnd_random) % dh_bucket_count;
    dhandle = TAILQ_FIRST(&conn->dhhash[rnd_bucket]);
    for (; rnd_dh > 0; rnd_dh--)
        dhandle = TAILQ_NEXT(dhandle, hashq);
#else
    /* Just step through dhandles. */
    dhandle = *dhandle_p;
    if (dhandle != NULL)
        dhandle = TAILQ_NEXT(dhandle, q);
    if (dhandle == NULL)
        dhandle = TAILQ_FIRST(&conn->dhqh);

    WT_UNUSED(dh_bucket_count);
    WT_UNUSED(rnd_bucket);
    WT_UNUSED(rnd_dh);
#endif

    *dhandle_p = dhandle;
}

/*
 * __evict_btree_dominating_cache --
 *     Return if a single btree is occupying at least half of any of our target's cache usage.
 */
static WT_INLINE bool
__evict_btree_dominating_cache(WT_SESSION_IMPL *session, WT_BTREE *btree)
{
    WT_CACHE *cache;
    WT_EVICT *evict;
    uint64_t bytes_dirty;
    uint64_t bytes_max;

    cache = S2C(session)->cache;
    evict = S2C(session)->evict;
    bytes_max = S2C(session)->cache_size + 1;

    if (__wt_cache_bytes_plus_overhead(cache, __wt_atomic_load64(&btree->bytes_inmem)) >
      (uint64_t)(0.5 * evict->eviction_target * bytes_max) / 100)
        return (true);

    bytes_dirty =
      __wt_atomic_load64(&btree->bytes_dirty_intl) + __wt_atomic_load64(&btree->bytes_dirty_leaf);
    if (__wt_cache_bytes_plus_overhead(cache, bytes_dirty) >
      (uint64_t)(0.5 * evict->eviction_dirty_target * bytes_max) / 100)
        return (true);
    if (__wt_cache_bytes_plus_overhead(cache, __wt_atomic_load64(&btree->bytes_updates)) >
      (uint64_t)(0.5 * evict->eviction_updates_target * bytes_max) / 100)
        return (true);

    return (false);
}

/*
 * __evict_walk --
 *     Fill in the array by walking the next set of pages.
 */
static int
__evict_walk(WT_SESSION_IMPL *session, WTI_EVICT_QUEUE *queue)
{
    WT_BTREE *btree;
    WT_CACHE *cache;
    WT_CONNECTION_IMPL *conn;
    WT_DATA_HANDLE *dhandle;
    WT_DECL_RET;
    WT_EVICT *evict;
    WT_TRACK_OP_DECL;
    uint32_t evict_walk_period;
    u_int loop_count, max_entries, retries, slot, start_slot;
    u_int total_candidates;
    bool dhandle_list_locked;

    WT_TRACK_OP_INIT(session);

    conn = S2C(session);
    cache = conn->cache;
    evict = conn->evict;
    btree = NULL;
    dhandle = NULL;
    dhandle_list_locked = false;
    retries = 0;

    /*
     * Set the starting slot in the queue and the maximum pages added per walk.
     */
    start_slot = slot = queue->evict_entries;
    max_entries = WT_MIN(slot + WTI_EVICT_WALK_INCR, evict->evict_slots);

    /*
     * Another pathological case: if there are only a tiny number of candidate pages in cache, don't
     * put all of them on one queue.
     */
    total_candidates = (u_int)(F_ISSET(evict, WT_EVICT_CACHE_CLEAN | WT_EVICT_CACHE_UPDATES) ?
        __wt_cache_pages_inuse(cache) :
        __wt_atomic_load64(&cache->pages_dirty_leaf));
    max_entries = WT_MIN(max_entries, 1 + total_candidates / 2);

retry:
    loop_count = 0;
    while (slot < max_entries && loop_count++ < conn->dhandle_count) {
        /* We're done if shutting down or reconfiguring. */
        if (F_ISSET_ATOMIC_32(conn, WT_CONN_CLOSING | WT_CONN_RECONFIGURING))
            break;

        /*
         * If another thread is waiting on the eviction server to clear the walk point in a tree,
         * give up.
         */
        if (__wt_atomic_loadv32(&evict->pass_intr) != 0)
            WT_ERR(EBUSY);

        /*
         * Lock the dhandle list to find the next handle and bump its reference count to keep it
         * alive while we sweep.
         */
        if (!dhandle_list_locked) {
            WT_ERR(__evict_lock_handle_list(session));
            dhandle_list_locked = true;
        }

        if (dhandle == NULL) {
            /*
             * On entry, continue from wherever we got to in the scan last time through. If we don't
             * have a saved handle, pick one randomly from the list.
             */
            if ((dhandle = evict->walk_tree) != NULL)
                __evict_set_saved_walk_tree(session, NULL);
            else
                __evict_walk_choose_dhandle(session, &dhandle);
        } else {
            __evict_set_saved_walk_tree(session, NULL);
            __evict_walk_choose_dhandle(session, &dhandle);
        }

        /* If we couldn't find any dhandle, we're done. */
        if (dhandle == NULL)
            break;

        /* Ignore non-btree handles, or handles that aren't open. */
        if (!WT_DHANDLE_BTREE(dhandle) || !F_ISSET(dhandle, WT_DHANDLE_OPEN))
            continue;

        /* Skip files that don't allow eviction. */
        btree = dhandle->handle;
        if (btree->evict_disabled > 0) {
            WT_STAT_CONN_INCR(session, eviction_server_skip_trees_eviction_disabled);
            continue;
        }

        /*
         * Skip files that are checkpointing if we are only looking for dirty pages.
         */
        if (WT_BTREE_SYNCING(btree) &&
          !F_ISSET(evict, WT_EVICT_CACHE_CLEAN | WT_EVICT_CACHE_UPDATES)) {
            WT_STAT_CONN_INCR(session, eviction_server_skip_checkpointing_trees);
            continue;
        }

        /*
         * Skip files that are configured to stick in cache until we become aggressive.
         *
         * If the file is contributing heavily to our cache usage then ignore the "stickiness" of
         * its pages.
         */
        if (btree->evict_priority != 0 && !__wt_evict_aggressive(session) &&
          !__evict_btree_dominating_cache(session, btree)) {
            WT_STAT_CONN_INCR(session, eviction_server_skip_trees_stick_in_cache);
            continue;
        }

        if (!evict->use_npos_in_pass) {
            /*
             * Skip files if we have too many active walks.
             *
             * This used to be limited by the configured maximum number of hazard pointers per
             * session. Even though that ceiling has been removed, we need to test eviction with
             * huge numbers of active trees before allowing larger numbers of hazard pointers in the
             * walk session.
             */
            if (btree->evict_ref == NULL && session->hazards.num_active > WTI_EVICT_MAX_TREES) {
                WT_STAT_CONN_INCR(session, eviction_server_skip_trees_too_many_active_walks);
                continue;
            }
        }

        /*
         * If the cache walk flags have changed since the prior eviction pass on this tree then
         * reset the walk effectiveness tracking. Imagine a case where only dirty content has been
         * looked for and this tree doesn't have much dirty content. Then eviction starts looking
         * for clean content - this tree might be a cornucopia of good clean candidate pages.
         * Specific for disaggregated connections, where we are using WT_EVICT_MODIFY_COUNT_MIN and
         * WT_DIRTY_PAGE_LOW_PRESSURE_THRESHOLD values to change the priority for this heuristic.
         */
        if (__wt_conn_is_disagg(session) && btree->last_evict_walk_flags != evict->flags) {
            __wt_atomic_store32(&btree->evict_walk_period, 0);
            btree->last_evict_walk_flags = evict->flags;
        }

        /*
         * If we are filling the queue, skip files that haven't been useful in the past.
         */
        evict_walk_period = __wt_atomic_load32(&btree->evict_walk_period);
        if (evict_walk_period != 0 && btree->evict_walk_skips++ < evict_walk_period) {
            WT_STAT_CONN_INCR(session, eviction_server_skip_trees_not_useful_before);
            continue;
        }

        if (F_ISSET(btree, WT_BTREE_IN_MEMORY) && !F_ISSET(evict, WT_EVICT_CACHE_DIRTY))
            continue;

        btree->evict_walk_skips = 0;

        __evict_set_saved_walk_tree(session, dhandle);
        __wt_readunlock(session, &conn->dhandle_lock);
        dhandle_list_locked = false;

        /*
         * Re-check the "no eviction" flag, used to enforce exclusive access when a handle is being
         * closed.
         *
         * Only try to acquire the lock and simply continue if we fail; the lock is held while the
         * thread turning off eviction clears the tree's current eviction point, and part of the
         * process is waiting on this thread to acknowledge that action.
         *
         * If a handle is being discarded, it will still be marked open, but won't have a root page.
         */
        if (btree->evict_disabled == 0 && !__wt_spin_trylock(session, &evict->evict_walk_lock)) {
            if (btree->evict_disabled == 0 && btree->root.page != NULL) {
                WT_WITH_DHANDLE(
                  session, dhandle, ret = __evict_walk_tree(session, queue, max_entries, &slot));

                WT_ASSERT(session, __wt_session_gen(session, WT_GEN_SPLIT) == 0);
            }
            __wt_spin_unlock(session, &evict->evict_walk_lock);
            WT_ERR(ret);
            /*
             * If there is a checkpoint thread gathering handles, which means it is holding the
             * schema lock, then there is often contention on the evict walk lock with that thread.
             * If eviction is not in aggressive mode, sleep a bit to give the checkpoint thread a
             * chance to gather its handles.
             */
            if (F_ISSET_ATOMIC_32(conn, WT_CONN_CKPT_GATHER) && !__wt_evict_aggressive(session)) {
                __wt_sleep(0, 10);
                WT_STAT_CONN_INCR(session, eviction_walk_sleeps);
            }
        }
    }

    /*
     * Repeat the walks a few times if we don't find enough pages. Give up when we have some
     * candidates and we aren't finding more.
     */
    if (slot < max_entries &&
      (retries < 2 ||
        (retries < WT_RETRY_MAX && (slot == queue->evict_entries || slot > start_slot)))) {
        start_slot = slot;
        ++retries;
        goto retry;
    }

err:
    if (dhandle_list_locked)
        __wt_readunlock(session, &conn->dhandle_lock);

    /*
     * If we didn't find any entries on a walk when we weren't interrupted, let our caller know.
     */
    if (queue->evict_entries == slot && __wt_atomic_loadv32(&evict->pass_intr) == 0)
        ret = WT_NOTFOUND;

    queue->evict_entries = slot;
    WT_TRACK_OP_END(session);
    return (ret);
}

/*
 * __evict_push_candidate --
 *     Initialize a WTI_EVICT_ENTRY structure with a given page.
 */
static bool
__evict_push_candidate(
  WT_SESSION_IMPL *session, WTI_EVICT_QUEUE *queue, WTI_EVICT_ENTRY *evict_entry, WT_REF *ref)
{
    uint16_t new_flags, orig_flags;
    u_int slot;

    /*
     * Threads can race to queue a page (e.g., an ordinary LRU walk can race with a page being
     * queued for urgent eviction).
     */
    orig_flags = new_flags = ref->page->flags_atomic;
    FLD_SET(new_flags, WT_PAGE_EVICT_LRU);
    if (orig_flags == new_flags ||
      !__wt_atomic_cas16(&ref->page->flags_atomic, orig_flags, new_flags))
        return (false);

    /* Keep track of the maximum slot we are using. */
    slot = (u_int)(evict_entry - queue->evict_queue);
    if (slot >= queue->evict_max)
        queue->evict_max = slot + 1;

    if (evict_entry->ref != NULL)
        __evict_list_clear(session, evict_entry);

    evict_entry->btree = S2BT(session);
    evict_entry->ref = ref;
    evict_entry->score = __evict_entry_priority(session, ref);

    /* Adjust for size when doing dirty eviction. */
    if (F_ISSET(S2C(session)->evict, WT_EVICT_CACHE_DIRTY) &&
      evict_entry->score != WT_READGEN_EVICT_SOON && evict_entry->score != UINT64_MAX &&
      !__wt_page_is_modified(ref->page))
        evict_entry->score +=
          WT_MEGABYTE - WT_MIN(WT_MEGABYTE, __wt_atomic_loadsize(&ref->page->memory_footprint));

    return (true);
}

/*
 * __evict_walk_target --
 *     Calculate how many pages to queue for a given tree.
 */
static uint32_t
__evict_walk_target(WT_SESSION_IMPL *session)
{
    WT_CACHE *cache;
    WT_EVICT *evict;
    uint64_t btree_clean_inuse, btree_dirty_inuse, btree_updates_inuse, bytes_per_slot, cache_inuse;
    uint32_t target_pages, target_pages_clean, target_pages_dirty, target_pages_updates;
    bool want_tree;

    cache = S2C(session)->cache;
    evict = S2C(session)->evict;
    btree_clean_inuse = btree_dirty_inuse = btree_updates_inuse = 0;
    target_pages_clean = target_pages_dirty = target_pages_updates = 0;

/*
 * The minimum number of pages we should consider per tree.
 */
#define MIN_PAGES_PER_TREE 10

    /*
     * The target number of pages for this tree is proportional to the space it is taking up in
     * cache. Round to the nearest number of slots so we assign all of the slots to a tree filling
     * 99+% of the cache (and only have to walk it once).
     */
    if (F_ISSET(evict, WT_EVICT_CACHE_CLEAN)) {
        btree_clean_inuse = __wt_btree_bytes_evictable(session);
        cache_inuse = __wt_cache_bytes_inuse(cache);
        bytes_per_slot = 1 + cache_inuse / evict->evict_slots;
        target_pages_clean = (uint32_t)((btree_clean_inuse + bytes_per_slot / 2) / bytes_per_slot);
    }

    if (F_ISSET(evict, WT_EVICT_CACHE_DIRTY)) {
        btree_dirty_inuse = __wt_btree_dirty_leaf_inuse(session);
        cache_inuse = __wt_cache_dirty_leaf_inuse(cache);
        bytes_per_slot = 1 + cache_inuse / evict->evict_slots;
        target_pages_dirty = (uint32_t)((btree_dirty_inuse + bytes_per_slot / 2) / bytes_per_slot);
    }

    if (F_ISSET(evict, WT_EVICT_CACHE_UPDATES)) {
        btree_updates_inuse = __wt_btree_bytes_updates(session);
        cache_inuse = __wt_cache_bytes_updates(cache);
        bytes_per_slot = 1 + cache_inuse / evict->evict_slots;
        target_pages_updates =
          (uint32_t)((btree_updates_inuse + bytes_per_slot / 2) / bytes_per_slot);
    }

    target_pages = WT_MAX(target_pages_clean, target_pages_dirty);
    target_pages = WT_MAX(target_pages, target_pages_updates);

    /*
     * Walk trees with a small fraction of the cache in case there are so many trees that none of
     * them use enough of the cache to be allocated slots. Only skip a tree if it has no bytes of
     * interest.
     */
    if (target_pages == 0) {
        want_tree = (F_ISSET(evict, WT_EVICT_CACHE_CLEAN) && (btree_clean_inuse > 0)) ||
          (F_ISSET(evict, WT_EVICT_CACHE_DIRTY) && (btree_dirty_inuse > 0)) ||
          (F_ISSET(evict, WT_EVICT_CACHE_UPDATES) && (btree_updates_inuse > 0));

        if (!want_tree) {
            WT_STAT_CONN_INCR(session, eviction_server_skip_unwanted_tree);
            return (0);
        }
    }

    /*
     * There is some cost associated with walking a tree. If we're going to visit this tree, always
     * look for a minimum number of pages.
     */
    if (target_pages < MIN_PAGES_PER_TREE)
        target_pages = MIN_PAGES_PER_TREE;

    /* If the tree is dead, take a lot of pages. */
    if (F_ISSET(session->dhandle, WT_DHANDLE_DEAD))
        target_pages *= 10;

    return (target_pages);
}

/*
 * __evict_skip_dirty_candidate --
 *     Check if eviction should skip the dirty page.
 */
static WT_INLINE bool
__evict_skip_dirty_candidate(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    WT_CONNECTION_IMPL *conn;
    WT_TXN *txn;

    conn = S2C(session);
    txn = session->txn;

    /*
     * If the global transaction state hasn't changed since the last time we tried eviction, it's
     * unlikely we can make progress. This heuristic avoids repeated attempts to evict the same
     * page.
     */
    if (!__wt_page_evict_retry(session, page)) {
        WT_STAT_CONN_INCR(session, eviction_server_skip_pages_retry);
        return (true);
    }

    /*
     * If we are under cache pressure, allow evicting pages with newly committed updates to free
     * space. Otherwise, avoid doing that as it may thrash the cache.
     */
    if (F_ISSET(conn->evict, WT_EVICT_CACHE_DIRTY_HARD | WT_EVICT_CACHE_UPDATES_HARD) &&
      F_ISSET(txn, WT_TXN_HAS_SNAPSHOT)) {
        if (!__txn_visible_id(session, __wt_atomic_load64(&page->modify->update_txn)))
            return (true);
    } else if (__wt_atomic_load64(&page->modify->update_txn) >=
      __wt_atomic_loadv64(&conn->txn_global.last_running)) {
        WT_STAT_CONN_INCR(session, eviction_server_skip_pages_last_running);
        return (true);
    } else if (F_ISSET(conn, WT_CONN_PRECISE_CHECKPOINT)) {
        wt_timestamp_t pinned_stable_ts;
        __wt_txn_pinned_stable_timestamp(session, &pinned_stable_ts);
        if (__wt_atomic_load64(&page->modify->newest_commit_timestamp) > pinned_stable_ts) {
            WT_STAT_CONN_INCR(session, eviction_server_skip_pages_checkpoint_timestamp);
            return (true);
        }
    }

    /*
     * For pages that are getting random updates (often index pages), try not to reconcile them too
     * often. It makes better use of I/O if they accumulate more changes between reconciliations
     */
#define WT_EVICT_MODIFY_COUNT_MIN 15 /* Number of modifications since the prior reconciliation */
    /*
     * If the cache is dirty, but not under pressure skip pages with just a few modifications
     * hopefully they can accumulate more changes before being reconciled. The cache has low
     * pressure if cache usage is less than 90% of the eviction dirty trigger threshold. Currently
     * only for disaggregated storage.
     */
#define WT_DIRTY_PAGE_LOW_PRESSURE_THRESHOLD \
    0.9 /* Cache usage below 90% of the eviction trigger threshold is considered low pressure */
    if (__wt_conn_is_disagg(session) &&
      __wt_atomic_load32(&page->modify->page_state) < WT_EVICT_MODIFY_COUNT_MIN) {
        double pct_dirty = 0.0, pct_updates = 0.0;
        bool high_pressure = false;

        if (F_ISSET(conn->evict, WT_EVICT_CACHE_DIRTY)) {
            WT_IGNORE_RET(__wt_evict_dirty_needed(session, &pct_dirty));
            high_pressure = (pct_dirty >
              (conn->evict->eviction_dirty_trigger * WT_DIRTY_PAGE_LOW_PRESSURE_THRESHOLD));
        }

        if (!high_pressure && F_ISSET(conn->evict, WT_EVICT_CACHE_UPDATES)) {
            WT_IGNORE_RET(__wti_evict_updates_needed(session, &pct_updates));
            high_pressure = (pct_updates >
              (conn->evict->eviction_updates_trigger * WT_DIRTY_PAGE_LOW_PRESSURE_THRESHOLD));
        }

        if (!high_pressure)
            return (true);
    }
    return (false);
}

/*
 * __evict_get_target_pages --
 *     Calculate the target pages to add to the queue.
 */
static WT_INLINE uint32_t
__evict_get_target_pages(WT_SESSION_IMPL *session, u_int max_entries, uint32_t slot)
{
    WT_BTREE *btree;
    uint32_t remaining_slots, target_pages;

    btree = S2BT(session);

    /*
     * Figure out how many slots to fill from this tree. Note that some care is taken in the
     * calculation to avoid overflow.
     */
    remaining_slots = max_entries - slot;

    /*
     * For this handle, calculate the number of target pages to evict. If the number of target pages
     * is zero, then simply return early from this function.
     *
     * If the progress has not met the previous target, continue using the previous target.
     */
    target_pages = __evict_walk_target(session);

    if ((target_pages == 0) || btree->evict_walk_progress >= btree->evict_walk_target) {
        btree->evict_walk_target = target_pages;
        btree->evict_walk_progress = 0;
    }
    target_pages = btree->evict_walk_target - btree->evict_walk_progress;

    if (target_pages > remaining_slots)
        target_pages = remaining_slots;

    /*
     * Reduce the number of pages to be selected from btrees other than the history store (HS) if
     * the cache pressure is high and HS content dominates the cache. Evicting unclean non-HS pages
     * can generate even more HS content and will not help with the cache pressure, and will
     * probably just amplify it further.
     */
    if (!WT_IS_HS(btree->dhandle) && __wti_evict_hs_dirty(session)) {
        /* If target pages are less than 10, keep it like that. */
        if (target_pages >= 10) {
            target_pages = target_pages / 10;
            WT_STAT_CONN_DSRC_INCR(session, cache_eviction_target_page_reduced);
        }
    }

    if (target_pages != 0) {
        /*
         * These statistics generate a histogram of the number of pages targeted for eviction each
         * round. The range of values here start at MIN_PAGES_PER_TREE as this is the smallest
         * number of pages we can target, unless there are fewer slots available. The aim is to
         * cover the likely ranges of target pages in as few statistics as possible to reduce the
         * overall overhead.
         */
        if (target_pages < MIN_PAGES_PER_TREE) {
            WT_STAT_CONN_INCR(session, cache_eviction_target_page_lt10);
            WT_STAT_DSRC_INCR(session, cache_eviction_target_page_lt10);
        } else if (target_pages < 32) {
            WT_STAT_CONN_INCR(session, cache_eviction_target_page_lt32);
            WT_STAT_DSRC_INCR(session, cache_eviction_target_page_lt32);
        } else if (target_pages < 64) {
            WT_STAT_CONN_INCR(session, cache_eviction_target_page_lt64);
            WT_STAT_DSRC_INCR(session, cache_eviction_target_page_lt64);
        } else if (target_pages < 128) {
            WT_STAT_CONN_INCR(session, cache_eviction_target_page_lt128);
            WT_STAT_DSRC_INCR(session, cache_eviction_target_page_lt128);
        } else {
            WT_STAT_CONN_INCR(session, cache_eviction_target_page_ge128);
            WT_STAT_DSRC_INCR(session, cache_eviction_target_page_ge128);
        }
    }

    return (target_pages);
}

/*
 * __evict_get_min_pages --
 *     Calculate the minimum pages to visit.
 */
static WT_INLINE uint64_t
__evict_get_min_pages(WT_SESSION_IMPL *session, uint32_t target_pages)
{
    WT_EVICT *evict;
    uint64_t min_pages;

    evict = S2C(session)->evict;

    /*
     * Examine at least a reasonable number of pages before deciding whether to give up. When we are
     * not looking for clean pages, search the tree for longer.
     */
    min_pages = 10 * (uint64_t)target_pages;
    if (F_ISSET(evict, WT_EVICT_CACHE_CLEAN))
        WT_STAT_CONN_INCR(session, eviction_target_strategy_clean);
    else
        min_pages *= 10;
    if (F_ISSET(evict, WT_EVICT_CACHE_UPDATES))
        WT_STAT_CONN_INCR(session, eviction_target_strategy_updates);
    if (F_ISSET(evict, WT_EVICT_CACHE_DIRTY))
        WT_STAT_CONN_INCR(session, eviction_target_strategy_dirty);

    return (min_pages);
}

/*
 * __evict_try_restore_walk_position --
 *     Try to restore the walk position from saved soft pos. Returns true if the walk position is
 *     restored.
 */
static WT_INLINE int
__evict_try_restore_walk_position(WT_SESSION_IMPL *session, WT_BTREE *btree, uint32_t walk_flags)
{
#define PATH_STR_MAX 1024
    char path_str[PATH_STR_MAX];
    size_t path_str_offset;
    double unused; /* GCC fails to WT_UNUSED() :( */

    if (btree->evict_ref != NULL)
        return (0); /* We've got a pointer already */
    if (WT_NPOS_IS_INVALID(btree->evict_pos))
        return (0); /* No restore point */
    WT_RET_ONLY(
      __wt_page_from_npos_for_eviction(session, &btree->evict_ref, btree->evict_pos, 0, walk_flags),
      WT_PANIC);

    if (btree->evict_ref != NULL &&
      WT_VERBOSE_LEVEL_ISSET(session, WT_VERB_EVICTION, WT_VERBOSE_DEBUG_1)) {
        WT_UNUSED(unused = __wt_page_npos(session, btree->evict_ref, WT_NPOS_MID, path_str,
                    &path_str_offset, PATH_STR_MAX));
        __wt_verbose_debug1(session, WT_VERB_EVICTION,
          "Evict walk point recalled from position %lf %s page %s ref %p", btree->evict_pos,
          F_ISSET(btree->evict_ref, WT_REF_FLAG_INTERNAL) ? "INTERNAL" : "LEAF", path_str,
          (void *)btree->evict_ref);
    }

    WT_STAT_CONN_INCR(session, eviction_restored_pos);
    if (btree->evict_saved_ref_check != 0 &&
      btree->evict_saved_ref_check != (uint64_t)btree->evict_ref)
        WT_STAT_CONN_INCR(session, eviction_restored_pos_differ);

    return (0);
#undef PATH_STR_MAX
}

/*
 * __evict_walk_prepare --
 *     Choose the walk direction and descend to the initial walk point.
 */
static WT_INLINE int
__evict_walk_prepare(WT_SESSION_IMPL *session, uint32_t *walk_flagsp)
{
    WT_BTREE *btree;
    WT_DECL_RET;

    btree = S2BT(session);

    *walk_flagsp = WT_READ_EVICT_WALK_FLAGS;
    if (!F_ISSET(session->txn, WT_TXN_HAS_SNAPSHOT))
        FLD_SET(*walk_flagsp, WT_READ_VISIBLE_ALL);

    WT_RET(__evict_try_restore_walk_position(session, btree, *walk_flagsp));

    if (btree->evict_ref != NULL)
        WT_STAT_CONN_INCR(session, eviction_walk_saved_pos);
    else
        WT_STAT_CONN_INCR(session, eviction_walk_from_root);

    /*
     * Choose a random point in the tree if looking for candidates in a tree with no starting point
     * set. This is mostly aimed at ensuring eviction fairly visits all pages in trees with a lot of
     * in-cache content.
     */
    switch (btree->evict_start_type) {
    case WT_EVICT_WALK_NEXT:
        /* Each time when evict_ref is null, alternate between linear and random walk */
        if (!S2C(session)->evict_legacy_page_visit_strategy && btree->evict_ref == NULL &&
          (++btree->linear_walk_restarts) & 1) {
            if (S2C(session)->evict->use_npos_in_pass)
                /* Alternate with rand_prev so that the start of the tree is visited more often */
                goto rand_prev;
            else
                goto rand_next;
        }
        break;
    case WT_EVICT_WALK_PREV:
        /* Each time when evict_ref is null, alternate between linear and random walk */
        if (!S2C(session)->evict_legacy_page_visit_strategy && btree->evict_ref == NULL &&
          (++btree->linear_walk_restarts) & 1) {
            if (S2C(session)->evict->use_npos_in_pass)
                /* Alternate with rand_next so that the end of the tree is visited more often */
                goto rand_next;
            else
                goto rand_prev;
        }
        FLD_SET(*walk_flagsp, WT_READ_PREV);
        break;
    case WT_EVICT_WALK_RAND_PREV:
rand_prev:
        FLD_SET(*walk_flagsp, WT_READ_PREV);
    /* FALLTHROUGH */
    case WT_EVICT_WALK_RAND_NEXT:
rand_next:
        if (btree->evict_ref == NULL) {
            for (;;) {
                /* Ensure internal pages indexes remain valid */
                WT_WITH_PAGE_INDEX(session,
                  ret = __wt_random_descent(
                    session, &btree->evict_ref, WT_READ_EVICT_READ_FLAGS, &session->rnd_random));
                if (ret != WT_RESTART)
                    break;
                WT_STAT_CONN_INCR(session, eviction_walk_restart);
            }
            WT_RET_NOTFOUND_OK(ret);

            if (btree->evict_ref == NULL)
                WT_STAT_CONN_INCR(session, eviction_walk_random_returns_null_position);
        }
        break;
    }

    return (ret);
}

/*
 * __evict_should_give_up_walk --
 *     Check if we should give up on the current walk.
 */
static WT_INLINE bool
__evict_should_give_up_walk(WT_SESSION_IMPL *session, uint64_t pages_seen, uint64_t pages_queued,
  uint64_t min_pages, uint32_t target_pages)
{
    WT_BTREE *btree;
    bool give_up;

    btree = S2BT(session);

    /*
     * Check whether we're finding a good ratio of candidates vs pages seen. Some workloads create
     * "deserts" in trees where no good eviction candidates can be found. Abandon the walk if we get
     * into that situation.
     */
    give_up = !__wt_evict_aggressive(session) && !WT_IS_HS(btree->dhandle) &&
      pages_seen > min_pages &&
      (pages_queued == 0 || (pages_seen / pages_queued) > (min_pages / target_pages));
    if (give_up) {
        /*
         * Try a different walk start point next time if a walk gave up.
         */
        switch (btree->evict_start_type) {
        case WT_EVICT_WALK_NEXT:
            btree->evict_start_type = WT_EVICT_WALK_PREV;
            break;
        case WT_EVICT_WALK_PREV:
            btree->evict_start_type = WT_EVICT_WALK_RAND_PREV;
            break;
        case WT_EVICT_WALK_RAND_PREV:
            btree->evict_start_type = WT_EVICT_WALK_RAND_NEXT;
            break;
        case WT_EVICT_WALK_RAND_NEXT:
            btree->evict_start_type = WT_EVICT_WALK_NEXT;
            break;
        }

        /*
         * We differentiate the reasons we gave up on this walk and increment the stats accordingly.
         */
        if (pages_queued == 0)
            WT_STAT_CONN_INCR(session, eviction_walks_gave_up_no_targets);
        else
            WT_STAT_CONN_INCR(session, eviction_walks_gave_up_ratio);
    }

    return (give_up);
}

/*
 * __evict_try_queue_page --
 *     Check if we should queue the page for eviction. Queue it to the urgent queue or the regular
 *     queue.
 */
static WT_INLINE void
__evict_try_queue_page(WT_SESSION_IMPL *session, WTI_EVICT_QUEUE *queue, WT_REF *ref,
  WT_PAGE *last_parent, WTI_EVICT_ENTRY *evict_entry, bool *urgent_queuedp, bool *queuedp)
{
    WT_BTREE *btree;
    WT_CONNECTION_IMPL *conn;
    WT_EVICT *evict;
    WT_PAGE *page;
    bool modified, want_page;

    btree = S2BT(session);
    conn = S2C(session);
    evict = conn->evict;
    page = ref->page;
    modified = __wt_page_is_modified(page);
    *queuedp = false;

    /* Don't queue dirty pages in trees during checkpoints. */
    if (modified && WT_BTREE_SYNCING(btree)) {
        WT_STAT_CONN_INCR(session, eviction_server_skip_dirty_pages_during_checkpoint);
        return;
    }

    /*
     * It's possible (but unlikely) to visit a page without a read generation, if we race with the
     * read instantiating the page. Set the page's read generation here to ensure a bug doesn't
     * somehow leave a page without a read generation.
     */
    if (__wt_atomic_load64(&page->read_gen) == WT_READGEN_NOTSET)
        __wti_evict_read_gen_new(session, page);

    /* Pages being forcibly evicted go on the urgent queue. */
    if (modified &&
      (__wt_atomic_load64(&page->read_gen) == WT_READGEN_EVICT_SOON ||
        __wt_atomic_loadsize(&page->memory_footprint) >= btree->splitmempage)) {
        WT_STAT_CONN_INCR(session, eviction_pages_queued_oldest);
        if (__wt_evict_page_urgent(session, ref))
            *urgent_queuedp = true;
        return;
    }

    /*
     * If history store dirty content is dominating the cache, we want to prioritize evicting
     * history store pages over other btree pages. This helps in keeping cache contents below the
     * configured cache size during checkpoints where reconciling non-HS pages can generate a
     * significant amount of HS dirty content very quickly.
     */
    if (WT_IS_HS(btree->dhandle) && __wti_evict_hs_dirty(session)) {
        WT_STAT_CONN_INCR(session, eviction_pages_queued_urgent_hs_dirty);
        if (__wt_evict_page_urgent(session, ref))
            *urgent_queuedp = true;
        return;
    }

    /* Pages that are empty or from dead trees are fast-tracked. */
    if (__wt_page_is_empty(page) || F_ISSET(session->dhandle, WT_DHANDLE_DEAD))
        goto fast;

    /* Skip pages we don't want. */
    want_page =
      (F_ISSET(evict, WT_EVICT_CACHE_CLEAN) && !F_ISSET(btree, WT_BTREE_IN_MEMORY) && !modified) ||
      (F_ISSET(evict, WT_EVICT_CACHE_DIRTY) && modified) ||
      (F_ISSET(evict, WT_EVICT_CACHE_UPDATES) && !F_ISSET(btree, WT_BTREE_IN_MEMORY) &&
        page->modify != NULL);
    if (!want_page) {
        WT_STAT_CONN_INCR(session, eviction_server_skip_unwanted_pages);
        return;
    }

    /*
     * Do not evict a clean metadata page that contains historical data needed to satisfy a reader.
     * Since there is no history store for metadata, we won't be able to serve an older reader if we
     * evict this page.
     */
    if (WT_IS_METADATA(session->dhandle) && F_ISSET(evict, WT_EVICT_CACHE_CLEAN_HARD) &&
      F_ISSET(ref, WT_REF_FLAG_LEAF) && !modified && page->modify != NULL &&
      !__wt_txn_visible_all(session, page->modify->rec_max_txn, page->modify->rec_max_timestamp)) {
        WT_STAT_CONN_INCR(session, eviction_server_skip_metatdata_with_history);
        return;
    }

    /*
     * Don't attempt eviction of internal pages with children in cache (indicated by seeing an
     * internal page that is the parent of the last page we saw).
     *
     * Also skip internal page unless we get aggressive, the tree is idle (indicated by the tree
     * being skipped for walks), or we are in eviction debug mode. The goal here is that if trees
     * become completely idle, we eventually push them out of cache completely.
     */
    if (!FLD_ISSET(conn->debug_flags, WT_CONN_DEBUG_EVICT_AGGRESSIVE_MODE) &&
      F_ISSET(ref, WT_REF_FLAG_INTERNAL)) {
        if (page == last_parent) {
            WT_STAT_CONN_INCR(session, eviction_server_skip_intl_page_with_active_child);
            return;
        }
        if (__wt_atomic_load32(&btree->evict_walk_period) == 0 && !__wt_evict_aggressive(session))
            return;
    }

    /* Evaluate dirty page candidacy, when eviction is not aggressive. */
    if (!__wt_evict_aggressive(session) && modified && __evict_skip_dirty_candidate(session, page))
        return;

fast:
    /* If the page can't be evicted, give up. */
    if (!__wt_page_can_evict(session, ref, NULL))
        return;

    WT_ASSERT(session, evict_entry->ref == NULL);
    if (!__evict_push_candidate(session, queue, evict_entry, ref))
        return;

    *queuedp = true;
    __wt_verbose_debug2(session, WT_VERB_EVICTION, "walk select: %p, size %" WT_SIZET_FMT,
      (void *)page, __wt_atomic_loadsize(&page->memory_footprint));

    return;
}

/*
 * __evict_walk_tree --
 *     Get a few page eviction candidates from a single underlying file.
 */
static int
__evict_walk_tree(WT_SESSION_IMPL *session, WTI_EVICT_QUEUE *queue, u_int max_entries, u_int *slotp)
{
    WT_BTREE *btree;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_EVICT *evict;
    WTI_EVICT_ENTRY *end, *evict_entry, *start;
    WT_PAGE *last_parent, *page;
    WT_REF *ref;
    WT_TXN *txn;
    uint64_t internal_pages_already_queued, internal_pages_queued, internal_pages_seen;
    uint64_t min_pages, pages_already_queued, pages_queued, pages_seen, refs_walked;
    uint64_t pages_seen_clean, pages_seen_dirty, pages_seen_updates;
    uint32_t evict_walk_period, target_pages, walk_flags;
    int restarts;
    bool give_up, queued, urgent_queued;

    conn = S2C(session);
    btree = S2BT(session);
    evict = conn->evict;
    last_parent = NULL;
    restarts = 0;
    give_up = urgent_queued = false;
    txn = session->txn;

    WT_ASSERT_SPINLOCK_OWNED(session, &evict->evict_walk_lock);

    start = queue->evict_queue + *slotp;
    target_pages = __evict_get_target_pages(session, max_entries, *slotp);

    /* If we don't want any pages from this tree, move on. */
    if (target_pages == 0)
        return (0);

    end = start + target_pages;

    min_pages = __evict_get_min_pages(session, target_pages);

    WT_RET_NOTFOUND_OK(__evict_walk_prepare(session, &walk_flags));

    /*
     * Get some more eviction candidate pages, starting at the last saved point. Clear the saved
     * point immediately, we assert when discarding pages we're not discarding an eviction point, so
     * this clear must be complete before the page is released.
     */
    ref = btree->evict_ref;
    btree->evict_ref = NULL;
    /* Clear the saved position just in case we never put it back. */
    __wt_evict_clear_npos(btree);

    /*
     * Get the snapshot for the eviction server when we want to evict dirty content under cache
     * pressure. This snapshot is used to check for the visibility of the last modified transaction
     * id on the page.
     */
    if (F_ISSET(evict, WT_EVICT_CACHE_DIRTY_HARD | WT_EVICT_CACHE_UPDATES_HARD))
        __wt_txn_bump_snapshot(session);

    /*
     * !!! Take care terminating this loop.
     *
     * Don't make an extra call to __wt_tree_walk after we hit the end of a
     * tree: that will leave a page pinned, which may prevent any work from
     * being done.
     *
     * Once we hit the page limit, do one more step through the walk in
     * case we are appending and only the last page in the file is live.
     */
    internal_pages_already_queued = internal_pages_queued = internal_pages_seen = 0;
    pages_seen_clean = pages_seen_dirty = pages_seen_updates = 0;
    for (evict_entry = start, pages_already_queued = pages_queued = pages_seen = refs_walked = 0;
         evict_entry < end && (ret == 0 || ret == WT_NOTFOUND);
         last_parent = ref == NULL ? NULL : ref->home,
        ret = __wt_tree_walk_count(session, &ref, &refs_walked, walk_flags)) {

        if ((give_up = __evict_should_give_up_walk(
               session, pages_seen, pages_queued, min_pages, target_pages)))
            break;

        if (ref == NULL) {
            WT_STAT_CONN_INCR(session, eviction_walks_ended);

            if (++restarts == 2) {
                WT_STAT_CONN_INCR(session, eviction_walks_stopped);
                break;
            }
            WT_STAT_CONN_INCR(session, eviction_walks_started);
            continue;
        }

        ++pages_seen;

        /* Ignore root pages entirely. */
        if (__wt_ref_is_root(ref))
            continue;

        page = ref->page;

        /*
         * Update the maximum evict pass generation gap seen at time of eviction. This helps track
         * how long it's been since a page was last queued for eviction. We need to update the
         * statistic here during the walk and not at __evict_page because the evict_pass_gen is
         * reset here.
         */
        const uint64_t gen_gap = __wt_atomic_load64(&evict->evict_pass_gen) - page->evict_pass_gen;
        if (gen_gap > __wt_atomic_load64(&evict->evict_max_gen_gap))
            __wt_atomic_store64(&evict->evict_max_gen_gap, gen_gap);

        page->evict_pass_gen = __wt_atomic_load64(&evict->evict_pass_gen);

        if (__wt_page_is_modified(page))
            ++pages_seen_dirty;
        else if (page->modify != NULL)
            ++pages_seen_updates;
        else
            ++pages_seen_clean;

        /* Count internal pages seen. */
        if (F_ISSET(ref, WT_REF_FLAG_INTERNAL))
            internal_pages_seen++;

        /* Use the EVICT_LRU flag to avoid putting pages onto the list multiple times. */
        if (F_ISSET_ATOMIC_16(page, WT_PAGE_EVICT_LRU)) {
            pages_already_queued++;
            if (F_ISSET(ref, WT_REF_FLAG_INTERNAL))
                internal_pages_already_queued++;
            continue;
        }

        __evict_try_queue_page(
          session, queue, ref, last_parent, evict_entry, &urgent_queued, &queued);

        if (queued) {
            ++evict_entry;
            ++pages_queued;
            ++btree->evict_walk_progress;

            /* Count internal pages queued. */
            if (F_ISSET(ref, WT_REF_FLAG_INTERNAL))
                internal_pages_queued++;
        }
    }
    if (F_ISSET(txn, WT_TXN_HAS_SNAPSHOT))
        __wt_txn_release_snapshot(session);
    WT_RET_NOTFOUND_OK(ret);

    *slotp += (u_int)(evict_entry - start);
    WT_STAT_CONN_INCRV(session, eviction_pages_ordinary_queued, (u_int)(evict_entry - start));

    __wt_verbose_debug2(session, WT_VERB_EVICTION,
      "%s walk: target %" PRIu32 ", seen %" PRIu64 ", queued %" PRIu64, session->dhandle->name,
      target_pages, pages_seen, pages_queued);

    /* If we couldn't find the number of pages we were looking for, skip the tree next time. */
    evict_walk_period = __wt_atomic_load32(&btree->evict_walk_period);
    if (pages_queued < target_pages / 2 && !urgent_queued)
        __wt_atomic_store32(
          &btree->evict_walk_period, WT_MIN(WT_MAX(1, 2 * evict_walk_period), 100));
    else if (pages_queued == target_pages) {
        __wt_atomic_store32(&btree->evict_walk_period, 0);
        /*
         * If there's a chance the Btree was fully evicted, update the evicted flag in the handle.
         */
        if (__wt_btree_bytes_evictable(session) == 0)
            FLD_SET(session->dhandle->advisory_flags, WT_DHANDLE_ADVISORY_EVICTED);
    } else if (evict_walk_period > 0)
        __wt_atomic_store32(&btree->evict_walk_period, evict_walk_period / 2);

    /*
     * Give up the walk occasionally.
     *
     * If we happen to end up on the root page or a page requiring urgent eviction, clear it. We
     * have to track hazard pointers, and the root page complicates that calculation.
     *
     * Likewise if we found no new candidates during the walk: there is no point keeping a page
     * pinned, since it may be the only candidate in an idle tree.
     *
     * If we land on a page requiring forced eviction, or that isn't an ordinary in-memory page,
     * move until we find an ordinary page: we should not prevent exclusive access to the page until
     * the next walk.
     */
    if (ref != NULL) {
        if (__wt_ref_is_root(ref) || evict_entry == start || give_up ||
          __wt_atomic_loadsize(&ref->page->memory_footprint) >= btree->splitmempage) {
            if (restarts == 0)
                WT_STAT_CONN_INCR(session, eviction_walks_abandoned);
            WT_RET(__wt_page_release(evict->walk_session, ref, walk_flags));
            ref = NULL;
        } else {
            while (ref != NULL &&
              (WT_REF_GET_STATE(ref) != WT_REF_MEM ||
                __wti_evict_readgen_is_soon_or_wont_need(&ref->page->read_gen)))
                WT_RET_NOTFOUND_OK(__wt_tree_walk_count(session, &ref, &refs_walked, walk_flags));
        }
        btree->evict_ref = ref;
        if (evict->use_npos_in_pass)
            __evict_clear_walk(session, false);
    }

    WT_STAT_CONN_INCRV(session, eviction_walk, refs_walked);
    WT_STAT_CONN_DSRC_INCRV(session, cache_eviction_pages_seen, pages_seen);
    WT_STAT_CONN_INCRV(session, eviction_pages_already_queued, pages_already_queued);
    WT_STAT_CONN_INCRV(session, eviction_internal_pages_seen, internal_pages_seen);
    WT_STAT_CONN_INCRV(
      session, eviction_internal_pages_already_queued, internal_pages_already_queued);
    WT_STAT_CONN_INCRV(session, eviction_internal_pages_queued, internal_pages_queued);
    WT_STAT_CONN_DSRC_INCR(session, eviction_walk_passes);
    WT_STAT_CONN_DSRC_INCRV(session, cache_eviction_pages_seen_clean, pages_seen_clean);
    WT_STAT_CONN_DSRC_INCRV(session, cache_eviction_pages_seen_dirty, pages_seen_dirty);
    WT_STAT_CONN_DSRC_INCRV(session, cache_eviction_pages_seen_updates, pages_seen_updates);
    return (0);
}

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

    __wt_spin_lock(session, &evict->evict_queue_lock);

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
        if (!is_server)
            __wt_spin_lock(session, &queue->evict_lock);
        else if (__wt_spin_trylock(session, &queue->evict_lock) != 0)
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
        (void)__wt_atomic_addv32(&evict_entry->btree->evict_busy, 1);

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
 * __evict_page --
 *     Called by both eviction and application threads to evict a page.
 */
static int
__evict_page(WT_SESSION_IMPL *session, bool is_server)
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
        S2C(session)->evict->app_evicts++;
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

    (void)__wt_atomic_subv32(&btree->evict_busy, 1);

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
 * The function returns an error code from either __evict_page or __wt_txn_is_blocking.
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
    for (uint64_t initial_progress = __wt_atomic_loadv64(&evict->eviction_progress);; ret = 0) {
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
        if (!busy && __wt_atomic_loadv64(&txn_shared->pinned_id) != WT_TXN_NONE &&
          __wt_atomic_loadv64(&txn_global->current) != __wt_atomic_loadv64(&txn_global->oldest_id))
            busy = true;
        uint64_t max_progress = busy ? 5 : 20;

        /* See if eviction is still needed. */
        double pct_full;
        if (!__wt_evict_needed(session, busy, readonly, &pct_full) ||
          (pct_full < 100.0 &&
            (__wt_atomic_loadv64(&evict->eviction_progress) > initial_progress + max_progress)))
            break;

        if (!__evict_check_user_ok_with_eviction(session, interruptible))
            break;

        /* Evict a page. */
        ret = __evict_page(session, false);
        if (ret == 0) {
            /* If the caller holds resources, we can stop after a successful eviction. */
            if (busy)
                break;
        } else if (ret == WT_NOTFOUND) {
            /* Allow the queue to re-populate before retrying. */
            __wt_cond_wait(session, conn->evict_threads.wait_cond, 10 * WT_THOUSAND, NULL);
            evict->app_waits++;
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
    if (S2BT(session)->evict_disabled > 0 || F_ISSET_ATOMIC_16(page, WT_PAGE_EVICT_LRU_URGENT))
        return (false);

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
            __evict_list_clear_page_locked(session, ref, true);
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
      __evict_push_candidate(session, urgent_queue, evict_entry, ref)) {
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
            __wt_cond_signal(session, S2C(session)->evict_threads.wait_cond);
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
 *     At present, it is exclusively called for metadata and bloom filter files, as these are meant
 *     to be retained in the cache.
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

/*
 * __verbose_dump_cache_single --
 *     Output diagnostic information about a single file in the cache.
 */
static int
__verbose_dump_cache_single(WT_SESSION_IMPL *session, uint64_t *total_bytesp,
  uint64_t *total_dirty_bytesp, uint64_t *total_updates_bytesp)
{
    WT_BTREE *btree;
    WT_DATA_HANDLE *dhandle;
    WT_PAGE *page;
    WT_REF *next_walk;
    size_t size;
    uint64_t intl_bytes, intl_bytes_max, intl_dirty_bytes;
    uint64_t intl_dirty_bytes_max, intl_dirty_pages, intl_pages;
    uint64_t leaf_bytes, leaf_bytes_max, leaf_dirty_bytes;
    uint64_t leaf_dirty_bytes_max, leaf_dirty_pages, leaf_pages, updates_bytes;

    intl_bytes = intl_bytes_max = intl_dirty_bytes = 0;
    intl_dirty_bytes_max = intl_dirty_pages = intl_pages = 0;
    leaf_bytes = leaf_bytes_max = leaf_dirty_bytes = 0;
    leaf_dirty_bytes_max = leaf_dirty_pages = leaf_pages = 0;
    updates_bytes = 0;

    dhandle = session->dhandle;
    btree = dhandle->handle;
    WT_RET(__wt_msg(session, "%s(%s%s)%s%s:", dhandle->name,
      WT_DHANDLE_IS_CHECKPOINT(dhandle) ? "checkpoint=" : "",
      WT_DHANDLE_IS_CHECKPOINT(dhandle) ? dhandle->checkpoint : "<live>",
      btree->evict_disabled != 0 ? " eviction disabled" : "",
      btree->evict_disabled_open ? " at open" : ""));

    /*
     * We cannot walk the tree of a dhandle held exclusively because the owning thread could be
     * manipulating it in a way that causes us to dump core. So print out that we visited and
     * skipped it.
     */
    if (F_ISSET(dhandle, WT_DHANDLE_EXCLUSIVE))
        return (__wt_msg(session, " handle opened exclusively, cannot walk tree, skipping"));

    next_walk = NULL;
    while (__wt_tree_walk(session, &next_walk,
             WT_READ_CACHE | WT_READ_NO_EVICT | WT_READ_NO_WAIT | WT_READ_VISIBLE_ALL) == 0 &&
      next_walk != NULL) {
        page = next_walk->page;
        size = __wt_atomic_loadsize(&page->memory_footprint);

        if (F_ISSET(next_walk, WT_REF_FLAG_INTERNAL)) {
            ++intl_pages;
            intl_bytes += size;
            intl_bytes_max = WT_MAX(intl_bytes_max, size);
            if (__wt_page_is_modified(page)) {
                ++intl_dirty_pages;
                intl_dirty_bytes += size;
                intl_dirty_bytes_max = WT_MAX(intl_dirty_bytes_max, size);
            }
        } else {
            ++leaf_pages;
            leaf_bytes += size;
            leaf_bytes_max = WT_MAX(leaf_bytes_max, size);
            if (__wt_page_is_modified(page)) {
                ++leaf_dirty_pages;
                leaf_dirty_bytes += size;
                leaf_dirty_bytes_max = WT_MAX(leaf_dirty_bytes_max, size);
            }
            if (page->modify != NULL)
                updates_bytes += page->modify->bytes_updates;
        }
    }

    if (intl_pages == 0)
        WT_RET(__wt_msg(session, "internal: 0 pages"));
    else
        WT_RET(
          __wt_msg(session,
            "internal: "
            "%" PRIu64 " pages, %.2f KB, "
            "%" PRIu64 "/%" PRIu64 " clean/dirty pages, "
            "%.2f/%.2f clean / dirty KB, "
            "%.2f KB max page, "
            "%.2f KB max dirty page ",
            intl_pages, (double)intl_bytes / WT_KILOBYTE, intl_pages - intl_dirty_pages,
            intl_dirty_pages, (double)(intl_bytes - intl_dirty_bytes) / WT_KILOBYTE,
            (double)intl_dirty_bytes / WT_KILOBYTE, (double)intl_bytes_max / WT_KILOBYTE,
            (double)intl_dirty_bytes_max / WT_KILOBYTE));
    if (leaf_pages == 0)
        WT_RET(__wt_msg(session, "leaf: 0 pages"));
    else
        WT_RET(
          __wt_msg(session,
            "leaf: "
            "%" PRIu64 " pages, %.2f KB, "
            "%" PRIu64 "/%" PRIu64 " clean/dirty pages, "
            "%.2f /%.2f /%.2f clean/dirty/updates KB, "
            "%.2f KB max page, "
            "%.2f KB max dirty page",
            leaf_pages, (double)leaf_bytes / WT_KILOBYTE, leaf_pages - leaf_dirty_pages,
            leaf_dirty_pages, (double)(leaf_bytes - leaf_dirty_bytes) / WT_KILOBYTE,
            (double)leaf_dirty_bytes / WT_KILOBYTE, (double)updates_bytes / WT_KILOBYTE,
            (double)leaf_bytes_max / WT_KILOBYTE, (double)leaf_dirty_bytes_max / WT_KILOBYTE));

    *total_bytesp += intl_bytes + leaf_bytes;
    *total_dirty_bytesp += intl_dirty_bytes + leaf_dirty_bytes;
    *total_updates_bytesp += updates_bytes;

    return (0);
}

/*
 * __verbose_dump_cache_apply --
 *     Apply dumping cache for all the dhandles.
 */
static int
__verbose_dump_cache_apply(WT_SESSION_IMPL *session, uint64_t *total_bytesp,
  uint64_t *total_dirty_bytesp, uint64_t *total_updates_bytesp)
{
    WT_CONNECTION_IMPL *conn;
    WT_DATA_HANDLE *dhandle;
    WT_DECL_RET;

    conn = S2C(session);
    for (dhandle = NULL;;) {
        WT_DHANDLE_NEXT(session, dhandle, &conn->dhqh, q);
        if (dhandle == NULL)
            break;

        /* Skip if the tree is marked discarded by another thread. */
        if (!WT_DHANDLE_BTREE(dhandle) || !F_ISSET(dhandle, WT_DHANDLE_OPEN) ||
          F_ISSET(dhandle, WT_DHANDLE_DISCARD | WT_DHANDLE_OUTDATED))
            continue;

        WT_WITH_DHANDLE(session, dhandle,
          ret = __verbose_dump_cache_single(
            session, total_bytesp, total_dirty_bytesp, total_updates_bytesp));
        if (ret != 0)
            WT_RET(ret);
    }
    return (0);
}

/*
 * __wt_verbose_dump_cache --
 *     Output diagnostic information about the cache.
 */
int
__wt_verbose_dump_cache(WT_SESSION_IMPL *session)
{
    WT_CACHE *cache;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    double pct;
    uint64_t bytes_dirty_intl, bytes_dirty_leaf, bytes_inmem;
    uint64_t cache_bytes_updates, total_bytes, total_dirty_bytes, total_updates_bytes;
    bool needed;

    conn = S2C(session);
    cache = conn->cache;
    total_bytes = total_dirty_bytes = total_updates_bytes = 0;
    pct = 0.0; /* [-Werror=uninitialized] */
    WT_NOT_READ(cache_bytes_updates, 0);

    WT_RET(__wt_msg(session, "%s", WT_DIVIDER));
    WT_RET(__wt_msg(session, "cache dump"));

    WT_RET(__wt_msg(session, "cache full: %s", __wt_cache_full(session) ? "yes" : "no"));
    needed = __wt_evict_clean_needed(session, &pct);
    WT_RET(__wt_msg(session, "cache clean check: %s (%2.3f%%)", needed ? "yes" : "no", pct));
    needed = __wt_evict_dirty_needed(session, &pct);
    WT_RET(__wt_msg(session, "cache dirty check: %s (%2.3f%%)", needed ? "yes" : "no", pct));
    needed = __wti_evict_updates_needed(session, &pct);
    WT_RET(__wt_msg(session, "cache updates check: %s (%2.3f%%)", needed ? "yes" : "no", pct));

    WT_WITH_HANDLE_LIST_READ_LOCK(session,
      ret = __verbose_dump_cache_apply(
        session, &total_bytes, &total_dirty_bytes, &total_updates_bytes));
    WT_RET(ret);

    /*
     * Apply the overhead percentage so our total bytes are comparable with the tracked value.
     */
    total_bytes = __wt_cache_bytes_plus_overhead(conn->cache, total_bytes);
    cache_bytes_updates = __wt_cache_bytes_updates(cache);

    bytes_inmem = __wt_atomic_load64(&cache->bytes_inmem);
    bytes_dirty_intl = __wt_atomic_load64(&cache->bytes_dirty_intl);
    bytes_dirty_leaf = __wt_atomic_load64(&cache->bytes_dirty_leaf);

    WT_RET(__wt_msg(session, "cache dump: total found: %.2f MB vs tracked inuse %.2f MB",
      (double)total_bytes / WT_MEGABYTE, (double)bytes_inmem / WT_MEGABYTE));
    WT_RET(__wt_msg(session, "total dirty bytes: %.2f MB vs tracked dirty %.2f MB",
      (double)total_dirty_bytes / WT_MEGABYTE,
      (double)(bytes_dirty_intl + bytes_dirty_leaf) / WT_MEGABYTE));
    WT_RET(__wt_msg(session, "total updates bytes: %.2f MB vs tracked updates %.2f MB",
      (double)total_updates_bytes / WT_MEGABYTE, (double)cache_bytes_updates / WT_MEGABYTE));

    return (0);
}
