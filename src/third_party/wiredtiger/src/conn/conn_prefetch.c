/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __prefetch_thread_chk --
 *     Check to decide if the pre-fetch thread should continue running.
 */
static bool
__prefetch_thread_chk(WT_SESSION_IMPL *session)
{
    return (FLD_ISSET(S2C(session)->server_flags, WT_CONN_SERVER_PREFETCH));
}

/*
 * __prefetch_thread_run --
 *     Entry function for a prefetch thread. This is called repeatedly from the thread group code so
 *     it does not need to loop itself.
 */
static int
__prefetch_thread_run(WT_SESSION_IMPL *session, WT_THREAD *thread)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_PREFETCH_QUEUE_ENTRY *pe;

    WT_UNUSED(thread);
    WT_ASSERT(session, !WT_SESSION_IS_DEFAULT(session));
    conn = S2C(session);

    /* Mark the session as a prefetch thread session. */
    F_SET(session, WT_SESSION_PREFETCH_THREAD);

    if (FLD_ISSET(conn->server_flags, WT_CONN_SERVER_PREFETCH))
        __wt_cond_wait(session, conn->prefetch_threads.wait_cond, WT_THOUSAND * WT_THOUSAND, NULL);

    while (!TAILQ_EMPTY(&conn->pfqh)) {
        /* Encourage races. */
        __wt_timing_stress(session, WT_TIMING_STRESS_PREFETCH_1, NULL);

        __wt_spin_lock(session, &conn->prefetch_lock);
        pe = TAILQ_FIRST(&conn->pfqh);

        /* If there is no work for the thread to do - return back to the thread pool */
        if (pe == NULL) {
            __wt_spin_unlock(session, &conn->prefetch_lock);
            break;
        }

        TAILQ_REMOVE(&conn->pfqh, pe, q);
        --conn->prefetch_queue_count;

        /*
         * If the cache is getting close to its eviction clean trigger, don't attempt to pre-fetch
         * the current ref as we may hang if the cache becomes full and we need to wait until space
         * in the cache clears up. Repeat this process until either eviction has evicted enough
         * eligible pages (allowing pre-fetch to read into the cache), or we iterate through and
         * remove all the refs from the pre-fetch queue and pre-fetch becomes a no-op.
         */
        if (__wt_evict_clean_pressure(session)) {
            F_CLR_ATOMIC_8(pe->ref, WT_REF_FLAG_PREFETCH);
            __wt_spin_unlock(session, &conn->prefetch_lock);
            __wt_free(session, pe);
            continue;
        }

        /* Encourage races. */
        __wt_timing_stress(session, WT_TIMING_STRESS_PREFETCH_2, NULL);

        /*
         * We increment this while in the prefetch lock as the thread reading from the queue expects
         * that behavior.
         */
        (void)__wt_atomic_addv32(&((WT_BTREE *)pe->dhandle->handle)->prefetch_busy, 1);

        WT_PREFETCH_ASSERT(
          session, F_ISSET_ATOMIC_8(pe->ref, WT_REF_FLAG_PREFETCH), prefetch_skipped_no_flag_set);
        __wt_spin_unlock(session, &conn->prefetch_lock);

        /*
         * It's a weird case, but if verify is utilizing prefetch and encounters a corrupted block,
         * stop using prefetch. Some of the guarantees about ref and page freeing are ignored in
         * that case, which can invalidate entries on the prefetch queue. Don't prefetch fast
         * deleted pages - they have special performance and visibility considerations associated
         * with them. Don't prefetch fast deleted pages to avoid wasted effort. We can skip reading
         * these deleted pages into the cache if the fast truncate information is visible in the
         * session transaction snapshot.
         */
        if (!F_ISSET_ATOMIC_32(conn, WT_CONN_DATA_CORRUPTION) && pe->ref->page_del == NULL)
            WT_WITH_DHANDLE(session, pe->dhandle, ret = __wt_prefetch_page_in(session, pe));

        /*
         * It is now safe to clear the flag. The prefetch worker is done interacting with the ref
         * and the associated internal page can be safely evicted from now on.
         */
        F_CLR_ATOMIC_8(pe->ref, WT_REF_FLAG_PREFETCH);
        (void)__wt_atomic_subv32(&((WT_BTREE *)pe->dhandle->handle)->prefetch_busy, 1);

        __wt_free(session, pe);

        /*
         * Ignore specific errors that prevented prefetch from making progress, they are harmless.
         */
        if (ret == WT_NOTFOUND || ret == WT_RESTART) {
            WT_STAT_CONN_INCR(session, prefetch_skipped_error_ok);
            ret = 0;
        }
        WT_ERR(ret);
    }

