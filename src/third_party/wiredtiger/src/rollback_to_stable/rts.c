/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wti_rts_progress_msg --
 *     Log a verbose message about the progress of the current rollback to stable.
 */
void
__wti_rts_progress_msg(WT_SESSION_IMPL *session, WT_TIMER *rollback_start, uint64_t rollback_count,
  uint64_t max_count, uint64_t *rollback_msg_count, bool walk)
{
    uint64_t time_diff_ms;

    /* Time since the rollback started. */
    __wt_timer_evaluate_ms(session, rollback_start, &time_diff_ms);

    if ((time_diff_ms / (WT_THOUSAND * WT_PROGRESS_MSG_PERIOD)) > *rollback_msg_count) {
        if (walk)
            __wt_verbose(session, WT_VERB_RECOVERY_PROGRESS,
              "Rollback to stable has been performing on %s for %" PRIu64
              " milliseconds. For more detailed logging, enable WT_VERB_RTS ",
              session->dhandle->name, time_diff_ms);
        else
            __wt_verbose(session, WT_VERB_RECOVERY_PROGRESS,
              "Rollback to stable has been running for %" PRIu64
              " milliseconds and has inspected %" PRIu64 " files of %" PRIu64
              ". For more detailed logging, enable WT_VERB_RTS",
              time_diff_ms, rollback_count, max_count);
        *rollback_msg_count = time_diff_ms / (WT_THOUSAND * WT_PROGRESS_MSG_PERIOD);
    }
}

/*
 * __rts_thread_chk --
 *     Check to decide if the RTS thread should continue running.
 */
static bool
__rts_thread_chk(WT_SESSION_IMPL *session)
{
    return (FLD_ISSET(S2C(session)->server_flags, WT_CONN_SERVER_RTS));
}

/*
 * __rts_thread_run --
 *     Entry function for an RTS thread. This is called repeatedly from the thread group code so it
 *     does not need to loop itself.
 */
static int
__rts_thread_run(WT_SESSION_IMPL *session, WT_THREAD *thread)
{
    WT_DECL_RET;
    WT_RTS_WORK_UNIT *entry;

    WT_UNUSED(thread);

    /* Wait here. */
    if (FLD_ISSET(S2C(session)->server_flags, WT_CONN_SERVER_RTS))
        __wt_cond_wait(session, S2C(session)->rts->thread_group.wait_cond, 10 * WT_THOUSAND, NULL);

    /* Mark the RTS thread session as a rollback to stable session. */
    F_SET(session, WT_SESSION_ROLLBACK_TO_STABLE);

    while (!TAILQ_EMPTY(&S2C(session)->rts->rtsqh)) {
        __wti_rts_pop_work(session, &entry);
        if (entry == NULL)
            break;

        ret = __wti_rts_btree_work_unit(session, entry);
        __wti_rts_work_free(session, entry);
        WT_ERR(ret);
    }

    if (0) {
err:
        WT_RET_PANIC(session, ret, "rts thread error");
    }
    return (ret);
}

/*
 * __rts_thread_stop --
 *     Shutdown function for an RTS thread.
 */
static int
__rts_thread_stop(WT_SESSION_IMPL *session, WT_THREAD *thread)
{
    WT_UNUSED(thread);

    /* Clear the RTS thread session flag. */
    F_CLR(session, WT_SESSION_ROLLBACK_TO_STABLE);
    return (0);
}

/*
 * __rts_thread_create --
 *     Start RTS threads.
 */
static int
__rts_thread_create(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    uint32_t session_flags;

    conn = S2C(session);

    if (conn->rts->threads_num == 0)
        return (0);

    /* Set first, the thread might run before we finish up. */
    FLD_SET(conn->server_flags, WT_CONN_SERVER_RTS);

    /* RTS work unit list */
    TAILQ_INIT(&conn->rts->rtsqh);
    WT_RET(__wt_spin_init(session, &conn->rts->rts_lock, "RTS work unit list"));

    /* Create the RTS thread group. Set the group size to the maximum allowed sessions. */
    session_flags = WT_THREAD_CAN_WAIT | WT_THREAD_PANIC_FAIL;
    WT_RET(__wt_thread_group_create(session, &conn->rts->thread_group, "rts-threads",
      conn->rts->threads_num, conn->rts->threads_num, session_flags, __rts_thread_chk,
      __rts_thread_run, __rts_thread_stop));

    return (0);
}

/*
 * __rts_thread_destroy --
 *     Destroy the RTS threads.
 */
