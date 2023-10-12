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
 *
 * FIXME-WT-11691 The pre-fetch server currently starts up when pre-fetch is enabled on the
 *     connection level but this needs to be modified when we add the session level configuration.
 *     Perhaps we could delay starting the utility threads until the first session enables
 *     pre-fetching.
 */
int
__wt_prefetch_create(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
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

    session_flags = WT_THREAD_CAN_WAIT | WT_THREAD_PANIC_FAIL;
    WT_RET(__wt_thread_group_create(session, &conn->prefetch_threads, "prefetch-server", 8, 8,
      session_flags, __wt_prefetch_thread_chk, __wt_prefetch_thread_run, NULL));

    return (0);
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
    bool locked;

    WT_UNUSED(thread);

    WT_ASSERT(session, session->id != 0);

    conn = S2C(session);
    locked = false;
    WT_RET(__wt_scr_alloc(session, 0, &tmp));

    while (F_ISSET(conn, WT_CONN_PREFETCH_RUN)) {
        /*
         * Wait and cycle if there aren't any pages on the queue. It would be nice if this was
         * interrupt driven, but for now just backoff and re-check.
         */
        if (conn->prefetch_queue_count == 0) {
            __wt_sleep(0, 5000);
            break;
        }
        __wt_spin_lock(session, &conn->prefetch_lock);
        locked = true;
        pe = TAILQ_FIRST(&conn->pfqh);

        /* If there is no work for the thread to do - return back to the thread pool */
        if (pe == NULL)
            break;

        TAILQ_REMOVE(&conn->pfqh, pe, q);
        --conn->prefetch_queue_count;
        WT_ASSERT_ALWAYS(session, F_ISSET(pe->ref, WT_REF_FLAG_PREFETCH),
          "Any ref on the pre-fetch queue needs to have the pre-fetch flag set");
        __wt_spin_unlock(session, &conn->prefetch_lock);
        locked = false;

        WT_WITH_DHANDLE(session, pe->dhandle, ret = __wt_prefetch_page_in(session, pe));
        /*
         * It probably isn't strictly necessary to re-acquire the lock to reset the flag, but other
         * flag accesses do need to lock, so it's better to be consistent.
         */
        __wt_spin_lock(session, &conn->prefetch_lock);
        F_CLR(pe->ref, WT_REF_FLAG_PREFETCH);
        __wt_spin_unlock(session, &conn->prefetch_lock);
        WT_ERR(ret);

        __wt_free(session, pe);
    }

err:
    if (locked)
        __wt_spin_unlock(session, &conn->prefetch_lock);
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
    WT_DECL_RET;
    WT_PREFETCH_QUEUE_ENTRY *pe;

    conn = S2C(session);

    WT_RET(__wt_calloc_one(session, &pe));
    pe->ref = ref;
    pe->first_home = ref->home;
    pe->dhandle = session->dhandle;
    __wt_spin_lock(session, &conn->prefetch_lock);
    if (F_ISSET(ref, WT_REF_FLAG_PREFETCH))
        ret = EBUSY;
    else {
        F_SET(ref, WT_REF_FLAG_PREFETCH);
        TAILQ_INSERT_TAIL(&conn->pfqh, pe, q);
        ++conn->prefetch_queue_count;
    }
    __wt_spin_unlock(session, &conn->prefetch_lock);

    if (ret != 0)
        __wt_free(session, pe);
    return (ret);
}

/*
 * __wt_prefetch_destroy --
 *     Destroy the pre-fetch threads.
 */
int
__wt_prefetch_destroy(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    if (!F_ISSET(conn, WT_CONN_PREFETCH_RUN))
        return (0);

    F_CLR(conn, WT_CONN_PREFETCH_RUN);

    __wt_writelock(session, &conn->prefetch_threads.lock);

    WT_RET(__wt_thread_group_destroy(session, &conn->prefetch_threads));

    return (0);
}
