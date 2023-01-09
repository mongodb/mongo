/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __lsm_manager_run_server(WT_SESSION_IMPL *);

static WT_THREAD_RET __lsm_worker_manager(void *);

/*
 * __wt_lsm_manager_config --
 *     Configure the LSM manager.
 */
int
__wt_lsm_manager_config(WT_SESSION_IMPL *session, const char **cfg)
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    WT_RET(__wt_config_gets(session, cfg, "lsm_manager.merge", &cval));
    if (cval.val)
        F_SET(conn, WT_CONN_LSM_MERGE);
    WT_RET(__wt_config_gets(session, cfg, "lsm_manager.worker_thread_max", &cval));
    if (cval.val)
        conn->lsm_manager.lsm_workers_max = (uint32_t)cval.val;
    return (0);
}

/*
 * __lsm_general_worker_start --
 *     Start up all of the general LSM worker threads.
 */
static int
__lsm_general_worker_start(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_LSM_MANAGER *manager;
    WT_LSM_WORKER_ARGS *worker_args;

    conn = S2C(session);
    manager = &conn->lsm_manager;

    /*
     * Start the worker threads or new worker threads if called via reconfigure. The LSM manager is
     * worker[0]. This should get more sophisticated in the future - only launching as many worker
     * threads as are required to keep up with demand.
     */
    WT_ASSERT(session, manager->lsm_workers > 0);
    WT_ASSERT(session, manager->lsm_workers < manager->lsm_workers_max);
    for (; manager->lsm_workers < manager->lsm_workers_max; manager->lsm_workers++) {
        worker_args = &manager->lsm_worker_cookies[manager->lsm_workers];
        worker_args->work_cond = manager->work_cond;
        worker_args->id = manager->lsm_workers;
        /*
         * The first worker only does switch and drop operations as these are both short operations
         * and it is essential that switches are responsive to avoid introducing throttling stalls.
         */
        if (manager->lsm_workers == 1)
            worker_args->type = WT_LSM_WORK_DROP | WT_LSM_WORK_SWITCH;
        else {
            worker_args->type = WT_LSM_WORK_GENERAL_OPS;
            /*
             * Only allow half of the threads to run merges to avoid all workers getting stuck in
             * long-running merge operations. Make sure the first worker is allowed, so that there
             * is at least one thread capable of running merges. We know the first worker is id 2,
             * so set merges on even numbered workers.
             */
            if (manager->lsm_workers % 2 == 0)
                FLD_SET(worker_args->type, WT_LSM_WORK_MERGE);
        }
        WT_RET(__wt_lsm_worker_start(session, worker_args));
    }

    /*
     * Setup the first worker properly - if there are only a minimal number of workers allow the
     * first worker to flush. Otherwise a single merge can lead to switched chunks filling up the
     * cache. This is separate to the main loop so that it is applied on startup and reconfigure.
     */
    if (manager->lsm_workers_max == WT_LSM_MIN_WORKERS)
        FLD_SET(manager->lsm_worker_cookies[1].type, WT_LSM_WORK_FLUSH);
    else
        FLD_CLR(manager->lsm_worker_cookies[1].type, WT_LSM_WORK_FLUSH);

    return (0);
}

/*
 * __lsm_stop_workers --
 *     Stop worker threads until the number reaches the configured amount.
 */
static int
__lsm_stop_workers(WT_SESSION_IMPL *session)
{
    WT_LSM_MANAGER *manager;
    WT_LSM_WORKER_ARGS *worker_args;

    manager = &S2C(session)->lsm_manager;
    /*
     * Start at the end of the list of threads and stop them until we have the desired number. We
     * want to keep all active threads packed at the front of the worker array.
     */
    WT_ASSERT(session, manager->lsm_workers > manager->lsm_workers_max);
    for (; manager->lsm_workers > manager->lsm_workers_max; manager->lsm_workers--) {
        worker_args = &manager->lsm_worker_cookies[manager->lsm_workers - 1];
        WT_ASSERT(session, worker_args->tid_set);

        WT_RET(__wt_lsm_worker_stop(session, worker_args));
        worker_args->type = 0;

        /*
         * We do not clear the other fields because they are allocated statically when the
         * connection was opened.
         */
    }

    /*
     * Setup the first worker properly - if there are only a minimal number of workers it should
     * flush. Since the number of threads is being reduced the field can't already be set.
     */
    if (manager->lsm_workers_max == WT_LSM_MIN_WORKERS)
        FLD_SET(manager->lsm_worker_cookies[1].type, WT_LSM_WORK_FLUSH);

    return (0);
}

