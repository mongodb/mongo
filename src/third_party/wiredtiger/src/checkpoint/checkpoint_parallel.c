/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __checkpoint_parallel_thread_chk --
 *     Check to decide if the checkpoint page reconciliation thread should continue running.
 */
static bool
__checkpoint_parallel_thread_chk(WT_SESSION_IMPL *session)
{
    return (FLD_ISSET(S2C(session)->server_flags, WT_CONN_SERVER_CHECKPOINT_RECONCILE_THREADS));
}

/*
 * __checkpoint_parallel_free --
 *     Free a work unit.
 */
static void
__checkpoint_parallel_free(WT_SESSION_IMPL *session, WT_CHECKPOINT_PAGE_TO_RECONCILE *entry)
{
    __wt_free(session, entry);
}

/*
 * __wt_checkpoint_parallel_push_work --
 *     Push a work unit to the queue.
 */
int
__wt_checkpoint_parallel_push_work(
  WT_SESSION_IMPL *session, WT_REF *ref, uint32_t reconcile_flags, uint32_t release_flags)
{
    WT_CHECKPOINT_PAGE_TO_RECONCILE *entry;
    WT_CHECKPOINT_RECONCILE_THREADS *ckpt_threads;
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);
    ckpt_threads = conn->ckpt_reconcile_threads;

    WT_RET(__wt_calloc_one(session, &entry));
    entry->dhandle = session->dhandle;
    entry->isolation = session->txn->isolation;
    /* All workers use the private checkpoint snapshot copied in __checkpoint_prepare. */
    entry->snapshot = &ckpt_threads->checkpoint_snapshot;
    entry->ref = ref;
    entry->reconcile_flags = reconcile_flags;
    entry->release_flags = release_flags;

    __wt_spin_lock(session, &ckpt_threads->work_lock);
    TAILQ_INSERT_TAIL(&ckpt_threads->work_qh, entry, q);
    __wt_atomic_add_uint64(&ckpt_threads->work_pushed, 1);
    __wt_spin_unlock(session, &ckpt_threads->work_lock);
    __wt_cond_signal(session, ckpt_threads->work_cond);

    return (0);
}

/*
 * __checkpoint_parallel_pop_work --
 *     Pop a work unit from the queue.
 */
static void
__checkpoint_parallel_pop_work(WT_SESSION_IMPL *session, WT_CHECKPOINT_PAGE_TO_RECONCILE **entryp)
{
    WT_CHECKPOINT_PAGE_TO_RECONCILE *entry;
    WT_CHECKPOINT_RECONCILE_THREADS *ckpt_threads;
    WT_CONNECTION_IMPL *conn;

    *entryp = entry = NULL;

    conn = S2C(session);
    ckpt_threads = conn->ckpt_reconcile_threads;

    __wt_spin_lock(session, &ckpt_threads->work_lock);

    if (TAILQ_EMPTY(&ckpt_threads->work_qh)) {
        __wt_spin_unlock(session, &ckpt_threads->work_lock);
        return;
    }

    entry = TAILQ_FIRST(&ckpt_threads->work_qh);
    TAILQ_REMOVE(&ckpt_threads->work_qh, entry, q);
    *entryp = entry;

    __wt_spin_unlock(session, &ckpt_threads->work_lock);
    return;
}

/*
 * __checkpoint_parallel_work_queue_empty --
 *     Check whether the queue for the checkpoint page reconciliation workers is empty.
 */
static bool
__checkpoint_parallel_work_queue_empty(WT_SESSION_IMPL *session)
{
    WT_CHECKPOINT_RECONCILE_THREADS *ckpt_threads;
    bool empty;

    ckpt_threads = S2C(session)->ckpt_reconcile_threads;

    __wt_spin_lock(session, &ckpt_threads->work_lock);
    empty = TAILQ_EMPTY(&ckpt_threads->work_qh);
    __wt_spin_unlock(session, &ckpt_threads->work_lock);

    return (empty);
}

/*
 * __checkpoint_parallel_done_queue_empty --
 *     Check whether the done queue for the checkpoint page reconciliation is empty.
 */
