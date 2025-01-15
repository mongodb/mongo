/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *  All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"
#include "live_restore_private.h"

/*
 * __live_restore_worker_check --
 *     Thread groups cannot exist without a check function but in our case we don't use it due to it
 *     not meshing well with how we terminate threads. Given that, this function simply returns
 *     true.
 */
static bool
__live_restore_worker_check(WT_SESSION_IMPL *session)
{
    WT_UNUSED(session);
    return (true);
}

/*
 * __live_restore_worker_stop --
 *     When a live restore worker stops we need to manage some state. If all workers stop and the
 *     queue is empty then update the state statistic to track that.
 */
static int
__live_restore_worker_stop(WT_SESSION_IMPL *session, WT_THREAD *ctx)
{
    WT_UNUSED(ctx);
    WTI_LIVE_RESTORE_SERVER *server = S2C(session)->live_restore_server;

    __wt_spin_lock(session, &server->queue_lock);
    server->threads_working--;

    if (server->threads_working == 0) {
        /*
         * If all the threads have stopped and the queue is empty signal that the live restore is
         * complete.
         */
        if (TAILQ_EMPTY(&server->work_queue)) {
            uint64_t time_diff_ms;
            WT_STAT_CONN_SET(session, live_restore_state, WT_LIVE_RESTORE_COMPLETE);
            __wt_timer_evaluate_ms(session, &server->start_timer, &time_diff_ms);
            __wt_verbose(session, WT_VERB_LIVE_RESTORE_PROGRESS,
              "Completed restoring %" PRIu64 " files in %" PRIu64 " seconds",
              S2C(session)->live_restore_server->work_count, time_diff_ms / WT_THOUSAND);
        }
        /*
         * Future proofing: in general unless the conn is closing the queue must be empty if there
         * are zero threads working.
         */
        if (!F_ISSET(S2C(session), WT_CONN_CLOSING))
            WT_ASSERT_ALWAYS(session, TAILQ_EMPTY(&server->work_queue),
              "All background migration threads have finished but there is still work to do!");
    }
    __wt_spin_unlock(session, &server->queue_lock);

    return (0);
}

/*
 * __live_restore_free_work_item --
 *     Free a work item from the queue. Set the callers pointer to NULL. This enables unit testing
 *     and is consistent with most WiredTiger free functions.
 */
static void
__live_restore_free_work_item(WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_WORK_ITEM **work_itemp)
{
    __wt_free(session, (*work_itemp)->uri);
    __wt_free(session, *work_itemp);

    *work_itemp = NULL;
}

/*
 * __live_restore_work_queue_drain --
 *     Drain the work queue of any remaining items. This is called either on connection close, and
 *     the work will be continued after a restart, or for error handling cleanup in which case we're
 *     about to crash.
 */
static void
__live_restore_work_queue_drain(WT_SESSION_IMPL *session)
{
    WTI_LIVE_RESTORE_SERVER *server = S2C(session)->live_restore_server;

    /*
     * All contexts that call this function are single threaded however we take the lock as that is
     * the correct semantic and will future proof the code.
     */
    __wt_spin_lock(session, &server->queue_lock);
    if (!TAILQ_EMPTY(&server->work_queue)) {
        WTI_LIVE_RESTORE_WORK_ITEM *work_item = NULL, *work_item_tmp = NULL;
        TAILQ_FOREACH_SAFE(work_item, &server->work_queue, q, work_item_tmp)
        {
            TAILQ_REMOVE(&server->work_queue, work_item, q);
            __live_restore_free_work_item(session, &work_item);
        }
    }
    WT_ASSERT_ALWAYS(
      session, TAILQ_EMPTY(&server->work_queue), "Live restore work queue failed to drain");
    __wt_spin_unlock(session, &server->queue_lock);
}

/*
 * __live_restore_worker_run --
 *     Entry function for a live restore thread. This is called repeatedly from the thread group
 *     code so it does not need to loop itself. Each run will fill the holes for a single file.
 */