/*
 * __wt_lsm_manager_reconfig --
 *     Re-configure the LSM manager.
 */
int
__wt_lsm_manager_reconfig(WT_SESSION_IMPL *session, const char **cfg)
{
    WT_LSM_MANAGER *manager;
    uint32_t orig_workers;

    manager = &S2C(session)->lsm_manager;
    orig_workers = manager->lsm_workers_max;

    WT_RET(__wt_lsm_manager_config(session, cfg));
    /*
     * If LSM hasn't started yet, we simply reconfigured the settings and we'll let the normal code
     * path start the threads.
     */
    if (manager->lsm_workers_max == 0)
        return (0);
    if (manager->lsm_workers == 0)
        return (0);
    /*
     * If the number of workers has not changed, we're done.
     */
    if (orig_workers == manager->lsm_workers_max)
        return (0);
    /*
     * If we want more threads, start them.
     */
    if (manager->lsm_workers_max > orig_workers)
        return (__lsm_general_worker_start(session));

    /*
     * Otherwise we want to reduce the number of workers.
     */
    WT_ASSERT(session, manager->lsm_workers_max < orig_workers);
    WT_RET(__lsm_stop_workers(session));
    return (0);
}

/*
 * __wt_lsm_manager_start --
 *     Start the LSM management infrastructure. Our queues and locks were initialized when the
 *     connection was initialized.
 */
int
__wt_lsm_manager_start(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_LSM_MANAGER *manager;
    WT_SESSION_IMPL *worker_session;
    uint32_t i;

    conn = S2C(session);
    manager = &conn->lsm_manager;

    /*
     * If readonly or the manager is running, or we've already failed, there's no work to do.
     */
    if (F_ISSET(conn, WT_CONN_READONLY) || manager->lsm_workers != 0 ||
      F_ISSET(manager, WT_LSM_MANAGER_SHUTDOWN))
        return (0);

    /* It's possible to race, see if we're the winner. */
    if (!__wt_atomic_cas32(&manager->lsm_workers, 0, 1))
        return (0);

    /* We need at least a manager, a switch thread and a generic worker. */
    WT_ASSERT(session, manager->lsm_workers_max > 2);

    /*
     * Open sessions for all potential worker threads here - it's not safe to have worker threads
     * open/close sessions themselves. All the LSM worker threads do their operations on read-only
     * files. Use read-uncommitted isolation to avoid keeping updates in cache unnecessarily.
     */
    for (i = 0; i < WT_LSM_MAX_WORKERS; i++) {
        WT_ERR(__wt_open_internal_session(conn, "lsm-worker", false, 0, 0, &worker_session));
        worker_session->isolation = WT_ISO_READ_UNCOMMITTED;
        manager->lsm_worker_cookies[i].session = worker_session;
    }

    FLD_SET(conn->server_flags, WT_CONN_SERVER_LSM);

    /* Start the LSM manager thread. */
    WT_ERR(__wt_thread_create(session, &manager->lsm_worker_cookies[0].tid, __lsm_worker_manager,
      &manager->lsm_worker_cookies[0]));

    if (0) {
err:
        for (i = 0; (worker_session = manager->lsm_worker_cookies[i].session) != NULL; i++)
            WT_TRET((&worker_session->iface)->close(&worker_session->iface, NULL));

        /* Make the failure permanent, we won't try again. */
        F_SET(manager, WT_LSM_MANAGER_SHUTDOWN);

        /*
         * Reset the workers count (otherwise, LSM destroy will hang waiting for threads to exit.
         */
        WT_PUBLISH(manager->lsm_workers, 0);
    }
    return (ret);
}