static bool
__checkpoint_parallel_done_queue_empty(WT_SESSION_IMPL *session)
{
    WT_CHECKPOINT_RECONCILE_THREADS *ckpt_threads;
    bool empty;

    ckpt_threads = S2C(session)->ckpt_reconcile_threads;

    __wt_spin_lock(session, &ckpt_threads->done_lock);
    empty = TAILQ_EMPTY(&ckpt_threads->done_qh);
    __wt_spin_unlock(session, &ckpt_threads->done_lock);

    return (empty);
}

/*
 * __checkpoint_parallel_push_done --
 *     Push a work done unit to the queue.
 */
static int
__checkpoint_parallel_push_done(WT_SESSION_IMPL *session, WT_CHECKPOINT_PAGE_TO_RECONCILE *entry)
{
    WT_CHECKPOINT_RECONCILE_THREADS *ckpt_threads;

    ckpt_threads = S2C(session)->ckpt_reconcile_threads;

    __wt_spin_lock(session, &ckpt_threads->done_lock);
    TAILQ_INSERT_TAIL(&ckpt_threads->done_qh, entry, q);
    __wt_spin_unlock(session, &ckpt_threads->done_lock);

    WT_RET(__wt_semaphore_post(session, &ckpt_threads->done_sem));
    return (0);
}

/*
 * __checkpoint_parallel_pop_done --
 *     Pop a work done unit from the queue. The caller is responsible for freeing it.
 */
static void
__checkpoint_parallel_pop_done(WT_SESSION_IMPL *session, WT_CHECKPOINT_PAGE_TO_RECONCILE **entryp)
{
    WT_CHECKPOINT_PAGE_TO_RECONCILE *entry;
    WT_CHECKPOINT_RECONCILE_THREADS *ckpt_threads;
    WT_CONNECTION_IMPL *conn;

    *entryp = entry = NULL;

    conn = S2C(session);
    ckpt_threads = conn->ckpt_reconcile_threads;

    __wt_spin_lock(session, &ckpt_threads->done_lock);

    if (TAILQ_EMPTY(&ckpt_threads->done_qh)) {
        __wt_spin_unlock(session, &ckpt_threads->done_lock);
        return;
    }

    entry = TAILQ_FIRST(&ckpt_threads->done_qh);
    TAILQ_REMOVE(&ckpt_threads->done_qh, entry, q);
    *entryp = entry;

    __wt_spin_unlock(session, &ckpt_threads->done_lock);
    return;
}

/*
 * __checkpoint_parallel_thread_run --
 *     Entry function for a checkpoint page reconciliation thread. This is called repeatedly from
 *     the thread group code, with that in mind the internal loop may seem redundant but working on
 *     the assumption that the queue is likely to have more entries we loop internally to skip
 *     expensive checks.
 */
