/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "format.h"
#include <sys/mman.h>

/*
 * disagg_redirect_output --
 *     Redirect output to a file in the run directory, unless running quietly.
 */
static void
disagg_redirect_output(const char *output_file)
{
    char path[256];

    testutil_snprintf(path, sizeof(path), "%s/%s", g.home, output_file);

    printf("===> Output will be written to %s\n", path);
    printf("     (If you want to watch live, run: tail -f %s)\n\n", path);
    fflush(stdout);

    if (freopen(path, "w", stdout) == NULL)
        testutil_die(errno, "freopen stdout %s", path);
    if (dup2(fileno(stdout), fileno(stderr)) == -1)
        testutil_die(errno, "dup2 stderr->stdout");

    __wt_stream_set_no_buffer(stdout);
    __wt_stream_set_no_buffer(stderr);
}

/*
 * disagg_teardown_multi_node --
 *     Wait for and clean up any follower processes if we're in multi-node disagg mode.
 */
void
disagg_teardown_multi_node(void)
{
    if (!disagg_is_multi_node())
        return;

    if (g.follower_pid > 0) { /* Parent: leader */
        /* Wait for the follower process to exit. */
        track("Waiting for follower to finish execution.", 0ULL);
        testutil_timeout_wait(720, g.follower_pid);
        g.follower_pid = 0;
    }
    close(g.disagg_multi_sync_socket);
    testutil_check(munmap(g.disagg_multi_db_hash, sizeof(DISAGG_MULTI_DB_HASH)));
    g.disagg_multi_db_hash = NULL;
}

/*
 * disagg_setup_multi_node --
 *     Set up the environment for multi-node disagg, forking follower processes as needed.
 */
void
disagg_setup_multi_node(void)
{
    pid_t pid;
    int sv[2];
    char follower_home[256];

    if (!disagg_is_multi_node())
        return;

    testutil_snprintf(follower_home, sizeof(follower_home), "%s/follower", g.home);
    memset(&g.checkpoint_metadata, 0, sizeof(g.checkpoint_metadata));

    /*
     * Create required dir before forking to avoid parent/child races. Skip on reopen, since the run
     * directories already exist.
     */
    if (!g.reopen) {
        testutil_recreate_dir(g.home);
        testutil_mkdir(follower_home);
    }

    /* Initialize a shared page log directory path for all nodes. */
    testutil_snprintf(g.home_page_log, sizeof(g.home_page_log), "%s", g.home);
    /*
     * Allocate a shared memory region to hold hash values shared between leader and follower
     * processes, used by disagg multi node tests to validate data consistency.
     */
    g.disagg_multi_db_hash = mmap(NULL, sizeof(DISAGG_MULTI_DB_HASH), PROT_READ | PROT_WRITE,
      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    testutil_assert_errno(g.disagg_multi_db_hash != MAP_FAILED);

    /* Create a socket pair for leader-follower synchronization.*/
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == -1)
        testutil_die(errno, "Failed to create socket pair for leader-follower sync");

    fflush(NULL);
    pid = fork();
    testutil_assert_errno(pid >= 0);
    if (pid == 0) { /* Child: follower */
        progname = "t[follower]";
        config_single(NULL, "disagg.mode=follower", true);
        path_setup(follower_home);
        disagg_redirect_output("follower.out");
        close(sv[0]);
        g.disagg_multi_sync_socket = sv[1];
    } else { /* Parent: leader */
        progname = "t[leader]";
        config_single(NULL, "disagg.mode=leader", true);
        disagg_redirect_output("leader.out");
        close(sv[1]);
        g.disagg_multi_sync_socket = sv[0];
    }

    g.follower_pid = pid;
}

/*
 * disagg_multi_sync_point --
 *     Synchronization point in disagg multi-node setup for leader-follower.
 */
static void
disagg_multi_sync_point(void)
{
    char send = 'S'; /* S for sync */
    char recv;

    /* Signal from leader or follower to synchronize. */
    if (write(g.disagg_multi_sync_socket, &send, 1) != 1)
        testutil_die(errno, "disagg_multi_sync_point: write");

    track("Reached sync point. Waiting for other process...", 0ULL);

    /* Wait for synchronization signal from the other process. */
    if (read(g.disagg_multi_sync_socket, &recv, 1) != 1)
        testutil_die(errno, "disagg_multi_sync_point: read");
}

/*
 * disagg_sync_multi_node --
 *     Synchronization point in disagg multi-node setup for leader-follower data validation.
 */
void
disagg_sync_multi_node(WT_SESSION *session)
{
    uint64_t hash = 0;
    if (!disagg_is_multi_node())
        return;

    if (GV(DISAGG_MULTI_VALIDATION)) {
        hash = checksum_database(session);
        if (g.disagg_leader)
            g.disagg_multi_db_hash->leader_hash = hash;
        else
            g.disagg_multi_db_hash->follower_hash = hash;
    }

    /* Initial synchronization between leader and follower processes. */
    disagg_multi_sync_point();

    if (GV(DISAGG_MULTI_VALIDATION)) {
        /*
         * If there's a mismatch, then we're going to assert. Before we do, preserve the state of
         * ingest and stable tables.
         */

        bool hash_match =
          g.disagg_multi_db_hash->leader_hash == g.disagg_multi_db_hash->follower_hash;
        if (!hash_match && GV(DISAGG_PRESERVE))
            testutil_disagg_preserve(session->connection, "preserve", g.stable_timestamp);

        /* Exit synchronization between leader and follower processes. */
        disagg_multi_sync_point();

        /* Assert after sync point to ensure both nodes have preserved the data. */
        testutil_assert(hash_match);
    }
}