/*
 * __wt_lsm_manager_free_work_unit --
 *     Release an LSM tree work unit.
 */
void
__wt_lsm_manager_free_work_unit(WT_SESSION_IMPL *session, WT_LSM_WORK_UNIT *entry)
{
    if (entry != NULL) {
        WT_ASSERT(session, entry->lsm_tree->queue_ref > 0);

        (void)__wt_atomic_sub32(&entry->lsm_tree->queue_ref, 1);
        __wt_free(session, entry);
    }
}

/*
 * __wt_lsm_manager_destroy --
 *     Destroy the LSM manager threads and subsystem.
 */
int
__wt_lsm_manager_destroy(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_LSM_MANAGER *manager;
    WT_LSM_WORK_UNIT *current;
    uint64_t removed;
    uint32_t i;

    conn = S2C(session);
    manager = &conn->lsm_manager;
    removed = 0;

    /* Clear the LSM server flag. */
    FLD_CLR(conn->server_flags, WT_CONN_SERVER_LSM);

    WT_ASSERT(session, !F_ISSET(conn, WT_CONN_READONLY) || manager->lsm_workers == 0);
    if (manager->lsm_workers > 0) {
        /* Wait for the main LSM manager thread to finish. */
        while (!F_ISSET(manager, WT_LSM_MANAGER_SHUTDOWN)) {
            WT_STAT_CONN_INCR(session, conn_close_blocked_lsm);
            __wt_yield();
        }

        /* Clean up open LSM handles. */
        ret = __wt_lsm_tree_close_all(session);

        WT_TRET(__wt_thread_join(session, &manager->lsm_worker_cookies[0].tid));

        /* Release memory from any operations left on the queue. */
        while ((current = TAILQ_FIRST(&manager->switchqh)) != NULL) {
            TAILQ_REMOVE(&manager->switchqh, current, q);
            ++removed;
            __wt_lsm_manager_free_work_unit(session, current);
        }
        while ((current = TAILQ_FIRST(&manager->appqh)) != NULL) {
            TAILQ_REMOVE(&manager->appqh, current, q);
            ++removed;
            __wt_lsm_manager_free_work_unit(session, current);
        }
        while ((current = TAILQ_FIRST(&manager->managerqh)) != NULL) {
            TAILQ_REMOVE(&manager->managerqh, current, q);
            ++removed;
            __wt_lsm_manager_free_work_unit(session, current);
        }

        /* Close all LSM worker sessions. */
        for (i = 0; i < WT_LSM_MAX_WORKERS; i++)
            WT_TRET(__wt_session_close_internal(manager->lsm_worker_cookies[i].session));
    }
    WT_STAT_CONN_INCRV(session, lsm_work_units_discarded, removed);

    return (ret);
}

/*
 * __lsm_manager_worker_shutdown --
 *     Shutdown the LSM worker threads.
 */
static int
__lsm_manager_worker_shutdown(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;
    WT_LSM_MANAGER *manager;
    u_int i;

    manager = &S2C(session)->lsm_manager;

    /*
     * Wait for the rest of the LSM workers to shutdown. Start at index one - since we (the manager)
     * are at index 0.
     */
    for (i = 1; i < manager->lsm_workers; i++) {
        WT_ASSERT(session, manager->lsm_worker_cookies[i].tid_set);
        WT_TRET(__wt_lsm_worker_stop(session, &manager->lsm_worker_cookies[i]));
    }
    return (ret);
}

/*
 * __lsm_manager_run_server --
 *     Run manager thread operations.
 */