err:
    return (ret);
}

/*
 * __wti_prefetch_create --
 *     Start the pre-fetch server.
 */
int
__wti_prefetch_create(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    uint32_t session_flags;

    conn = S2C(session);

    /*
     * This might have already been parsed and set during connection configuration, but do it here
     * as well, in preparation for the functionality being runtime configurable.
     */
    WT_RET(__wt_config_gets(session, cfg, "prefetch.available", &cval));
    conn->prefetch_available = cval.val != 0;

    /*
     * Pre-fetch functionality isn't runtime configurable, so don't bother starting utility threads
     * if it isn't available.
     */
    if (!conn->prefetch_available)
        return (0);

    FLD_SET(conn->server_flags, WT_CONN_SERVER_PREFETCH);

    session_flags = WT_THREAD_CAN_WAIT | WT_THREAD_PANIC_FAIL;
    WT_ERR(__wt_thread_group_create(session, &conn->prefetch_threads, "prefetch-server",
      WT_PREFETCH_THREAD_COUNT, WT_PREFETCH_THREAD_COUNT, session_flags, __prefetch_thread_chk,
      __prefetch_thread_run, NULL));
    return (0);

err:
    /* Quit the prefetch server. */
    WT_TRET(__wti_prefetch_destroy(session));
    return (ret);
}

/*
 * __wt_conn_prefetch_queue_push --
 *     Push a ref onto the pre-fetch queue.
 */
int
__wt_conn_prefetch_queue_push(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_PREFETCH_QUEUE_ENTRY *pe;

    conn = S2C(session);

    /*
     * Pre-fetch shouldn't wait until the cache is already full before it stops adding new refs to
     * the queue. It should take a more conservative approach and stop as soon as it detects that we
     * are close to hitting the eviction clean trigger.
     */
    if (__wt_evict_clean_pressure(session))
        return (EBUSY);

    WT_RET(__wt_calloc_one(session, &pe));
    pe->ref = ref;
    pe->first_home = ref->home;
    pe->dhandle = session->dhandle;

    __wt_spin_lock(session, &conn->prefetch_lock);
    /* Don't queue pages for trees that have eviction disabled. */
    if (S2BT(session)->evict_disabled > 0) {
        __wt_spin_unlock(session, &conn->prefetch_lock);
        WT_ERR(EBUSY);
    }

    /* We should never add a ref that is already in the prefetch queue. */
    WT_ASSERT(session, !F_ISSET_ATOMIC_8(ref, WT_REF_FLAG_PREFETCH));

    /* Encourage races. */
    __wt_timing_stress(session, WT_TIMING_STRESS_PREFETCH_3, NULL);

    /*
     * The page can be read into memory and evicted concurrently. Eviction may split the page and
     * add the ref to the stash to be freed later before the WT_REF_FLAG_PREFETCH flag is set. In
     * another case, the page can be fast truncated and become globally visible concurrently. This
     * may also lead to the ref being added to the stash before the WT_REF_FLAG_PREFETCH flag is
     * set. Lock the ref to ensure those cases cannot happen. If we fail to lock the ref, someone
     * else must have started to operate on it. Ignore this page without waiting.
     */
    if (!WT_REF_CAS_STATE(session, ref, WT_REF_DISK, WT_REF_LOCKED)) {
        __wt_spin_unlock(session, &conn->prefetch_lock);
        goto err;
    }

    /*
     * On top of indicating the leaf page is now in the prefetch queue, the prefetch flag also
     * guarantees the corresponding internal page and itself cannot be evicted until prefetch has
     * processed the leaf page.
     */
    F_SET_ATOMIC_8(ref, WT_REF_FLAG_PREFETCH);
    /* Unlock the ref. */
    WT_REF_SET_STATE(ref, WT_REF_DISK);
    TAILQ_INSERT_TAIL(&conn->pfqh, pe, q);
    ++conn->prefetch_queue_count;
    __wt_spin_unlock(session, &conn->prefetch_lock);
    __wt_cond_signal(session, conn->prefetch_threads.wait_cond);

    if (0) {
err:
        __wt_free(session, pe);
    }

    return (ret);
}