static int
__live_restore_worker_run(WT_SESSION_IMPL *session, WT_THREAD *ctx)
{
    WT_DECL_RET;
    WTI_LIVE_RESTORE_SERVER *server = S2C(session)->live_restore_server;
    uint64_t time_diff_ms;

    __wt_spin_lock(session, &server->queue_lock);
    if (TAILQ_EMPTY(&server->work_queue)) {
        /* Stop our thread from running. This will call the stop_func and trigger state cleanup. */
        F_CLR(ctx, WT_THREAD_RUN);
        __wt_verbose_debug1(session, WT_VERB_LIVE_RESTORE, "%s", "Live restore worker terminating");
        __wt_spin_unlock(session, &server->queue_lock);
        return (0);
    }
    WTI_LIVE_RESTORE_WORK_ITEM *work_item = TAILQ_FIRST(&server->work_queue);
    WT_ASSERT(session, work_item != NULL);
    TAILQ_REMOVE(&server->work_queue, work_item, q);
    __wt_verbose_debug2(
      session, WT_VERB_LIVE_RESTORE, "Live restore worker taking queue item: %s", work_item->uri);
    __wt_timer_evaluate_ms(session, &server->msg_timer, &time_diff_ms);

    /* Print out a progress message periodically. */
    if ((time_diff_ms / (WT_THOUSAND * WT_PROGRESS_MSG_PERIOD)) > server->msg_count) {
        __wt_verbose(session, WT_VERB_LIVE_RESTORE_PROGRESS,
          "Live restore has been running for %" PRIu64 " seconds and has %" PRIu64
          " files of %" PRIu64 " left to process",
          time_diff_ms / WT_THOUSAND, server->work_items_remaining, server->work_count);
        server->msg_count = time_diff_ms / (WT_THOUSAND * WT_PROGRESS_MSG_PERIOD);
    }

    __wt_spin_unlock(session, &server->queue_lock);

    /*
     * Open a cursor so no one can get exclusive access on the object. This prevents concurrent
     * schema operations like drop. If the file no longer exists we don't need to copy anything and
     * can return a success.
     */
    WT_CURSOR *cursor;
    WT_SESSION *wt_session = (WT_SESSION *)session;
    ret = wt_session->open_cursor(wt_session, work_item->uri, NULL, NULL, &cursor);
    if (ret == ENOENT) {
        /* Free the work item. */
        __live_restore_free_work_item(session, &work_item);
        return (0);
    }
    WT_RET(ret);

    /*
     * We need to get access to the WiredTiger file handle. Given we've opened the cursor we should
     * be able to access the WT_FH by first getting to its block manager and then the WT_FH.
     */
    WT_BM *bm = CUR2BT(cursor)->bm;
    WT_ASSERT(session, bm->is_multi_handle == false);

    /* FIXME-WT-13897 Replace this with an API call into the block manager. */
    WT_FILE_HANDLE *fh = bm->block->fh->handle;

    __wt_verbose_debug2(
      session, WT_VERB_LIVE_RESTORE, "Live restore worker: Filling holes in %s", work_item->uri);
    ret = __wti_live_restore_fs_fill_holes(fh, wt_session);
    __wt_verbose_debug1(session, WT_VERB_LIVE_RESTORE,
      "Live restore worker: Finished finished filling holes in %s", work_item->uri);

    /* Free the work item. */
    __live_restore_free_work_item(session, &work_item);
    WT_STAT_CONN_SET(
      session, live_restore_work_remaining, __wt_atomic_sub64(&server->work_items_remaining, 1));
    WT_TRET(cursor->close(cursor));
    return (ret);
}

/*
 * __insert_queue_item --
 *     Insert an item into the live restore queue.
 */
static int
__insert_queue_item(WT_SESSION_IMPL *session, char *uri, uint64_t *work_count)
{
    WT_DECL_RET;

    __wt_verbose_debug2(
      session, WT_VERB_LIVE_RESTORE, "Live restore server: Adding %s to the work queue", uri);

    WTI_LIVE_RESTORE_SERVER *server = S2C(session)->live_restore_server;
    WTI_LIVE_RESTORE_WORK_ITEM *work_item = NULL;

    WT_ERR(__wt_calloc_one(session, &work_item));
    WT_ERR(__wt_strdup(session, uri, &work_item->uri));
    TAILQ_INSERT_HEAD(&server->work_queue, work_item, q);
    (*work_count)++;

    if (0) {
err:
        __wt_free(session, work_item->uri);
        __wt_free(session, work_item);
    }

    return (ret);
}

/*
 * __live_restore_init_work_queue --
 *     Populate the live restore work queue. Free all objects on failure.
 */
static int
__live_restore_init_work_queue(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn = S2C(session);
    WT_DECL_RET;
    WTI_LIVE_RESTORE_SERVER *server = conn->live_restore_server;

    /* Initialize the work queue. */
    TAILQ_INIT(&server->work_queue);
    __wt_verbose_debug1(
      session, WT_VERB_FILEOPS, "%s", "Live restore server: Initializing the work queue");

    WT_CURSOR *cursor;
    WT_RET(__wt_metadata_cursor(session, &cursor));
    uint64_t work_count = 0;
    while ((ret = cursor->next(cursor)) == 0) {
        char *uri = NULL;
        WT_ERR(cursor->get_key(cursor, &uri));
        if (WT_PREFIX_MATCH(uri, "file:"))
            WT_ERR(__insert_queue_item(session, uri, &work_count));
    }
    WT_ERR_NOTFOUND_OK(ret, false);

    /*
     * The first step on restoration from a backup is to rebuild the metadata file. Thus if we are
     * restoring from a backup we don't need to queue it. Otherwise we need to ensure we transfer it
     * over.
     */
    if (!F_ISSET(conn, WT_CONN_BACKUP_PARTIAL_RESTORE))
        WT_ERR(__insert_queue_item(session, (char *)("file:" WT_METAFILE), &work_count));

    WT_STAT_CONN_SET(session, live_restore_work_remaining, work_count);
    __wt_atomic_store64(&conn->live_restore_server->work_count, work_count);
    __wt_atomic_store64(&conn->live_restore_server->work_items_remaining, work_count);

    if (0) {
err:
        /* Something broke, drain the queue. */
        __live_restore_work_queue_drain(session);
    }
    return (ret);
}