static int
__lsm_manager_run_server(WT_SESSION_IMPL *session)
{
    struct timespec now;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_LSM_TREE *lsm_tree;
    uint64_t fillms, idlems;
    bool dhandle_locked;

    conn = S2C(session);
    dhandle_locked = false;

    while (FLD_ISSET(conn->server_flags, WT_CONN_SERVER_LSM)) {
        __wt_sleep(0, 10 * WT_THOUSAND);
        if (TAILQ_EMPTY(&conn->lsmqh))
            continue;
        __wt_readlock(session, &conn->dhandle_lock);
        FLD_SET(session->lock_flags, WT_SESSION_LOCKED_HANDLE_LIST_READ);
        dhandle_locked = true;
        TAILQ_FOREACH (lsm_tree, &conn->lsmqh, q) {
            if (!lsm_tree->active)
                continue;
            __wt_epoch(session, &now);
            /*
             * If work was added reset our counts and time. Otherwise compute an idle time.
             */
            if (lsm_tree->work_count != lsm_tree->mgr_work_count || lsm_tree->work_count == 0) {
                idlems = 0;
                lsm_tree->mgr_work_count = lsm_tree->work_count;
                lsm_tree->last_active = now;
            } else
                idlems = WT_TIMEDIFF_MS(now, lsm_tree->last_active);
            fillms = 3 * lsm_tree->chunk_fill_ms;
            if (fillms == 0)
                fillms = 10 * WT_THOUSAND;
            /*
             * If the tree appears to not be triggering enough LSM maintenance, help it out. Some
             * types of additional work units don't hurt, and can be necessary if some work units
             * aren't completed for some reason. If the tree hasn't been modified, and there are
             * more than 1 chunks - try to get the tree smaller so queries run faster. If we are
             * getting aggressive - ensure there are enough work units that we can get chunks
             * merged. If we aren't pushing enough work units, compared to how often new chunks are
             * being created add some more.
             */
            if (lsm_tree->queue_ref >= LSM_TREE_MAX_QUEUE)
                WT_STAT_CONN_INCR(session, lsm_work_queue_max);
            else if ((!lsm_tree->modified && lsm_tree->nchunks > 1) ||
              (lsm_tree->queue_ref == 0 && lsm_tree->nchunks > 1) ||
              (lsm_tree->merge_aggressiveness > WT_LSM_AGGRESSIVE_THRESHOLD &&
                !F_ISSET(lsm_tree, WT_LSM_TREE_COMPACTING)) ||
              idlems > fillms) {
                WT_ERR(__wt_lsm_manager_push_entry(session, WT_LSM_WORK_SWITCH, 0, lsm_tree));
                WT_ERR(__wt_lsm_manager_push_entry(session, WT_LSM_WORK_DROP, 0, lsm_tree));
                WT_ERR(__wt_lsm_manager_push_entry(session, WT_LSM_WORK_FLUSH, 0, lsm_tree));
                WT_ERR(__wt_lsm_manager_push_entry(session, WT_LSM_WORK_BLOOM, 0, lsm_tree));
                __wt_verbose(session, WT_VERB_LSM_MANAGER,
                  "MGR %s: queue %" PRIu32 " mod %d nchunks %" PRIu32 " flags %#" PRIx32
                  " aggressive %" PRIu32 " idlems %" PRIu64 " fillms %" PRIu64,
                  lsm_tree->name, lsm_tree->queue_ref, lsm_tree->modified, lsm_tree->nchunks,
                  lsm_tree->flags, lsm_tree->merge_aggressiveness, idlems, fillms);
                WT_ERR(__wt_lsm_manager_push_entry(session, WT_LSM_WORK_MERGE, 0, lsm_tree));
            }
        }
        __wt_readunlock(session, &conn->dhandle_lock);
        FLD_CLR(session->lock_flags, WT_SESSION_LOCKED_HANDLE_LIST_READ);
        dhandle_locked = false;
    }

err:
    if (dhandle_locked) {
        __wt_readunlock(session, &conn->dhandle_lock);
        FLD_CLR(session->lock_flags, WT_SESSION_LOCKED_HANDLE_LIST_READ);
    }
    return (ret);
}