static int
__checkpoint_parallel_thread_run(WT_SESSION_IMPL *session, WT_THREAD *thread)
{
    WT_CHECKPOINT_PAGE_TO_RECONCILE *entry;
    WT_CHECKPOINT_RECONCILE_THREADS *ckpt_threads;
    WT_DECL_RET;
    uint64_t time_rec_start;
    bool signalled;

    ckpt_threads = S2C(session)->ckpt_reconcile_threads;

    /* Mark the session as a checkpoint worker session. */
    F_SET(session, WT_SESSION_CHECKPOINT);
    F_SET(session, WT_SESSION_CHECKPOINT_WORKER);
    session->syncing = true;

    /* Wait until the next event. */
    __wt_cond_wait_signal(
      session, ckpt_threads->work_cond, WT_MILLION, __checkpoint_parallel_thread_chk, &signalled);

    for (;;) {
        __checkpoint_parallel_pop_work(session, &entry);
        if (entry == NULL)
            break;

        /*
         * Begin a transaction, if we don't already have one. This transaction will be used for the
         * entire checkpoint, similarly as in the single-threaded checkpoint case. The main
         * checkpoint thread is responsible for committing or rolling back this transaction.
         */
        if (!F_ISSET(session->txn, WT_TXN_RUNNING))
            WT_ERR(__wt_txn_begin(session, NULL));

        /* Set up the transaction for the given entry. */
        __wt_txn_import_snapshot(session, entry->snapshot);
        session->isolation = session->txn->isolation = entry->isolation;

        /* Reconcile the page. */
        time_rec_start = __wt_clock(session);
        WT_WITH_DHANDLE(session, entry->dhandle,
          ret = __wt_reconcile(session, entry->ref, NULL, entry->reconcile_flags));

        /* Update the reconciliation time and the statistics. */
        entry->reconcile_time = __wt_clock(session) - time_rec_start;
        if (ret == 0)
            WT_STAT_CONN_INCR(session, checkpoint_parallel_pages_reconciled);

        WT_ERR(ret);

err:
        entry->result = ret;
        __checkpoint_parallel_push_done(session, entry);

        if (ret != 0) {
            __wt_verbose(session, WT_VERB_CHECKPOINT,
              "Checkpoint page reconciliation thread %u failed to reconcile a page: %s (%d)",
              thread->id, __wt_strerror(session, ret, NULL, 0), ret);
            break;
        }
    }

    return (ret);
}

/*
 * __checkpoint_parallel_thread_stop --
 *     Stop the checkpoint page reconciliation threads.
 */
static int
__checkpoint_parallel_thread_stop(WT_SESSION_IMPL *session, WT_THREAD *thread)
{
    __wt_verbose(
      session, WT_VERB_CHECKPOINT, "Checkpoint page reconciliation thread %u exiting", thread->id);

    /* We should never shrink the thread group while a transaction is running. */
    WT_ASSERT(session, !F_ISSET(session->txn, WT_TXN_RUNNING));

    if (F_ISSET(session->txn, WT_TXN_RUNNING))
        WT_RET_PANIC(session, WT_VERB_CHECKPOINT,
          "Checkpoint page reconciliation thread %u stopping while a transaction is running",
          thread->id);
    return (0);
}

/*
 * __wt_checkpoint_parallel_thread_create --
 *     Start the checkpoint page reconciliation threads.
 */
int
__wt_checkpoint_parallel_thread_create(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CHECKPOINT_RECONCILE_THREADS *ckpt_threads;
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    uint32_t session_flags;
    int checkpoint_threads;

    conn = S2C(session);

    conn->ckpt_reconcile_threads = ckpt_threads = &conn->_ckpt_reconcile_threads;

    /* Get the number of checkpoint threads from the configuration. */
    WT_RET(__wt_config_gets(session, cfg, "checkpoint_threads", &cval));
    checkpoint_threads = (int)cval.val;
    if (checkpoint_threads < 1)
        checkpoint_threads = 1;

    ckpt_threads->num_threads = (uint32_t)checkpoint_threads;

    /* If the number of checkpoint threads is 1, parallel checkpoints are disabled. */
    if (checkpoint_threads == 1)
        return (0);

    /* Set first, the thread might run before we finish up. */
    FLD_SET(conn->server_flags, WT_CONN_SERVER_CHECKPOINT_RECONCILE_THREADS);

    TAILQ_INIT(&ckpt_threads->work_qh);
    WT_RET(__wt_spin_init(
      session, &ckpt_threads->work_lock, "checkpoint page reconciliation threads - work queue"));
    WT_RET(
      __wt_cond_auto_alloc(session, "checkpoint page reconciliation threads - work queue (signal)",
        10 * WT_THOUSAND, WT_MILLION, &ckpt_threads->work_cond));

    TAILQ_INIT(&ckpt_threads->done_qh);
    WT_RET(__wt_spin_init(
      session, &ckpt_threads->done_lock, "checkpoint page reconciliation threads - done queue"));

    WT_RET(__wt_semaphore_init(session, &ckpt_threads->done_sem, 0,
      "checkpoint page reconciliation threads - done queue (semaphore)"));

    /* Create the checkpoint thread group. */
    session_flags = WT_THREAD_CAN_WAIT | WT_THREAD_PANIC_FAIL;
    WT_ERR(__wt_thread_group_create(session, &ckpt_threads->thread_group,
      "checkpoint-page-reconciliation-threads", ckpt_threads->num_threads,
      ckpt_threads->num_threads, session_flags, __checkpoint_parallel_thread_chk,
      __checkpoint_parallel_thread_run, __checkpoint_parallel_thread_stop));

    if (0) {
err:
        WT_TRET(__wt_checkpoint_parallel_thread_destroy(session));
    }
    return (ret);
}