static int
__rts_thread_destroy(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    conn = S2C(session);

    if (conn->rts->threads_num == 0)
        return (0);

    /* Wait for any RTS thread group changes to stabilize. */
    __wt_writelock(session, &conn->rts->thread_group.lock);

    /* Signal the threads to finish. */
    FLD_CLR(conn->server_flags, WT_CONN_SERVER_RTS);
    __wt_cond_signal(session, conn->rts->thread_group.wait_cond);

    __wt_verbose(
      session, WT_VERB_RTS, WT_RTS_VERB_TAG_WAIT_THREADS "%s", "waiting for helper threads");

    /* We call the destroy function still holding the write lock. It assumes it is called locked. */
    WT_TRET(__wt_thread_group_destroy(session, &conn->rts->thread_group));
    __wt_spin_destroy(session, &conn->rts->rts_lock);

    return (ret);
}

/*
 * __wti_rts_btree_apply_all --
 *     Perform rollback to stable to all files listed in the metadata, apart from the metadata and
 *     history store files.
 */
int
__wti_rts_btree_apply_all(WT_SESSION_IMPL *session, wt_timestamp_t rollback_timestamp)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_RTS_WORK_UNIT *entry;
    WT_TIMER timer;
    uint64_t max_count, rollback_count, rollback_msg_count;
    char ts_string[WT_TS_INT_STRING_SIZE];
    const char *config, *uri;
    bool have_cursor, rts_threads_started;

    __wt_timer_start(session, &timer);
    max_count = rollback_count = 0;
    rollback_msg_count = 0;
    rts_threads_started = false;

    /*
     * Walk the metadata first to count how many files we have overall. That allows us to give
     * signal about progress.
     */
    WT_RET(__wt_metadata_cursor(session, &cursor));
    have_cursor = true;
    while ((ret = cursor->next(cursor)) == 0) {
        WT_ERR(cursor->get_key(cursor, &uri));
        if (WT_BTREE_PREFIX(uri))
            ++max_count;
    }
    WT_ERR_NOTFOUND_OK(ret, false);
    WT_ERR(__wt_metadata_cursor_release(session, &cursor));
    have_cursor = false;

    WT_ERR(__rts_thread_create(session));
    rts_threads_started = true;

    WT_ERR(__wt_metadata_cursor(session, &cursor));
    have_cursor = true;
    while ((ret = cursor->next(cursor)) == 0) {
        /* Log a progress message. */
        WT_ERR(cursor->get_key(cursor, &uri));
        WT_ERR(cursor->get_value(cursor, &config));
        if (WT_BTREE_PREFIX(uri))
            ++rollback_count;
        __wti_rts_progress_msg(
          session, &timer, rollback_count, max_count, &rollback_msg_count, false);

        F_SET(session, WT_SESSION_QUIET_CORRUPT_FILE);
        ret = __wti_rts_btree_walk_btree_apply(session, uri, config, rollback_timestamp);
        F_CLR(session, WT_SESSION_QUIET_CORRUPT_FILE);

        WT_ERR(ret);
    }
    WT_ERR_NOTFOUND_OK(ret, false);

    /*
     * Wait until the entire RTS queue is finished processing before performing the history store
     * final pass. Moreover, the main thread joins the processing queue rather than waiting for the
     * workers alone to complete the task.
     */
    if (S2C(session)->rts->threads_num != 0) {
        while (!TAILQ_EMPTY(&S2C(session)->rts->rtsqh)) {
            __wti_rts_pop_work(session, &entry);
            if (entry == NULL)
                break;
            ret = __wti_rts_btree_work_unit(session, entry);
            __wti_rts_work_free(session, entry);
            WT_ERR(ret);
        }
    }

    WT_ERR(__rts_thread_destroy(session));
    rts_threads_started = false;

    /*
     * Performing eviction in parallel to a checkpoint can lead to a situation where the history
     * store has more updates than its corresponding data store. Performing history store cleanup at
     * the end can enable the removal of any such unstable updates that are written to the history
     * store.
     *
     * Do not perform the final pass on the history store in an in-memory configuration as it
     * doesn't exist.
     */
    if (!F_ISSET(S2C(session), WT_CONN_IN_MEMORY)) {
        __wt_verbose_level_multi(session, WT_VERB_RECOVERY_RTS(session), WT_VERBOSE_DEBUG_3,
          WT_RTS_VERB_TAG_HS_TREE_FINAL_PASS
          "performing final pass of the history store to remove unstable entries with "
          "rollback_timestamp=%s",
          __wt_timestamp_to_string(rollback_timestamp, ts_string));
        WT_ERR(__wti_rts_history_final_pass(session, rollback_timestamp));
    }
err:
    if (have_cursor)
        WT_TRET(__wt_metadata_cursor_release(session, &cursor));
    if (rts_threads_started)
        WT_TRET(__rts_thread_destroy(session));
    return (ret);
}