/*
 * __lsm_worker_manager --
 *     A thread that manages all open LSM trees, and the shared LSM worker threads.
 */
static WT_THREAD_RET
__lsm_worker_manager(void *arg)
{
    WT_DECL_RET;
    WT_LSM_MANAGER *manager;
    WT_LSM_WORKER_ARGS *cookie;
    WT_SESSION_IMPL *session;

    cookie = (WT_LSM_WORKER_ARGS *)arg;
    session = cookie->session;
    manager = &S2C(session)->lsm_manager;

    WT_ERR(__lsm_general_worker_start(session));
    WT_ERR(__lsm_manager_run_server(session));
    WT_ERR(__lsm_manager_worker_shutdown(session));

    if (ret != 0) {
err:
        WT_IGNORE_RET(__wt_panic(session, ret, "LSM worker manager thread error"));
    }

    /* Connection close waits on us to shutdown, let it know we're done. */
    F_SET(manager, WT_LSM_MANAGER_SHUTDOWN);
    WT_FULL_BARRIER();

    return (WT_THREAD_RET_VALUE);
}

/*
 * __wt_lsm_manager_clear_tree --
 *     Remove all entries for a tree from the LSM manager queues. This introduces an inefficiency if
 *     LSM trees are being opened and closed regularly.
 */
void
__wt_lsm_manager_clear_tree(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
    WT_LSM_MANAGER *manager;
    WT_LSM_WORK_UNIT *current, *tmp;
    uint64_t removed;

    manager = &S2C(session)->lsm_manager;
    removed = 0;

    /* Clear out the tree from the switch queue */
    __wt_spin_lock(session, &manager->switch_lock);
    TAILQ_FOREACH_SAFE(current, &manager->switchqh, q, tmp)
    {
        if (current->lsm_tree != lsm_tree)
            continue;
        ++removed;
        TAILQ_REMOVE(&manager->switchqh, current, q);
        __wt_lsm_manager_free_work_unit(session, current);
    }
    __wt_spin_unlock(session, &manager->switch_lock);
    /* Clear out the tree from the application queue */
    __wt_spin_lock(session, &manager->app_lock);
    TAILQ_FOREACH_SAFE(current, &manager->appqh, q, tmp)
    {
        if (current->lsm_tree != lsm_tree)
            continue;
        ++removed;
        TAILQ_REMOVE(&manager->appqh, current, q);
        __wt_lsm_manager_free_work_unit(session, current);
    }
    __wt_spin_unlock(session, &manager->app_lock);
    /* Clear out the tree from the manager queue */
    __wt_spin_lock(session, &manager->manager_lock);
    TAILQ_FOREACH_SAFE(current, &manager->managerqh, q, tmp)
    {
        if (current->lsm_tree != lsm_tree)
            continue;
        ++removed;
        TAILQ_REMOVE(&manager->managerqh, current, q);
        __wt_lsm_manager_free_work_unit(session, current);
    }
    __wt_spin_unlock(session, &manager->manager_lock);
    WT_STAT_CONN_INCRV(session, lsm_work_units_discarded, removed);
}

/*
 * We assume this is only called from __wt_lsm_manager_pop_entry and we have session, entry and type
 * available to use. If the queue is empty we may return from the macro.
 */
#define LSM_POP_ENTRY(qh, qlock, qlen)            \
    do {                                          \
        if (TAILQ_EMPTY(qh))                      \
            return (0);                           \
        __wt_spin_lock(session, qlock);           \
        TAILQ_FOREACH (entry, (qh), q) {          \
            if (FLD_ISSET(type, entry->type)) {   \
                TAILQ_REMOVE(qh, entry, q);       \
                WT_STAT_CONN_DECR(session, qlen); \
                break;                            \
            }                                     \
        }                                         \
        __wt_spin_unlock(session, (qlock));       \
    } while (0)

