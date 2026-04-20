/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __rts_phase_string --
 *     Return a human-readable string for an RTS phase.
 */
static const char *
__rts_phase_string(uint32_t phase)
{
    switch (phase) {
    case WT_RTS_PHASE_INACTIVE:
        return ("INACTIVE");
    case WT_RTS_PHASE_METADATA_COUNT:
        return ("METADATA_COUNT");
    case WT_RTS_PHASE_BTREE_APPLY:
        return ("BTREE_APPLY");
    case WT_RTS_PHASE_QUEUE_DRAIN:
        return ("QUEUE_DRAIN");
    case WT_RTS_PHASE_HS_FINAL_PASS:
        return ("HS_FINAL_PASS");
    case WT_RTS_PHASE_COMPLETE:
        return ("COMPLETE");
    default:
        return ("UNKNOWN");
    }
}

/*
 * __rts_emit_overall_progress --
 *     Emit an overall RTS progress message with counters, percentage, and throughput.
 */
static void
__rts_emit_overall_progress(WT_SESSION_IMPL *session)
{
    WT_ROLLBACK_TO_STABLE *rts;
    uint64_t btrees_completed, btrees_processed, btrees_skipped, elapsed_ms, pages_per_sec,
      pages_walked, pct, total_btrees;
    uint32_t phase;

    rts = S2C(session)->rts;

    btrees_processed = __wt_atomic_load_uint64_relaxed(&rts->progress.btrees_processed);
    btrees_skipped = __wt_atomic_load_uint64_relaxed(&rts->progress.btrees_skipped);
    pages_walked = __wt_atomic_load_uint64_relaxed(&rts->progress.pages_walked);
    phase = __wt_atomic_load_uint32_relaxed(&rts->progress.phase);
    total_btrees = rts->progress.total_btrees;

    btrees_completed = btrees_processed + btrees_skipped;
    __wt_timer_evaluate_ms(session, &rts->progress.start_timer, &elapsed_ms);
    pct = total_btrees > 0 ? (100 * btrees_completed / total_btrees) : 0;
    pages_per_sec = elapsed_ms > 0 ? (pages_walked * WT_THOUSAND / elapsed_ms) : 0;

    __wt_verbose(session, WT_VERB_RECOVERY_PROGRESS,
      "Rollback to stable [%s] overall: running for %" PRIu64 " seconds, %" PRIu64 " of %" PRIu64
      " btrees done (%" PRIu64 "%%), %" PRIu64 " processed, %" PRIu64 " skipped, %" PRIu64
      " total pages walked (%" PRIu64 " pages/sec)",
      __rts_phase_string(phase), elapsed_ms / WT_THOUSAND, btrees_completed, total_btrees, pct,
      btrees_processed, btrees_skipped, pages_walked, pages_per_sec);

    /* Notify the application via the progress callback. */
    WT_IGNORE_RET(__wt_progress(session, "rollback to stable", btrees_completed));
}

/*
 * __rts_progress_msg --
 *     Log a verbose message about the overall progress of rollback to stable. Called from the
 *     metadata walk loop in __wti_rts_btree_apply_all.
 */
static void
__rts_progress_msg(WT_SESSION_IMPL *session)
{
    WT_ROLLBACK_TO_STABLE *rts;
    uint64_t clock_now, overall_last;

    rts = S2C(session)->rts;
    clock_now = __wt_clock(session);
    overall_last = __wt_atomic_load_uint64_relaxed(&rts->progress.overall_last_report);
    if (__wt_clock_to_nsec(clock_now, overall_last) >=
        (uint64_t)WT_BILLION * WT_PROGRESS_MSG_PERIOD &&
      __wt_atomic_cas_uint64(&rts->progress.overall_last_report, overall_last, clock_now))
        __rts_emit_overall_progress(session);
}

/*
 * __wti_rts_progress_msg_walk --
 *     Log a verbose message about the progress of a per-btree page walk during rollback to stable.
 *     Uses the normalized position (npos) of the current page to compute a percentage within the
 *     tree and estimate time remaining for this btree.
 */
void
__wti_rts_progress_msg_walk(WT_SESSION_IMPL *session, uint64_t btree_start_clock,
  uint64_t *last_report_clock, double npos, uint64_t btree_pages)
{
    WT_ROLLBACK_TO_STABLE *rts;
    uint64_t btree_eta_sec, btree_pct, btree_pages_per_sec, clock_now, elapsed_ns, elapsed_sec,
      overall_last;
    uint32_t phase;

    rts = S2C(session)->rts;

    /* Use total btree walk time (not interval) for pages/sec and ETA calculations. */
    clock_now = __wt_clock(session);
    elapsed_ns = __wt_clock_to_nsec(clock_now, btree_start_clock);
    elapsed_sec = elapsed_ns / WT_BILLION;

    phase = __wt_atomic_load_uint32_relaxed(&rts->progress.phase);
    btree_pct = (uint64_t)(npos * 100);
    btree_pages_per_sec = elapsed_sec > 0 ? (btree_pages / elapsed_sec) : 0;

    /* Estimate time remaining for this btree based on npos progression. */
    btree_eta_sec = 0;
    if (npos > 0.05 && npos < 1.0)
        btree_eta_sec = (uint64_t)((1.0 - npos) * (double)elapsed_sec / npos);

    if (btree_eta_sec > 0)
        __wt_verbose(session, WT_VERB_RECOVERY_PROGRESS,
          "Rollback to stable [%s] btree walk on %s for %" PRIu64 " seconds, %" PRIu64
          "%% through btree, %" PRIu64 " pages walked (%" PRIu64
          " pages/sec)"
          ", btree ETA %" PRIu64 " seconds",
          __rts_phase_string(phase), session->dhandle->name, elapsed_sec, btree_pct, btree_pages,
          btree_pages_per_sec, btree_eta_sec);
    else
        __wt_verbose(session, WT_VERB_RECOVERY_PROGRESS,
          "Rollback to stable [%s] btree walk on %s for %" PRIu64 " seconds, %" PRIu64
          "%% through btree, %" PRIu64 " pages walked (%" PRIu64 " pages/sec)",
          __rts_phase_string(phase), session->dhandle->name, elapsed_sec, btree_pct, btree_pages,
          btree_pages_per_sec);

    /*
     * Emit an overall progress line. Use CAS on overall_last_report so that exactly one thread wins
     * per reporting period, regardless of which thread it is. Skip if progress was not initialized
     * (e.g., single-file RTS via rollback_to_stable_one).
     */
    if (rts->progress.total_btrees > 0) {
        overall_last = __wt_atomic_load_uint64_relaxed(&rts->progress.overall_last_report);
        if (__wt_clock_to_nsec(clock_now, overall_last) >=
            (uint64_t)WT_BILLION * WT_PROGRESS_MSG_PERIOD &&
          __wt_atomic_cas_uint64(&rts->progress.overall_last_report, overall_last, clock_now))
            __rts_emit_overall_progress(session);
    }

    *last_report_clock = clock_now;
}