/*
 * disagg_is_multi_node --
 *     Return true if disagg is configured for multi-node.
 */
bool
disagg_is_multi_node(void)
{
    const char *page_log;
    bool disagg_enabled;

    page_log = GVS(DISAGG_PAGE_LOG);
    disagg_enabled = (strcmp(page_log, "off") != 0 && strcmp(page_log, "none") != 0);

    return (disagg_enabled && GV(DISAGG_MULTI));
}

/*
 * disagg_is_mode_switch --
 *     Check if disagg is configured to use "switch" mode.
 */
bool
disagg_is_mode_switch(void)
{
    return (g.disagg_storage_config && strcmp(GVS(DISAGG_MODE), "switch") == 0);
}

/*
 * stepdown_workers_drained --
 *     Return true when WT's all_durable timestamp has reached step_down_ts, meaning every
 *     transaction at or below step_down_ts has completed (committed or rolled back). Using
 *     all_durable is stronger than checking per-thread commit_ts: it guarantees no gaps remain in
 *     the commit history at or below the step-down boundary.
 */
static bool
stepdown_workers_drained(wt_timestamp_t step_down_ts)
{
    wt_timestamp_t all_durable;

    testutil_check(timestamp_query("get=all_durable", &all_durable));
    return (all_durable >= step_down_ts);
}

/*
 * disagg_async_stepdown --
 *     Perform an async step-down while worker threads are still live: 1. Stop the checkpoint and
 *     timestamp threads so they cannot interfere. 2. Write lock: capture step_down_ts, advance
 *     g.timestamp past it, and notify WT via set_timestamp(step_down_timestamp) - all under the
 *     lock so WT begins enforcing the boundary before any new timestamps are handed out. WT rolls
 *     back in-flight write transactions while setting the stepdown_ts; threads unblocked from the
 *     write lock get ts values > step_down_ts -> ingest. 3. Drain: wait until every worker has
 *     committed or rolled back at or below step_down_ts.
 */
void
disagg_async_stepdown(wt_thread_t *checkpoint_tid, wt_thread_t *timestamp_tid)
{
    SAP sap;
    WT_SESSION *session;
    wt_timestamp_t step_down_ts;
    uint64_t drain_polls;
    char config[128];

    memset(&sap, 0, sizeof(sap));
    wt_wrap_open_session(g.wts_conn, &sap, NULL, NULL, &session);

    track("[stepdown] stopping checkpoint and timestamp threads", 0ULL);

    /*
     * Stop the checkpoint thread before notifying WT. An uncontrolled checkpoint taken after
     * notification could land stable at the wrong boundary.
     */
    if (g.checkpoint_config == CHECKPOINT_ON) {
        g.checkpoint_quit = true;
        testutil_check(__wt_thread_join(NULL, checkpoint_tid));
    }

    /*
     * Stop the timestamp thread before notifying WT. It must not advance stable past step_down_ts
     * after we pin it below.
     */
    if (g.transaction_timestamps_config) {
        g.timestamp_quit = true;
        testutil_check(__wt_thread_join(NULL, timestamp_tid));
    }

    /*
     * Write lock: prevents any new timestamp from being allocated while we capture step_down_ts,
     * bump g.timestamp past it, and notify WT. Holding the lock through the set_timestamp call
     * ensures WT begins enforcing the boundary before any new allocations are handed out. Threads
     * currently holding the read lock (mid-allocation) finish first; threads waiting for the read
     * lock unblock after we release and get ts values strictly above step_down_ts.
     */
    lock_writelock(session, &g.timestamp_lock);
    step_down_ts = g.timestamp;
    /*
     * Reserve step_down_ts + 1 and step_down_ts + 2 as a gap; all allocations now yield ts >
     * step_down_ts.
     */
    g.timestamp += 2;
    WT_RELEASE_WRITE_WITH_BARRIER(g.stepdown_ts, step_down_ts);
    testutil_snprintf(config, sizeof(config), "step_down_timestamp=%" PRIx64, step_down_ts);
    testutil_check(g.wts_conn->set_timestamp(g.wts_conn, config));
    lock_writeunlock(session, &g.timestamp_lock);

    track(
      "[stepdown] notified WT at ts=%" PRIu64 "; draining in-flight transactions", step_down_ts);

    /*
     * Drain: wait until every in-flight transaction at or below step_down_ts has committed or been
     * rolled back. Timeout after 60 seconds; a permanently hung worker is caught by the 15-minute
     * abort in the outer spin loop.
     */
    for (drain_polls = 60 * WT_THOUSAND / 250; drain_polls > 0; --drain_polls) {
        if (stepdown_workers_drained(step_down_ts))
            break;
        __wt_sleep(0, 250 * WT_THOUSAND);
    }
    testutil_assertfmt(
      drain_polls > 0, "step-down drain timed out at step_down_ts=%" PRIu64, step_down_ts);

    /*
     * Reset the quit flags now that the threads are joined. g.stepdown_ts is intentionally left set
     * so that disagg_switch_roles() can use it for the step-down checkpoint after operations()
     * returns.
     */
    g.checkpoint_quit = false;
    g.timestamp_quit = false;

    wt_wrap_close_session(session);
}