/*
 * __wt_checkpoint_parallel_thread_destroy --
 *     Destroy the checkpoint page reconciliation threads.
 */
int
__wt_checkpoint_parallel_thread_destroy(WT_SESSION_IMPL *session)
{
    WT_CHECKPOINT_RECONCILE_THREADS *ckpt_threads;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    conn = S2C(session);
    ckpt_threads = conn->ckpt_reconcile_threads;

    /* Check whether we have initialized the threads to begin with. */
    if (ckpt_threads == NULL)
        return (0);
    if (!FLD_ISSET(conn->server_flags, WT_CONN_SERVER_CHECKPOINT_RECONCILE_THREADS))
        return (0);

    /* Wait for any checkpoint thread group changes to stabilize. */
    __wt_writelock(session, &ckpt_threads->thread_group.lock);

    /*
     * Signal the threads to finish and stop populating the queue.
     */
    FLD_CLR(conn->server_flags, WT_CONN_SERVER_CHECKPOINT_RECONCILE_THREADS);
    __wt_cond_signal(session, ckpt_threads->work_cond);

    __wt_verbose(session, WT_VERB_CHECKPOINT, "%s", "Waiting for helper threads");

    /*
     * Process the done queue to release the relevant pages and free the queue entries.
     */
    ret = __wt_checkpoint_parallel_finish(session, NULL);
    if (ret != 0) {
        __wt_verbose_warning(session, WT_VERB_CHECKPOINT,
          "Checkpoint page reconciliation failed: %s", __wt_strerror(session, ret, NULL, 0));
        ret = 0; /* We don't need to communicate this error to the caller. */
    }

    /*
     * We call the destroy function still holding the write lock. It assumes it is called locked.
     */
    WT_TRET(__wt_thread_group_destroy(session, &ckpt_threads->thread_group));
    __wt_spin_destroy(session, &ckpt_threads->work_lock);
    __wt_cond_destroy(session, &ckpt_threads->work_cond);
    __wt_spin_destroy(session, &ckpt_threads->done_lock);
    WT_TRET(__wt_semaphore_destroy(session, &ckpt_threads->done_sem));

    /* Free the checkpoint snapshot buffer used by parallel workers. */
    __wt_free(session, ckpt_threads->checkpoint_snapshot_array);
    ckpt_threads->checkpoint_snapshot_capacity = 0;
    ckpt_threads->checkpoint_snapshot.snapshot = NULL;
    ckpt_threads->checkpoint_snapshot.snapshot_count = 0;

    return (ret);
}

/*
 * __wt_checkpoint_parallel_finish --
 *     Wait for the checkpoint page reconciliation workers to finish and release the acquired
 *     resources.
 */