/*
 * __wt_lsm_manager_pop_entry --
 *     Retrieve the head of the queue, if it matches the requested work unit type.
 */
int
__wt_lsm_manager_pop_entry(WT_SESSION_IMPL *session, uint32_t type, WT_LSM_WORK_UNIT **entryp)
{
    WT_LSM_MANAGER *manager;
    WT_LSM_WORK_UNIT *entry;

    *entryp = entry = NULL;

    manager = &S2C(session)->lsm_manager;

    /*
     * Pop the entry off the correct queue based on our work type.
     */
    if (type == WT_LSM_WORK_SWITCH)
        LSM_POP_ENTRY(&manager->switchqh, &manager->switch_lock, lsm_work_queue_switch);
    else if (type == WT_LSM_WORK_MERGE)
        LSM_POP_ENTRY(&manager->managerqh, &manager->manager_lock, lsm_work_queue_manager);
    else
        LSM_POP_ENTRY(&manager->appqh, &manager->app_lock, lsm_work_queue_app);
    if (entry != NULL)
        WT_STAT_CONN_INCR(session, lsm_work_units_done);
    *entryp = entry;
    return (0);
}

/*
 * Push a work unit onto the appropriate queue. This macro assumes we are called from
 * __wt_lsm_manager_push_entry and we have session and entry available for use.
 */
#define LSM_PUSH_ENTRY(qh, qlock, qlen)    \
    do {                                   \
        __wt_spin_lock(session, qlock);    \
        TAILQ_INSERT_TAIL((qh), entry, q); \
        WT_STAT_CONN_INCR(session, qlen);  \
        __wt_spin_unlock(session, qlock);  \
    } while (0)

/*
 * __wt_lsm_manager_push_entry --
 *     Add an entry to the end of the switch queue.
 */
int
__wt_lsm_manager_push_entry(
  WT_SESSION_IMPL *session, uint32_t type, uint32_t flags, WT_LSM_TREE *lsm_tree)
{
    WT_LSM_MANAGER *manager;
    WT_LSM_WORK_UNIT *entry;

    manager = &S2C(session)->lsm_manager;

    WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_READONLY));
    /*
     * Don't add merges or bloom filter creates if merges or bloom filters are disabled in the tree.
     */
    switch (type) {
    case WT_LSM_WORK_BLOOM:
        if (FLD_ISSET(lsm_tree->bloom, WT_LSM_BLOOM_OFF))
            return (0);
        break;
    case WT_LSM_WORK_MERGE:
        if (!F_ISSET(lsm_tree, WT_LSM_TREE_MERGES))
            return (0);
        break;
    }

    /*
     * Don't allow any work units unless a tree is active, this avoids races on shutdown between
     * clearing out queues and pushing new work units.
     *
     * Increment the queue reference before checking the flag since on close, the flag is cleared
     * and then the queue reference count is checked.
     */
    (void)__wt_atomic_add32(&lsm_tree->queue_ref, 1);
    if (!lsm_tree->active) {
        (void)__wt_atomic_sub32(&lsm_tree->queue_ref, 1);
        return (0);
    }

    (void)__wt_atomic_add64(&lsm_tree->work_count, 1);
    WT_RET(__wt_calloc_one(session, &entry));
    entry->type = type;
    entry->flags = flags;
    entry->lsm_tree = lsm_tree;
    WT_STAT_CONN_INCR(session, lsm_work_units_created);

    if (type == WT_LSM_WORK_SWITCH)
        LSM_PUSH_ENTRY(&manager->switchqh, &manager->switch_lock, lsm_work_queue_switch);
    else if (type == WT_LSM_WORK_MERGE)
        LSM_PUSH_ENTRY(&manager->managerqh, &manager->manager_lock, lsm_work_queue_manager);
    else
        LSM_PUSH_ENTRY(&manager->appqh, &manager->app_lock, lsm_work_queue_app);

    __wt_cond_signal(session, manager->work_cond);
    return (0);
}
