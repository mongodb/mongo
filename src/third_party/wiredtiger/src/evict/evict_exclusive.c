/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Exclusive eviction access: acquiring and releasing the per-tree evict_disabled counter, which
 * prevents the eviction server from walking or queuing pages for a specific tree. When
 * evict_disabled is non-zero the walk skips the tree entirely and force-eviction of individual
 * pages is disallowed; only whole-file eviction via __wt_evict_file remains permitted.
 *
 * __wt_evict_file_exclusive_on increments evict_disabled, interrupts any in-progress walk for
 * the tree, drains pages already on the LRU queues, and waits for concurrent eviction activity
 * to finish. __wt_evict_file_exclusive_off decrements it atomically. Both functions are called
 * from schema operations, tree open/close paths, and __wt_evict_file itself.
 *
 * __wti_evict_lock_handle_list acquires the connection-wide dhandle read lock with a custom
 * yield/sleep backoff that watches pass_intr so the eviction server can be interrupted quickly.
 * __wti_evict_set_saved_walk_tree maintains the session_inuse reference count on the data handle
 * that the eviction server last walked, preventing it from being closed mid-walk.
 */
#include "wt_internal.h"

/*
 * __wti_evict_lock_handle_list --
 *     Try to get the handle list lock, with yield and sleep back off. Keep timing statistics
 *     overall.
 */
int
__wti_evict_lock_handle_list(WT_SESSION_IMPL *session)
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
      __wt_atomic_load_uint32_v_relaxed(&evict->pass_intr) == 0;
      spins++) {
        if (spins < WT_THOUSAND)
            __wt_yield();
        else
            __wt_sleep(0, WT_THOUSAND);
    }
    return (ret);
}

/*
 * __wti_evict_set_saved_walk_tree --
 *     Set saved walk tree maintaining use count. Call it with NULL to clear the saved walk tree.
 */
void
__wti_evict_set_saved_walk_tree(WT_SESSION_IMPL *session, WT_DATA_HANDLE *new_dhandle)
{
    WT_DATA_HANDLE *old_dhandle;
    WT_EVICT *evict;

    evict = S2C(session)->evict;
    old_dhandle = evict->walk_tree;

    if (old_dhandle == new_dhandle)
        return;

    if (new_dhandle != NULL)
        (void)__wt_atomic_add_int32(&new_dhandle->session_inuse, 1);

    evict->walk_tree = new_dhandle;

    if (old_dhandle != NULL) {
        WT_ASSERT(session, __wt_atomic_load_int32_relaxed(&old_dhandle->session_inuse) > 0);
        (void)__wt_atomic_sub_int32(&old_dhandle->session_inuse, 1);
    }
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
    (void)__wt_atomic_add_uint32_v(&evict->pass_intr, 1);
    WTI_WITH_PASS_LOCK(
      session, ret = __wti_evict_clear_walk_and_saved_tree_if_current_locked(session));
    (void)__wt_atomic_sub_uint32_v(&evict->pass_intr, 1);
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
    while (__wt_tsan_suppress_load_uint32_v(&btree->evict_busy) > 0)
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
 * access, we acquire the eviction walk-lock and then the eviction's pass-intr lock. The eviction
 * server can hold the pass-intr lock and call into this function, which might deadlock with another
 * thread trying to get exclusive eviction access.
 */
#if defined(HAVE_DIAGNOSTIC)
    {
        int32_t v;

        WT_ASSERT(session, __wt_atomic_load_ptr_relaxed(&btree->evict_ref) == NULL);
        v = __wt_atomic_sub_int32(&btree->evict_disabled, 1);
        WT_ASSERT(session, v >= 0);
    }
#else
    (void)__wt_atomic_sub_int32(&btree->evict_disabled, 1);
#endif
    __wt_verbose_debug1(session, WT_VERB_EVICTION, "released exclusive eviction lock on btree %s",
      btree->dhandle->name);
}