int
__wt_checkpoint_parallel_finish(WT_SESSION_IMPL *session, uint64_t *reconcile_timep)
{
    WT_CHECKPOINT_PAGE_TO_RECONCILE *entry;
    WT_CHECKPOINT_RECONCILE_THREADS *ckpt_threads;
    WT_DECL_RET;
    uint64_t done_popped, reconcile_time, work_pushed;

    if (!WT_PARALLEL_CHECKPOINTS_ENABLED(session))
        return (0);

    /*
     * This function is called after all work has been pushed to the queue, and must wait for all
     * work to be done.
     */
    ckpt_threads = S2C(session)->ckpt_reconcile_threads;
    work_pushed = __wt_atomic_load_uint64_acquire(&ckpt_threads->work_pushed);

    done_popped = 0;
    reconcile_time = 0;
    while (work_pushed > done_popped) {
        WT_RET(__wt_semaphore_wait(session, &ckpt_threads->done_sem));

        __checkpoint_parallel_pop_done(session, &entry);
        if (entry == NULL) {
            /* We can get here if the semaphore was never posted. Try again. */
            __wt_yield();
            continue;
        }
        done_popped++;

        WT_TRET(entry->result);
        WT_TRET(__wt_page_release(session, entry->ref, entry->release_flags));
        reconcile_time += entry->reconcile_time;
        __checkpoint_parallel_free(session, entry);
    }

    if (reconcile_timep != NULL)
        *reconcile_timep = reconcile_time;

    WT_ASSERT_ALWAYS(session,
      __checkpoint_parallel_done_queue_empty(session) &&
        __checkpoint_parallel_work_queue_empty(session),
      "Checkpoint page reconciliation work queue corrupted");

    __wt_atomic_store_uint64_release(&ckpt_threads->work_pushed, 0);
    return (ret);
}

/*
 * __checkpoint_parallel_thread_release_snapshot --
 *     Release the snapshot associated with the thread.
 */
static int
__checkpoint_parallel_thread_release_snapshot(WT_SESSION_IMPL *session, WT_THREAD *thread)
{
    WT_UNUSED(thread);

    /*
     * It is reasonable to get here without having a snapshot if the thread never dequeued a work
     * item.
     */
    if (!F_ISSET(session->txn, WT_TXN_HAS_SNAPSHOT))
        return (0);

    __wt_verbose(session, WT_VERB_CHECKPOINT,
      "Checkpoint page reconciliation thread %u releasing the snapshot", thread->id);
    __wt_txn_release_snapshot(session);

    return (0);
}

/*
 * __wti_checkpoint_parallel_release_snapshot --
 *     Release all snapshots for the checkpoint page reconciliation workers.
 */
int
__wti_checkpoint_parallel_release_snapshot(WT_SESSION_IMPL *session)
{
    WT_CHECKPOINT_RECONCILE_THREADS *ckpt_threads;

    if (!WT_PARALLEL_CHECKPOINTS_ENABLED(session))
        return (0);

    WT_ASSERT_ALWAYS(session, __checkpoint_parallel_work_queue_empty(session),
      "Checkpoint page reconciliation workers still have work to do");

    ckpt_threads = S2C(session)->ckpt_reconcile_threads;
    WT_RET(__wt_thread_group_foreach(
      session, &ckpt_threads->thread_group, __checkpoint_parallel_thread_release_snapshot));
    return (0);
}

/*
 * __checkpoint_parallel_thread_commit --
 *     Commit the transaction associated with the thread.
 */
static int
__checkpoint_parallel_thread_commit(WT_SESSION_IMPL *session, WT_THREAD *thread)
{
    WT_UNUSED(thread);

    if (F_ISSET(session->txn, WT_TXN_RUNNING)) {
        __wt_verbose(session, WT_VERB_CHECKPOINT,
          "Checkpoint page reconciliation thread %u committing the transaction", thread->id);
        WT_RET(__wt_txn_commit(session, NULL));
    }

    return (0);
}

/*
 * __wti_checkpoint_parallel_commit --
 *     Commit all transactions for the checkpoint page reconciliation workers.
 */
int
__wti_checkpoint_parallel_commit(WT_SESSION_IMPL *session)
{
    WT_CHECKPOINT_RECONCILE_THREADS *ckpt_threads;

    if (!WT_PARALLEL_CHECKPOINTS_ENABLED(session))
        return (0);

    WT_ASSERT_ALWAYS(session, __checkpoint_parallel_work_queue_empty(session),
      "Checkpoint page reconciliation workers still have work to do");

    ckpt_threads = S2C(session)->ckpt_reconcile_threads;
    WT_RET(__wt_thread_group_foreach(
      session, &ckpt_threads->thread_group, __checkpoint_parallel_thread_commit));

    return (0);
}