/*
 * disagg_stepdown_thread --
 *     Thread wrapper for disagg_async_stepdown(). Runs the step-down in the background so the
 *     operations() spin loop continues ticking (track_ops) while the drain proceeds. Sets
 *     args->done under a release barrier when the drain is complete.
 */
WT_THREAD_RET
disagg_stepdown_thread(void *arg)
{
    STEPDOWN_ARGS *args;

    args = (STEPDOWN_ARGS *)arg;
    disagg_async_stepdown(args->checkpoint_tid, args->timestamp_tid);
    WT_RELEASE_WRITE_WITH_BARRIER(args->done, true);
    return (WT_THREAD_RET_VALUE);
}

/*
 * disagg_switch_roles --
 *     Toggle the current disagg role between "leader" and "follower". Dispatches to the async
 *     step-down path (disagg.stepdown_async) or the synchronous fallback.
 */
void
disagg_switch_roles(void)
{
    /* Perform step-up or step-down. */
    g.disagg_leader = !g.disagg_leader;

    if (!g.disagg_leader) {
        /* Stepping down: [leader -> follower]. */
        if (GV(DISAGG_STEPDOWN_ASYNC)) {
            /*
             * The async step-down thread stopped the checkpoint/timestamp threads and drained
             * in-flight transactions. Complete the role transition here: pin stable, take the
             * step-down checkpoint, and reconfigure. Both the checkpoint and reconfigure can block,
             * so they belong in this synchronous path (after operations() returns) rather than the
             * background thread, to avoid cache pressure from concurrent worker activity.
             */
            SAP sap;
            WT_SESSION *session;
            wt_timestamp_t stable_after;
            char config[128];

            memset(&sap, 0, sizeof(sap));
            wt_wrap_open_session(g.wts_conn, &sap, NULL, NULL, &session);

            /*
             * Pin stable at exactly step_down_ts now that all operations have finished. Use
             * prepare_commit_lock consistent with timestamp_once(). The subsequent checkpoint
             * captures exactly this boundary.
             */
            testutil_snprintf(config, sizeof(config), "stable_timestamp=%" PRIx64, g.stepdown_ts);
            lock_writelock(session, &g.prepare_commit_lock);
            testutil_check(g.wts_conn->set_timestamp(g.wts_conn, config));
            lock_writeunlock(session, &g.prepare_commit_lock);

            /*
             * Step-down checkpoint: stable is pinned at step_down_ts, so the checkpoint captures
             * exactly the content up to the cut-over and nothing newer.
             */
            track("[stepdown] taking step-down checkpoint", 0ULL);
            testutil_check(session->checkpoint(session, NULL));

            testutil_check(timestamp_query("get=stable", &stable_after));
            testutil_assertfmt(stable_after == g.stepdown_ts,
              "step-down checkpoint: stable=%" PRIu64 " != step_down_ts=%" PRIu64, stable_after,
              g.stepdown_ts);
            track("[stepdown] checkpoint verified", 0ULL);

            g.stepdown_ts = WT_TS_NONE;
            wt_wrap_close_session(session);

            track("[role change] leader -> follower (async completion)", 0ULL);
            testutil_check(g.wts_conn->reconfigure(g.wts_conn, "disaggregated=(role=follower)"));
            follower_read_latest_checkpoint();
        } else {
            /*
             * FIXME-WT-15763: graceful sync step-down is not yet fully supported, so reopen.
             */
            track("[role change] leader -> follower (sync)", 0ULL);
            wts_reopen();
            follower_read_latest_checkpoint();
            wts_prepare_discover(g.wts_conn);
        }
    } else {
        /* Stepping up: [follower -> leader] */
        SAP sap;
        WT_SESSION *session;

        track("[role change] follower -> leader", 0ULL);
        testutil_check(g.wts_conn->reconfigure(g.wts_conn, "disaggregated=(role=leader)"));

        memset(&sap, 0, sizeof(sap));
        wt_wrap_open_session(g.wts_conn, &sap, NULL, NULL, &session);
        /* Advance timestamps to cover all in-memory commits from the follower phase. */
        timestamp_sync_threads_commit_ts();
        timestamp_once(session, false, false);
        testutil_check(session->checkpoint(session, NULL));
        wt_wrap_close_session(session);
    }

    /* After every switch, verify the contents of each table */
    wts_verify_mirrors(g.wts_conn, NULL, NULL);
}