/*
 * __rts_progress_init --
 *     Initialize the RTS progress tracking fields.
 */
static void
__rts_progress_init(WT_SESSION_IMPL *session)
{
    WT_ROLLBACK_TO_STABLE *rts;

    rts = S2C(session)->rts;
    WT_CLEAR(rts->progress);
    __wt_timer_start(session, &rts->progress.start_timer);
    __wt_atomic_store_uint64_relaxed(&rts->progress.overall_last_report, __wt_clock(session));
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
    uint64_t max_count;
    char ts_string[WT_TS_INT_STRING_SIZE];
    const char *config, *saved_session_name, *uri;
    bool have_cursor, rts_threads_started;

    __rts_progress_init(session);
    __wt_atomic_store_uint32_relaxed(
      &S2C(session)->rts->progress.phase, WT_RTS_PHASE_METADATA_COUNT);

    max_count = 0;
    saved_session_name = session->name;
    rts_threads_started = false;

    /*
     * Walk the metadata first to count how many files we have overall. That allows us to give
     * signal about progress.
     */
    WT_RET(__wt_metadata_cursor(session, &cursor));
    have_cursor = true;
    while ((ret = cursor->next(cursor)) == 0) {
        WT_ERR(cursor->get_key(cursor, &uri));
        if (WT_BTREE_PREFIX(uri) && !WT_IS_URI_HS(uri) && !WT_IS_URI_METADATA(uri))
            ++max_count;
    }
    WT_ERR_NOTFOUND_OK(ret, false);
    WT_ERR(__wt_metadata_cursor_release(session, &cursor));
    have_cursor = false;

    S2C(session)->rts->progress.total_btrees = max_count;
    __wt_verbose(session, WT_VERB_RECOVERY_PROGRESS,
      "Rollback to stable found %" PRIu64 " btrees to process", max_count);

    __wt_atomic_store_uint32_relaxed(&S2C(session)->rts->progress.phase, WT_RTS_PHASE_BTREE_APPLY);

    WT_ERR(__rts_thread_create(session));
    rts_threads_started = true;

    WT_ERR(__wt_metadata_cursor(session, &cursor));
    have_cursor = true;
    while ((ret = cursor->next(cursor)) == 0) {
        /* Log a progress message. */
        WT_ERR(cursor->get_key(cursor, &uri));
        WT_ERR(cursor->get_value(cursor, &config));
        __rts_progress_msg(session);

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
        __wt_atomic_store_uint32_relaxed(
          &S2C(session)->rts->progress.phase, WT_RTS_PHASE_QUEUE_DRAIN);
        __wt_verbose(session, WT_VERB_RECOVERY_PROGRESS, "%s",
          "Rollback to stable finished metadata walk, draining worker queue");

        /* Rename session while joining workers so log messages identify us as a worker. */
        session->name = "rts-main-wk";
        while (!TAILQ_EMPTY(&S2C(session)->rts->rtsqh)) {
            __wti_rts_pop_work(session, &entry);
            if (entry == NULL)
                break;
            ret = __wti_rts_btree_work_unit(session, entry);
            __wti_rts_work_free(session, entry);
            WT_ERR(ret);
        }
        session->name = saved_session_name;
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
        __wt_atomic_store_uint32_relaxed(
          &S2C(session)->rts->progress.phase, WT_RTS_PHASE_HS_FINAL_PASS);
        __wt_verbose(session, WT_VERB_RECOVERY_PROGRESS, "%s",
          "Rollback to stable beginning history store final pass");
        __wt_verbose_level_multi(session, WT_VERB_RECOVERY_RTS(session), WT_VERBOSE_DEBUG_3,
          WT_RTS_VERB_TAG_HS_TREE_FINAL_PASS
          "performing final pass of the history store to remove unstable entries with "
          "rollback_timestamp=%s",
          __wt_timestamp_to_string(rollback_timestamp, ts_string));
        WT_ERR(__wti_rts_history_final_pass(session, rollback_timestamp));
    }

    __wt_atomic_store_uint32_relaxed(&S2C(session)->rts->progress.phase, WT_RTS_PHASE_COMPLETE);
err:
    session->name = saved_session_name;
    if (have_cursor)
        WT_TRET(__wt_metadata_cursor_release(session, &cursor));
    if (rts_threads_started)
        WT_TRET(__rts_thread_destroy(session));
    return (ret);
}