/*
 * __wt_live_restore_server_create --
 *     Start the worker threads and build the work queue.
 */
int
__wt_live_restore_server_create(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_DECL_RET;
    /*
     * Check that we have a live restore file system before starting the threads or allocating the
     * the server.
     */
    if (!F_ISSET(S2C(session), WT_CONN_LIVE_RESTORE_FS))
        return (0);

    WT_CONNECTION_IMPL *conn = S2C(session);
    WT_ERR(__wt_calloc_one(session, &conn->live_restore_server));

    /* Read the threads_max config, zero threads is valid in which case we don't do anything. */
    WT_CONFIG_ITEM cval;
    WT_ERR(__wt_config_gets(session, cfg, "live_restore.threads_max", &cval));
    if (cval.val == 0)
        return (0);

    WT_ERR(__wt_spin_init(
      session, &conn->live_restore_server->queue_lock, "live restore migration work queue"));

    /*
     * Set the in progress state before we run the threads. If we do it after there's a chance we'll
     * context switch and then this state will happen after the finish state. By setting it here it
     * also means we transition through all valid states.
     */
    WT_STAT_CONN_SET(session, live_restore_state, WT_LIVE_RESTORE_IN_PROGRESS);

    /*
     * Even if we start from an empty database the history store file will exist before we get here
     * which means there will always be at least one item in the queue.
     */
    WT_ERR(__live_restore_init_work_queue(session));

    /* Set this value before the threads start up in case they immediately decrement it. */
    conn->live_restore_server->threads_working = (uint32_t)cval.val;
    /*
     * Create the thread group.
     *
     * To force WT_THREAD_ACTIVE to be set on the threads we specify min_thread_count to be equal to
     * max_thread_count. This will prevent a 10 second wait from occurring per loop iteration.
     *
     * Furthermore because our threads terminate themselves the scaling logic may not be possible
     * without some adjustments to either the live restore server or the thread group code itself.
     */
    __wt_timer_start(session, &conn->live_restore_server->msg_timer);
    conn->live_restore_server->start_timer = conn->live_restore_server->msg_timer;
    __wt_verbose(session, WT_VERB_LIVE_RESTORE_PROGRESS,
      "Starting %" PRId64 " threads to restore %" PRIu64 " files", cval.val,
      conn->live_restore_server->work_count);
    WT_ERR(__wt_thread_group_create(session, &conn->live_restore_server->threads,
      "live_restore_workers", (uint32_t)cval.val, (uint32_t)cval.val, 0,
      __live_restore_worker_check, __live_restore_worker_run, __live_restore_worker_stop));

    if (0) {
err:
        __wt_free(session, conn->live_restore_server);
    }
    return (ret);
}

/*
 * __wt_live_restore_server_destroy --
 *     Destroy the live restore threads.
 */
int
__wt_live_restore_server_destroy(WT_SESSION_IMPL *session)
{
    WTI_LIVE_RESTORE_SERVER *server = S2C(session)->live_restore_server;

    /*
     * If we didn't create a live restore file system or the server there is nothing to do, it is
     * rare, but possible, to arrive here with the flag set and a NULL server. This situation
     * happens when an error is encountered after the file system initialization but before the
     * server is created.
     */
    if (!F_ISSET(S2C(session), WT_CONN_LIVE_RESTORE_FS) || server == NULL)
        return (0);

    /*
     * It is possible to get here without ever starting the thread group. Ensure that it has been
     * created before destroying it. One such case would be if we configure the live restore file
     * system. But then an error occurs and we never initialize the server before destroying it.
     */
    if (server->threads.wait_cond != NULL) {
        /* Let any running threads finish up. */
        __wt_cond_signal(session, server->threads.wait_cond);
        __wt_writelock(session, &server->threads.lock);
        /*
         * This call destroys the thread group lock, in theory it can fail and we will not free any
         * further items. Given we are in a failure state this is okay.
         */
        WT_RET(__wt_thread_group_destroy(session, &server->threads));
        __live_restore_work_queue_drain(session);
        __wt_spin_destroy(session, &server->queue_lock);
    }
    __wt_free(session, server);
    return (0);
}
