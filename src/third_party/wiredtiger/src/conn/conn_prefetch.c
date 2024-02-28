/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_prefetch_create --
 *     Start the pre-fetch server.
 */
int
__wt_prefetch_create(WT_SESSION_IMPL *session, const char *cfg[])
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

    F_SET(conn, WT_CONN_PREFETCH_RUN);

    session_flags = WT_THREAD_CAN_WAIT | WT_THREAD_PANIC_FAIL | WT_SESSION_PREFETCH_THREAD;
    WT_ERR(__wt_thread_group_create(session, &conn->prefetch_threads, "prefetch-server",
      WT_PREFETCH_THREAD_COUNT, WT_PREFETCH_THREAD_COUNT, session_flags, __wt_prefetch_thread_chk,
      __wt_prefetch_thread_run, NULL));
    return (0);

err:
    /* Quit the prefetch server. */
    WT_TRET(__wt_prefetch_destroy(session));
    return (ret);
}

/*
 * __wt_prefetch_thread_chk --
 *     Check to decide if the pre-fetch thread should continue running.
 */
bool
__wt_prefetch_thread_chk(WT_SESSION_IMPL *session)
{
    return (F_ISSET(S2C(session), WT_CONN_PREFETCH_RUN));
}

/*
 * __wt_prefetch_thread_run --
 *     Entry function for a prefetch thread. This is called repeatedly from the thread group code so
 *     it does not need to loop itself.
 */
int
__wt_prefetch_thread_run(WT_SESSION_IMPL *session, WT_THREAD *thread)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    WT_PREFETCH_QUEUE_ENTRY *pe;

    WT_UNUSED(thread);
    WT_ASSERT(session, session->id != 0);
    conn = S2C(session);

    WT_RET(__wt_scr_alloc(session, 0, &tmp));

    if (F_ISSET(conn, WT_CONN_PREFETCH_RUN))
        __wt_cond_wait(session, conn->prefetch_threads.wait_cond, 10 * WT_THOUSAND, NULL);

    while (!TAILQ_EMPTY(&conn->pfqh)) {
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
         * We increment this while in the prefetch lock as the thread reading from the queue expects
         * that behavior.
         */
        (void)__wt_atomic_addv32(&((WT_BTREE *)pe->dhandle->handle)->prefetch_busy, 1);
        __wt_spin_unlock(session, &conn->prefetch_lock);

        WT_PREFETCH_ASSERT(
          session, F_ISSET(pe->ref, WT_REF_FLAG_PREFETCH), block_prefetch_skipped_no_flag_set);

        /*
         * It's a weird case, but if verify is utilizing prefetch and encounters a corrupted block,
         * stop using prefetch. Some of the guarantees about ref and page freeing are ignored in
         * that case, which can invalidate entries on the prefetch queue. Don't prefetch fast
         * deleted pages - they have special performance and visibility considerations associated
         * with them. Don't prefetch fast deleted pages to avoid wasted effort. We can skip reading
         * these deleted pages into the cache if the fast truncate information is visible in the
         * session transaction snapshot.
         */
        if (!F_ISSET(conn, WT_CONN_DATA_CORRUPTION) && pe->ref->page_del == NULL)
            WT_WITH_DHANDLE(session, pe->dhandle, ret = __wt_prefetch_page_in(session, pe));

        /*
         * We don't take the prefetch lock here as the lock protects the queue, not the
         * prefetch_busy flag.
         */
        F_CLR(pe->ref, WT_REF_FLAG_PREFETCH);
        (void)__wt_atomic_subv32(&((WT_BTREE *)pe->dhandle->handle)->prefetch_busy, 1);
        WT_ERR(ret);

        __wt_free(session, pe);
    }

err:
    __wt_scr_free(session, &tmp);
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
    WT_PREFETCH_QUEUE_ENTRY *pe;

    conn = S2C(session);

    WT_RET(__wt_calloc_one(session, &pe));
    pe->ref = ref;
    pe->first_home = ref->home;
    pe->dhandle = session->dhandle;

    __wt_spin_lock(session, &conn->prefetch_lock);
    /* Don't queue pages for trees that have eviction disabled. */
    if (S2BT(session)->evict_disabled > 0) {
        __wt_spin_unlock(session, &conn->prefetch_lock);
        WT_RET(EBUSY);
    }

    F_SET(ref, WT_REF_FLAG_PREFETCH);
    TAILQ_INSERT_TAIL(&conn->pfqh, pe, q);
    ++conn->prefetch_queue_count;
    __wt_spin_unlock(session, &conn->prefetch_lock);

    __wt_cond_signal(session, conn->prefetch_threads.wait_cond);

    return (0);
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
    /*
     * We guarantee that no dhandle enters the prefetch busy state while we wait. This is because we
     * hold the lock while draining, and the lock is required when taking from the queue.
     */
    if (dhandle != NULL)
        while (((WT_BTREE *)dhandle->handle)->prefetch_busy > 0)
            __wt_yield();

    /* Empty the queue of the relevant pages, or all of them if specified. */
    TAILQ_FOREACH_SAFE(pe, &conn->pfqh, q, pe_tmp)
    {
        if (all || pe->dhandle == dhandle) {
            TAILQ_REMOVE(&conn->pfqh, pe, q);
            F_CLR(pe->ref, WT_REF_FLAG_PREFETCH);
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
    return (0);
}

/*
 * __wt_prefetch_destroy --
 *     Destroy the pre-fetch threads.
 */
int
__wt_prefetch_destroy(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    conn = S2C(session);

    if (!F_ISSET(conn, WT_CONN_PREFETCH_RUN))
        return (0);

    F_CLR(conn, WT_CONN_PREFETCH_RUN);

    /* Ensure that the pre-fetch queue is drained. */
    WT_TRET(__wt_conn_prefetch_clear_tree(session, true));

    /* Let any running threads finish up. */
    __wt_cond_signal(session, conn->prefetch_threads.wait_cond);

    __wt_writelock(session, &conn->prefetch_threads.lock);

    WT_RET(__wt_thread_group_destroy(session, &conn->prefetch_threads));

    return (ret);
}