/*
 * __wt_conn_prefetch_clear_tree --
 *     Clear pages from the pre-fetch queue, either all pages on the queue or pages from the current
 *     btree - depending on input parameters.
 */
int
__wt_conn_prefetch_clear_tree(WT_SESSION_IMPL *session, bool all)
{
    WT_CONNECTION_IMPL *conn;
    WT_DATA_HANDLE *dhandle;
    WT_PREFETCH_QUEUE_ENTRY *pe, *pe_tmp;

    conn = S2C(session);
    dhandle = session->dhandle;

    WT_ASSERT_ALWAYS(session, all || dhandle != NULL,
      "Pre-fetch needs to save a valid dhandle when clearing the queue for a btree");

    __wt_spin_lock(session, &conn->prefetch_lock);

    /* Empty the queue of the relevant pages, or all of them if specified. */
    TAILQ_FOREACH_SAFE(pe, &conn->pfqh, q, pe_tmp)
    {
        if (all || pe->dhandle == dhandle) {
            TAILQ_REMOVE(&conn->pfqh, pe, q);
            F_CLR_ATOMIC_8(pe->ref, WT_REF_FLAG_PREFETCH);
            __wt_free(session, pe);
            --conn->prefetch_queue_count;
        }
    }
    if (all)
        WT_ASSERT(session, conn->prefetch_queue_count == 0);

    /*
     * Give up the lock, consumers of the queue shouldn't see pages relevant to them. Additionally
     * new pages cannot be queued as the btree->evict_disable flag should prevent that. It is
     * important that the flag is checked after locking the prefetch queue lock. If not then threads
     * may not note that the tree is closed for prefetch.
     */
    __wt_spin_unlock(session, &conn->prefetch_lock);

    /*
     * If we are only clearing refs from a certain tree, wait for any other concurrent pre-fetch
     * activity to drain to prevent any invalid ref uses.
     */
    if (!all) {
        while (((WT_BTREE *)dhandle->handle)->prefetch_busy > 0)
            __wt_yield();
    }

    return (0);
}

/*
 * __wti_prefetch_destroy --
 *     Destroy the pre-fetch threads.
 */
int
__wti_prefetch_destroy(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    conn = S2C(session);

    if (!FLD_ISSET(conn->server_flags, WT_CONN_SERVER_PREFETCH))
        return (0);

    FLD_CLR(conn->server_flags, WT_CONN_SERVER_PREFETCH);

    /* Ensure that the pre-fetch queue is drained. */
    WT_TRET(__wt_conn_prefetch_clear_tree(session, true));

    /* Let any running threads finish up. */
    __wt_cond_signal(session, conn->prefetch_threads.wait_cond);

    __wt_writelock(session, &conn->prefetch_threads.lock);

    WT_RET(__wt_thread_group_destroy(session, &conn->prefetch_threads));

    return (ret);
}
