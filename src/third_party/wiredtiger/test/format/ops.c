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

static int col_insert(TINFO *, WT_CURSOR *);
static void col_insert_resolve(TINFO *);
static int col_modify(TINFO *, WT_CURSOR *, bool);
static int col_remove(TINFO *, WT_CURSOR *, bool);
static int col_reserve(TINFO *, WT_CURSOR *, bool);
static int col_truncate(TINFO *, WT_CURSOR *);
static int col_update(TINFO *, WT_CURSOR *, bool);
static int nextprev(TINFO *, WT_CURSOR *, bool);
static WT_THREAD_RET ops(void *);
static int read_row(TINFO *, WT_CURSOR *);
static int read_row_worker(WT_CURSOR *, TINFO *, uint64_t, WT_ITEM *, WT_ITEM *, bool);
static int row_insert(TINFO *, WT_CURSOR *, bool);
static int row_modify(TINFO *, WT_CURSOR *, bool);
static int row_remove(TINFO *, WT_CURSOR *, bool);
static int row_reserve(TINFO *, WT_CURSOR *, bool);
static int row_truncate(TINFO *, WT_CURSOR *);
static int row_update(TINFO *, WT_CURSOR *, bool);

static char modify_repl[256];

/*
 * modify_repl_init --
 *     Initialize the replacement information.
 */
static void
modify_repl_init(void)
{
    size_t i;

    for (i = 0; i < sizeof(modify_repl); ++i)
        modify_repl[i] = "zyxwvutsrqponmlkjihgfedcba"[i % 26];
}

/*
 * set_core_off --
 *     Turn off core dumps.
 */
void
set_core_off(void)
{
#ifdef HAVE_SETRLIMIT
    struct rlimit rlim;

    rlim.rlim_cur = rlim.rlim_max = 0;
    testutil_check(setrlimit(RLIMIT_CORE, &rlim));
#endif
}

/*
 * random_failure --
 *     Fail the process.
 */
static void
random_failure(void)
{
    static char *core = NULL;

    /*
     * Let our caller know. Note, format.sh checks for this message, so be cautious in changing the
     * format.
     */
    printf("%s: aborting to test recovery\n", progname);
    fflush(stdout);

    /* Turn off core dumps. */
    set_core_off();

    /* Fail at a random moment. */
    *core = 0;
}

TINFO **tinfo_list;

/*
 * tinfo_init --
 *     Initialize the worker thread structures.
 */
static void
tinfo_init(void)
{
    TINFO *tinfo;
    u_int i;

    /* Allocate the thread structures separately to minimize false sharing. */
    if (tinfo_list == NULL) {
        tinfo_list = dcalloc((size_t)g.c_threads + 1, sizeof(TINFO *));
        for (i = 0; i < g.c_threads; ++i) {
            tinfo_list[i] = dcalloc(1, sizeof(TINFO));
            tinfo = tinfo_list[i];

            tinfo->id = (int)i + 1;

            /* Set up the default key and value buffers. */
            tinfo->key = &tinfo->_key;
            key_gen_init(tinfo->key);
            tinfo->value = &tinfo->_value;
            val_gen_init(tinfo->value);
            tinfo->lastkey = &tinfo->_lastkey;
            key_gen_init(tinfo->lastkey);

            snap_init(tinfo);
        }
    }

    /* Cleanup for each new run. */
    for (i = 0; i < g.c_threads; ++i) {
        tinfo = tinfo_list[i];

        tinfo->ops = 0;
        tinfo->commit = 0;
        tinfo->insert = 0;
        tinfo->prepare = 0;
        tinfo->remove = 0;
        tinfo->rollback = 0;
        tinfo->search = 0;
        tinfo->truncate = 0;
        tinfo->update = 0;

        tinfo->session = NULL;
        tinfo->cursor = NULL;

        memset(tinfo->insert_list, 0, sizeof(tinfo->insert_list));
        tinfo->insert_list_cnt = 0;

        tinfo->state = TINFO_RUNNING;
        tinfo->quit = false;
    }
}

/*
 * tinfo_teardown --
 *     Tear down the worker thread structures.
 */
static void
tinfo_teardown(void)
{
    TINFO *tinfo;
    u_int i;

    for (i = 0; i < g.c_threads; ++i) {
        tinfo = tinfo_list[i];

        __wt_buf_free(NULL, &tinfo->vprint);
        __wt_buf_free(NULL, &tinfo->moda);
        __wt_buf_free(NULL, &tinfo->modb);

        /*
         * Assert records were not removed unless configured to do so, otherwise subsequent runs can
         * incorrectly report scan errors.
         */
        testutil_assert(g.c_delete_pct != 0 || tinfo->remove == 0);

        snap_teardown(tinfo);
        key_gen_teardown(tinfo->key);
        val_gen_teardown(tinfo->value);
        key_gen_teardown(tinfo->lastkey);

        free(tinfo);
    }
    free(tinfo_list);
    tinfo_list = NULL;
}

/*
 * tinfo_rollback_to_stable --
 *     Do a rollback to stable and verify operations.
 */
static void
tinfo_rollback_to_stable(WT_SESSION *session)
{
    WT_CURSOR *cursor;

    /* Rollback-to-stable only makes sense for timestamps and on-disk stores. */
    if (g.c_txn_timestamps == 0 || g.c_in_memory != 0)
        return;

    trace_msg("%-10s ts=%" PRIu64, "rts", g.stable_timestamp);
    testutil_check(g.wts_conn->rollback_to_stable(g.wts_conn, NULL));

    /* Check the saved snap operations for consistency. */
    testutil_check(session->open_cursor(session, g.uri, NULL, NULL, &cursor));
    snap_repeat_rollback(cursor, tinfo_list, g.c_threads);
    testutil_check(cursor->close(cursor));
}

/*
 * operations --
 *     Perform a number of operations in a set of threads.
 */
void
operations(u_int ops_seconds, bool lastrun)
{
    TINFO *tinfo, total;
    WT_CONNECTION *conn;
    WT_SESSION *session;
    wt_thread_t alter_tid, backup_tid, checkpoint_tid, compact_tid, hs_tid, import_tid, random_tid;
    wt_thread_t timestamp_tid;
    int64_t fourths, quit_fourths, thread_ops;
    uint32_t i;
    bool running;

    conn = g.wts_conn;

    session = NULL; /* -Wconditional-uninitialized */
    memset(&alter_tid, 0, sizeof(alter_tid));
    memset(&backup_tid, 0, sizeof(backup_tid));
    memset(&checkpoint_tid, 0, sizeof(checkpoint_tid));
    memset(&compact_tid, 0, sizeof(compact_tid));
    memset(&hs_tid, 0, sizeof(hs_tid));
    memset(&import_tid, 0, sizeof(import_tid));
    memset(&random_tid, 0, sizeof(random_tid));
    memset(&timestamp_tid, 0, sizeof(timestamp_tid));

    modify_repl_init();

    /*
     * There are two mechanisms to specify the length of the run, a number of operations and a
     * timer, when either expire the run terminates.
     *
     * Each thread does an equal share of the total operations (and make sure that it's not 0).
     *
     * Calculate how many fourth-of-a-second sleeps until the timer expires. If the timer expires
     * and threads don't return in 15 minutes, assume there is something hung, and force the quit.
     */
    if (g.c_ops == 0)
        thread_ops = -1;
    else {
        if (g.c_ops < g.c_threads)
            g.c_ops = g.c_threads;
        thread_ops = g.c_ops / g.c_threads;
    }
    if (ops_seconds == 0)
        fourths = quit_fourths = -1;
    else {
        fourths = ops_seconds * 4;
        quit_fourths = fourths + 15 * 4 * 60;
    }

    /* Get a session. */
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Initialize and start the worker threads. */
    tinfo_init();
    trace_msg("%s", "=============== thread ops start");

    for (i = 0; i < g.c_threads; ++i) {
        tinfo = tinfo_list[i];
        testutil_check(__wt_thread_create(NULL, &tinfo->tid, ops, tinfo));
    }

    /* Start optional special-purpose threads. */
    if (g.c_alter)
        testutil_check(__wt_thread_create(NULL, &alter_tid, alter, NULL));
    if (g.c_backups)
        testutil_check(__wt_thread_create(NULL, &backup_tid, backup, NULL));
    if (g.c_compact)
        testutil_check(__wt_thread_create(NULL, &compact_tid, compact, NULL));
    if (g.c_hs_cursor)
        testutil_check(__wt_thread_create(NULL, &hs_tid, hs_cursor, NULL));
    if (g.c_import)
        testutil_check(__wt_thread_create(NULL, &import_tid, import, NULL));
    if (g.c_random_cursor)
        testutil_check(__wt_thread_create(NULL, &random_tid, random_kv, NULL));
    if (g.c_txn_timestamps)
        testutil_check(__wt_thread_create(NULL, &timestamp_tid, timestamp, tinfo_list));

    if (g.c_checkpoint_flag == CHECKPOINT_ON)
        testutil_check(__wt_thread_create(NULL, &checkpoint_tid, checkpoint, NULL));

    /* Spin on the threads, calculating the totals. */
    for (;;) {
        /* Clear out the totals each pass. */
        memset(&total, 0, sizeof(total));
        for (i = 0, running = false; i < g.c_threads; ++i) {
            tinfo = tinfo_list[i];
            total.commit += tinfo->commit;
            total.insert += tinfo->insert;
            total.prepare += tinfo->prepare;
            total.remove += tinfo->remove;
            total.rollback += tinfo->rollback;
            total.search += tinfo->search;
            total.truncate += tinfo->truncate;
            total.update += tinfo->update;

            switch (tinfo->state) {
            case TINFO_RUNNING:
                running = true;
                break;
            case TINFO_COMPLETE:
                tinfo->state = TINFO_JOINED;
                testutil_check(__wt_thread_join(NULL, &tinfo->tid));
                break;
            case TINFO_JOINED:
                break;
            }

            /*
             * If the timer has expired or this thread has completed its operations, notify the
             * thread it should quit.
             */
            if (fourths == 0 || (thread_ops != -1 && tinfo->ops >= (uint64_t)thread_ops)) {
                /*
                 * On the last execution, optionally drop core for recovery testing.
                 */
                if (lastrun && g.c_abort)
                    random_failure();
                tinfo->quit = true;
            }
        }
        track("ops", 0ULL, &total);
        if (!running)
            break;
        __wt_sleep(0, 250000); /* 1/4th of a second */
        if (fourths != -1)
            --fourths;
        if (quit_fourths != -1 && --quit_fourths == 0) {
            fprintf(stderr, "%s\n", "format run more than 15 minutes past the maximum time");
            fprintf(stderr, "%s\n",
              "format run dumping cache and transaction state, then aborting the process");

            /*
             * If the library is deadlocked, we might just join the mess, set a two-minute timer to
             * limit our exposure.
             */
            set_alarm(120);

            (void)conn->debug_info(conn, "txn");
            (void)conn->debug_info(conn, "cache");

            __wt_abort(NULL);
        }
    }

    /* Wait for the special-purpose threads. */
    g.workers_finished = true;
    if (g.c_alter)
        testutil_check(__wt_thread_join(NULL, &alter_tid));
    if (g.c_backups)
        testutil_check(__wt_thread_join(NULL, &backup_tid));
    if (g.c_checkpoint_flag == CHECKPOINT_ON)
        testutil_check(__wt_thread_join(NULL, &checkpoint_tid));
    if (g.c_compact)
        testutil_check(__wt_thread_join(NULL, &compact_tid));
    if (g.c_hs_cursor)
        testutil_check(__wt_thread_join(NULL, &hs_tid));
    if (g.c_import)
        testutil_check(__wt_thread_join(NULL, &import_tid));
    if (g.c_random_cursor)
        testutil_check(__wt_thread_join(NULL, &random_tid));
    if (g.c_txn_timestamps)
        testutil_check(__wt_thread_join(NULL, &timestamp_tid));
    g.workers_finished = false;

    trace_msg("%s", "=============== thread ops stop");

    /*
     * The system should be quiescent at this point, call rollback to stable. Generally, we expect
     * applications to do rollback-to-stable as part of the database open, but calling it outside of
     * the open path is expected in the case of applications that are "restarting" but skipping the
     * close/re-open pair. Note we are not advancing the oldest timestamp, otherwise we wouldn't be
     * able to replay operations from after rollback-to-stable completes.
     */
    tinfo_rollback_to_stable(session);

    if (lastrun) {
        tinfo_teardown();
        timestamp_teardown(session);
    }

    testutil_check(session->close(session, NULL));
}

/*
 * begin_transaction_ts --
 *     Begin a timestamped transaction.
 */
static void
begin_transaction_ts(TINFO *tinfo)
{
    TINFO **tlp;
    WT_DECL_RET;
    WT_SESSION *session;
    uint64_t ts;
    char buf[64];

    session = tinfo->session;

    /*
     * Transaction reads are normally repeatable, but WiredTiger timestamps allow rewriting commits,
     * that is, applications can specify at commit time the timestamp at which the commit happens.
     * If that happens, our read might no longer be repeatable. Test in both modes: pick a read
     * timestamp we know is repeatable (because it's at least as old as the oldest resolved commit
     * timestamp in any thread), and pick a current timestamp, 50% of the time.
     */
    ts = 0;
    if (mmrand(&tinfo->rnd, 1, 2) == 1)
        for (ts = UINT64_MAX, tlp = tinfo_list; *tlp != NULL; ++tlp)
            ts = WT_MIN(ts, (*tlp)->commit_ts);
    if (ts != 0) {
        wiredtiger_begin_transaction(session, NULL);

        /*
         * If the timestamp has aged out of the system, we'll get EINVAL when we try and set it.
         * That kills the transaction, we have to restart.
         */
        testutil_check(__wt_snprintf(buf, sizeof(buf), "read_timestamp=%" PRIx64, ts));
        ret = session->timestamp_transaction(session, buf);
        if (ret == 0) {
            snap_op_init(tinfo, ts, true);
            trace_op(tinfo, "begin snapshot read-ts=%" PRIu64 " (repeatable)", ts);
            return;
        }
        if (ret != EINVAL)
            testutil_check(ret);

        testutil_check(session->rollback_transaction(session, NULL));
    }

    wiredtiger_begin_transaction(session, NULL);

    /*
     * Otherwise, pick a current timestamp.
     *
     * Prepare returns an error if the prepare timestamp is less than any active read timestamp,
     * single-thread transaction prepare and begin.
     *
     * Lock out the oldest timestamp update.
     */
    lock_writelock(session, &g.ts_lock);

    ts = __wt_atomic_addv64(&g.timestamp, 1);
    testutil_check(__wt_snprintf(buf, sizeof(buf), "read_timestamp=%" PRIx64, ts));
    testutil_check(session->timestamp_transaction(session, buf));

    lock_writeunlock(session, &g.ts_lock);

    snap_op_init(tinfo, ts, false);
    trace_op(tinfo, "begin snapshot read-ts=%" PRIu64 " (not repeatable)", ts);
}

/*
 * begin_transaction --
 *     Begin a non-timestamp transaction.
 */
static void
begin_transaction(TINFO *tinfo, const char *iso_config)
{
    WT_SESSION *session;

    session = tinfo->session;

    wiredtiger_begin_transaction(session, iso_config);

    snap_op_init(tinfo, WT_TS_NONE, false);
    trace_op(tinfo, "begin %s", iso_config);
}

/*
 * commit_transaction --
 *     Commit a transaction.
 */
static void
commit_transaction(TINFO *tinfo, bool prepared)
{
    WT_SESSION *session;
    uint64_t ts;
    char buf[64];

    ++tinfo->commit;

    session = tinfo->session;

    ts = 0; /* -Wconditional-uninitialized */
    if (g.c_txn_timestamps) {
        if (prepared)
            lock_readlock(session, &g.prepare_commit_lock);

        /* Lock out the oldest timestamp update. */
        lock_writelock(session, &g.ts_lock);

        ts = __wt_atomic_addv64(&g.timestamp, 1);
        testutil_check(__wt_snprintf(buf, sizeof(buf), "commit_timestamp=%" PRIx64, ts));
        testutil_check(session->timestamp_transaction(session, buf));

        if (prepared) {
            testutil_check(__wt_snprintf(buf, sizeof(buf), "durable_timestamp=%" PRIx64, ts));
            testutil_check(session->timestamp_transaction(session, buf));
        }

        lock_writeunlock(session, &g.ts_lock);
        testutil_check(session->commit_transaction(session, NULL));
        if (prepared)
            lock_readunlock(session, &g.prepare_commit_lock);
    } else
        testutil_check(session->commit_transaction(session, NULL));

    /* Remember our oldest commit timestamp. */
    tinfo->commit_ts = ts;

    trace_op(
      tinfo, "commit read-ts=%" PRIu64 ", commit-ts=%" PRIu64, tinfo->read_ts, tinfo->commit_ts);
}

/*
 * rollback_transaction --
 *     Rollback a transaction.
 */
static void
rollback_transaction(TINFO *tinfo)
{
    WT_SESSION *session;

    session = tinfo->session;

    ++tinfo->rollback;

    testutil_check(session->rollback_transaction(session, NULL));

    trace_op(tinfo, "abort read-ts=%" PRIu64, tinfo->read_ts);
}

/*
 * prepare_transaction --
 *     Prepare a transaction if timestamps are in use.
 */
static int
prepare_transaction(TINFO *tinfo)
{
    WT_DECL_RET;
    WT_SESSION *session;
    uint64_t ts;
    uint32_t longwait, pause_ms;
    char buf[64];

    session = tinfo->session;

    ++tinfo->prepare;

    /*
     * Prepare timestamps must be less than or equal to the eventual commit timestamp. Set the
     * prepare timestamp to whatever the global value is now. The subsequent commit will increment
     * it, ensuring correctness.
     *
     * Prepare returns an error if the prepare timestamp is less than any active read timestamp,
     * single-thread transaction prepare and begin.
     *
     * Lock out the oldest timestamp update.
     */
    lock_writelock(session, &g.ts_lock);

    ts = __wt_atomic_addv64(&g.timestamp, 1);
    testutil_check(__wt_snprintf(buf, sizeof(buf), "prepare_timestamp=%" PRIx64, ts));
    ret = session->prepare_transaction(session, buf);

    trace_op(tinfo, "prepare ts=%" PRIu64, ts);

    lock_writeunlock(session, &g.ts_lock);

    /*
     * Sometimes add a delay after prepare to induce extra memory stress. For 80% of the threads,
     * there is never a delay, so there is always a dedicated set of threads trying to do work. For
     * the other 20%, we'll sometimes delay. For these threads, 99% of the time, proceed without
     * delay. The rest of the time, pause up to 5 seconds, weighted toward the smaller delays.
     */
    if (tinfo->id % 5 == 0) {
        longwait = mmrand(&tinfo->rnd, 0, 999);
        if (longwait < 10) {
            pause_ms = mmrand(&tinfo->rnd, 1, 10) << longwait;
            __wt_sleep(0, (uint64_t)pause_ms * WT_THOUSAND);
        }
    }
    return (ret);
}

/*
 * OP_FAILED --
 *	General error handling.
 */
#define OP_FAILED(notfound_ok)                                                                \
    do {                                                                                      \
        positioned = false;                                                                   \
        if (intxn && (ret == WT_CACHE_FULL || ret == WT_ROLLBACK))                            \
            goto rollback;                                                                    \
        testutil_assert(                                                                      \
          (notfound_ok && ret == WT_NOTFOUND) || ret == WT_CACHE_FULL || ret == WT_ROLLBACK); \
    } while (0)

/*
 * Rollback updates returning prepare-conflict, they're unlikely to succeed unless the prepare
 * aborts. Reads wait out the error, so it's unexpected.
 */
#define READ_OP_FAILED(notfound_ok) OP_FAILED(notfound_ok)
#define WRITE_OP_FAILED(notfound_ok)    \
    do {                                \
        if (ret == WT_PREPARE_CONFLICT) \
            ret = WT_ROLLBACK;          \
        OP_FAILED(notfound_ok);         \
    } while (0)

/*
 * ops_open_session --
 *     Create a new session/cursor pair for the thread.
 */
static void
ops_open_session(TINFO *tinfo)
{
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_SESSION *session;

    conn = g.wts_conn;

    /* Close any open session/cursor. */
    if ((session = tinfo->session) != NULL)
        testutil_check(session->close(session, NULL));

    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /*
     * Configure "append", in the case of column stores, we append when inserting new rows.
     *
     * WT_SESSION.open_cursor can return EBUSY if concurrent with a metadata operation, retry.
     */
    while ((ret = session->open_cursor(session, g.uri, NULL, "append", &cursor)) == EBUSY)
        __wt_yield();
    testutil_checkfmt(ret, "%s", g.uri);

    tinfo->session = session;
    tinfo->cursor = cursor;
}

/* Isolation configuration. */
typedef enum {
    ISOLATION_READ_COMMITTED,
    ISOLATION_READ_UNCOMMITTED,
    ISOLATION_SNAPSHOT
} iso_level_t;

/* When in an explicit snapshot isolation transaction, track operations for later
 * repetition. */
#define SNAP_TRACK(tinfo, op)                         \
    do {                                              \
        if (intxn && iso_level == ISOLATION_SNAPSHOT) \
            snap_track(tinfo, op);                    \
    } while (0)

/*
 * ops --
 *     Per-thread operations.
 */
static WT_THREAD_RET
ops(void *arg)
{
    TINFO *tinfo;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_SESSION *session;
    iso_level_t iso_level;
    thread_op op;
    uint64_t max_rows, reset_op, session_op, truncate_op;
    uint32_t range, rnd;
    u_int i, j;
    const char *iso_config;
    bool greater_than, intxn, next, positioned, prepared;

    tinfo = arg;
    testutil_check(__wt_thread_str(tinfo->tidbuf, sizeof(tinfo->tidbuf)));

    /*
     * Characterize the per-thread random number generator. Normally we want independent behavior so
     * threads start in different parts of the RNG space, but we've found bugs by having the threads
     * pound on the same key/value pairs, that is, by making them traverse the same RNG space. 75%
     * of the time we run in independent RNG space.
     */
    if (g.c_independent_thread_rng)
        __wt_random_init_seed(NULL, &tinfo->rnd);
    else
        __wt_random_init(&tinfo->rnd);

    iso_level = ISOLATION_SNAPSHOT; /* -Wconditional-uninitialized */

    /* Set the first operation where we'll create sessions and cursors. */
    cursor = NULL;
    session = NULL;
    session_op = 0;

    /* Set the first operation where we'll reset the session. */
    reset_op = mmrand(&tinfo->rnd, 100, 10000);
    /* Set the first operation where we'll truncate a range. */
    truncate_op = g.c_truncate == 0 ? UINT64_MAX : mmrand(&tinfo->rnd, 100, 10000);

    /* Initialize operation trace. */
    trace_ops_init(tinfo);

    for (intxn = false; !tinfo->quit; ++tinfo->ops) {
        /* Periodically open up a new session and cursors. */
        if (tinfo->ops > session_op || session == NULL || cursor == NULL) {
            /*
             * We can't swap sessions/cursors if in a transaction, resolve any running transaction.
             */
            if (intxn) {
                commit_transaction(tinfo, false);
                intxn = false;
            }

            ops_open_session(tinfo);

            /* Pick the next session/cursor close/open. */
            session_op += mmrand(&tinfo->rnd, 100, 5000);

            session = tinfo->session;
            cursor = tinfo->cursor;
        }

        /*
         * If not in a transaction, reset the session periodically to make sure that operation is
         * tested. The test is not for equality, resets must be done outside of transactions so we
         * aren't likely to get an exact match.
         */
        if (!intxn && tinfo->ops > reset_op) {
            testutil_check(session->reset(session));

            /* Pick the next reset operation. */
            reset_op += mmrand(&tinfo->rnd, 20000, 50000);
        }

        /*
         * If not in a transaction and in a timestamp world, occasionally repeat a timestamped
         * operation.
         */
        if (!intxn && g.c_txn_timestamps && mmrand(&tinfo->rnd, 1, 15) == 1) {
            ++tinfo->search;
            snap_repeat_single(cursor, tinfo);
        }

        /* If not in a transaction and in a timestamp world, start a transaction. */
        if (!intxn && g.c_txn_timestamps) {
            iso_level = ISOLATION_SNAPSHOT;
            begin_transaction_ts(tinfo);
            intxn = true;
        }

        /*
         * If not in a transaction and not in a timestamp world, start a transaction some percentage
         * of the time.
         */
        if (!intxn && mmrand(&tinfo->rnd, 1, 100) < g.c_txn_implicit) {
            iso_level = ISOLATION_SNAPSHOT;
            iso_config = "isolation=snapshot";

            /* Occasionally do reads at an isolation level lower than snapshot. */
            switch (mmrand(NULL, 1, 20)) {
            case 1:
                iso_level = ISOLATION_READ_COMMITTED; /* 5% */
                iso_config = "isolation=read-committed";
                break;
            case 2:
                iso_level = ISOLATION_READ_UNCOMMITTED; /* 5% */
                iso_config = "isolation=read-uncommitted";
                break;
            }

            begin_transaction(tinfo, iso_config);
            intxn = true;
        }

        /*
         * Select an operation: all updates must be in snapshot isolation, modify must be in an
         * explicit transaction.
         */
        op = READ;
        if (iso_level == ISOLATION_SNAPSHOT) {
            i = mmrand(&tinfo->rnd, 1, 100);
            if (i < g.c_delete_pct && tinfo->ops > truncate_op) {
                op = TRUNCATE;

                /* Pick the next truncate operation. */
                truncate_op += mmrand(&tinfo->rnd, 20000, 100000);
            } else if (i < g.c_delete_pct)
                op = REMOVE;
            else if (i < g.c_delete_pct + g.c_insert_pct)
                op = INSERT;
            else if (intxn && i < g.c_delete_pct + g.c_insert_pct + g.c_modify_pct)
                op = MODIFY;
            else if (i < g.c_delete_pct + g.c_insert_pct + g.c_modify_pct + g.c_write_pct)
                op = UPDATE;
        }

        /*
         * Select a row. Column-store extends the object, explicitly read the maximum row count and
         * then use a local variable so the value won't change inside the loop.
         */
        max_rows = (volatile uint64_t)g.rows;
        tinfo->keyno = mmrand(&tinfo->rnd, 1, (u_int)max_rows);

        /*
         * Inserts, removes and updates can be done following a cursor set-key, or based on a cursor
         * position taken from a previous search. If not already doing a read, position the cursor
         * at an existing point in the tree 20% of the time.
         */
        positioned = false;
        if (op != READ && mmrand(&tinfo->rnd, 1, 5) == 1) {
            ++tinfo->search;
            ret = read_row(tinfo, cursor);
            if (ret == 0) {
                positioned = true;
                SNAP_TRACK(tinfo, READ);
            } else
                READ_OP_FAILED(true);
        }

        /*
         * Optionally reserve a row, it's an update so it requires snapshot isolation. Reserving a
         * row before a read isn't all that sensible, but not unexpected, either.
         */
        if (intxn && iso_level == ISOLATION_SNAPSHOT && mmrand(&tinfo->rnd, 0, 20) == 1) {
            switch (g.type) {
            case ROW:
                ret = row_reserve(tinfo, cursor, positioned);
                break;
            case FIX:
            case VAR:
                ret = col_reserve(tinfo, cursor, positioned);
                break;
            }
            if (ret == 0) {
                positioned = true;
                __wt_yield(); /* Encourage races */
            } else
                WRITE_OP_FAILED(true);
        }

        /* Perform the operation. */
        switch (op) {
        case INSERT:
            switch (g.type) {
            case ROW:
                ret = row_insert(tinfo, cursor, positioned);
                break;
            case FIX:
            case VAR:
                /*
                 * We can only append so many new records, once we reach that limit, update a record
                 * instead of inserting.
                 */
                if (tinfo->insert_list_cnt >= WT_ELEMENTS(tinfo->insert_list))
                    goto update_instead_of_chosen_op;

                ret = col_insert(tinfo, cursor);
                break;
            }

            /* Insert never leaves the cursor positioned. */
            positioned = false;
            if (ret == 0) {
                ++tinfo->insert;
                SNAP_TRACK(tinfo, INSERT);
            } else
                WRITE_OP_FAILED(false);
            break;
        case MODIFY:
            ++tinfo->update;
            switch (g.type) {
            case ROW:
                ret = row_modify(tinfo, cursor, positioned);
                break;
            case VAR:
                ret = col_modify(tinfo, cursor, positioned);
                break;
            }
            if (ret == 0) {
                positioned = true;
                SNAP_TRACK(tinfo, MODIFY);
            } else
                WRITE_OP_FAILED(true);
            break;
        case READ:
            ++tinfo->search;
            ret = read_row(tinfo, cursor);
            if (ret == 0) {
                positioned = true;
                SNAP_TRACK(tinfo, READ);
            } else
                READ_OP_FAILED(true);
            break;
        case REMOVE:
            switch (g.type) {
            case ROW:
                ret = row_remove(tinfo, cursor, positioned);
                break;
            case FIX:
            case VAR:
                ret = col_remove(tinfo, cursor, positioned);
                break;
            }
            if (ret == 0) {
                ++tinfo->remove;
                /*
                 * Don't set positioned: it's unchanged from the previous state, but not necessarily
                 * set.
                 */
                SNAP_TRACK(tinfo, REMOVE);
            } else
                WRITE_OP_FAILED(true);
            break;
        case TRUNCATE:
            /*
             * A maximum of 2 truncation operations at a time, more than that can lead to serious
             * thrashing.
             */
            if (__wt_atomic_addv64(&g.truncate_cnt, 1) > 2) {
                (void)__wt_atomic_subv64(&g.truncate_cnt, 1);
                goto update_instead_of_chosen_op;
            }

            if (!positioned)
                tinfo->keyno = mmrand(&tinfo->rnd, 1, (u_int)max_rows);

            /*
             * Truncate up to 5% of the table. If the range overlaps the beginning/end of the table,
             * set the key to 0 (the truncate function then sets a cursor to NULL so that code is
             * tested).
             *
             * This gets tricky: there are 2 directions (truncating from lower keys to the current
             * position or from the current position to higher keys), and collation order
             * (truncating from lower keys to higher keys or vice-versa).
             */
            greater_than = mmrand(&tinfo->rnd, 0, 1) == 1;
            range = max_rows < 20 ? 0 : mmrand(&tinfo->rnd, 0, (u_int)max_rows / 20);
            tinfo->last = tinfo->keyno;
            if (greater_than) {
                if (g.c_reverse) {
                    if (tinfo->keyno <= range)
                        tinfo->last = 0;
                    else
                        tinfo->last -= range;
                } else {
                    tinfo->last += range;
                    if (tinfo->last > max_rows)
                        tinfo->last = 0;
                }
            } else {
                if (g.c_reverse) {
                    tinfo->keyno += range;
                    if (tinfo->keyno > max_rows)
                        tinfo->keyno = 0;
                } else {
                    if (tinfo->keyno <= range)
                        tinfo->keyno = 0;
                    else
                        tinfo->keyno -= range;
                }
            }
            switch (g.type) {
            case ROW:
                ret = row_truncate(tinfo, cursor);
                break;
            case FIX:
            case VAR:
                ret = col_truncate(tinfo, cursor);
                break;
            }
            (void)__wt_atomic_subv64(&g.truncate_cnt, 1);

            /* Truncate never leaves the cursor positioned. */
            positioned = false;
            if (ret == 0) {
                ++tinfo->truncate;
                SNAP_TRACK(tinfo, TRUNCATE);
            } else
                WRITE_OP_FAILED(false);
            break;
        case UPDATE:
update_instead_of_chosen_op:
            ++tinfo->update;
            switch (g.type) {
            case ROW:
                ret = row_update(tinfo, cursor, positioned);
                break;
            case FIX:
            case VAR:
                ret = col_update(tinfo, cursor, positioned);
                break;
            }
            if (ret == 0) {
                positioned = true;
                SNAP_TRACK(tinfo, UPDATE);
            } else
                WRITE_OP_FAILED(false);
            break;
        }

        /* If we have pending inserts, try and update the total rows. */
        if (tinfo->insert_list_cnt > 0)
            col_insert_resolve(tinfo);

        /*
         * The cursor is positioned if we did any operation other than insert, do a small number of
         * next/prev cursor operations in a random direction.
         */
        if (positioned) {
            next = mmrand(&tinfo->rnd, 0, 1) == 1;
            j = mmrand(&tinfo->rnd, 1, 100);
            for (i = 0; i < j; ++i) {
                if ((ret = nextprev(tinfo, cursor, next)) == 0)
                    continue;

                READ_OP_FAILED(true);
                break;
            }
        }

        /* Reset the cursor: there is no reason to keep pages pinned. */
        testutil_check(cursor->reset(cursor));

        /*
         * No post-operation work is needed outside of a transaction. If in a transaction, add more
         * operations to the transaction half the time.
         */
        if (!intxn || (rnd = mmrand(&tinfo->rnd, 1, 10)) > 5)
            continue;

        /*
         * Ending a transaction. If the transaction was configured for snapshot isolation, repeat
         * the operations and confirm the results are unchanged.
         */
        if (intxn && iso_level == ISOLATION_SNAPSHOT) {
            __wt_yield(); /* Encourage races */

            ret = snap_repeat_txn(cursor, tinfo);
            testutil_assert(ret == 0 || ret == WT_ROLLBACK || ret == WT_CACHE_FULL);
            if (ret == WT_ROLLBACK || ret == WT_CACHE_FULL)
                goto rollback;
        }

        /* If prepare configured, prepare the transaction 10% of the time. */
        prepared = false;
        if (g.c_prepare && mmrand(&tinfo->rnd, 1, 10) == 1) {
            if ((ret = prepare_transaction(tinfo)) != 0)
                WRITE_OP_FAILED(false);

            __wt_yield(); /* Encourage races */
            prepared = true;
        }

        /*
         * If we're in a transaction, commit 40% of the time and rollback 10% of the time (we
         * continued to add operations to the transaction the remaining 50% of the time).
         */
        switch (rnd) {
        case 1:
        case 2:
        case 3:
        case 4: /* 40% */
            commit_transaction(tinfo, prepared);
            snap_repeat_update(tinfo, true);
            break;
        case 5: /* 10% */
rollback:
            rollback_transaction(tinfo);
            snap_repeat_update(tinfo, false);
            break;
        }

        intxn = false;
    }

    if (session != NULL) {
        testutil_check(session->close(session, NULL));
        tinfo->cursor = NULL;
        tinfo->session = NULL;
    }

    tinfo->state = TINFO_COMPLETE;
    return (WT_THREAD_RET_VALUE);
}

/*
 * wts_read_scan --
 *     Read and verify a subset of the elements in a file.
 */
void
wts_read_scan(void)
{
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_ITEM key, value;
    WT_SESSION *session;
    uint64_t keyno;

    conn = g.wts_conn;

    /*
     * We're not configuring transactions or read timestamps, if there's a diagnostic check, skip
     * the scan.
     */
    if (g.c_assert_read_timestamp)
        return;

    /* Set up the default key/value buffers. */
    key_gen_init(&key);
    val_gen_init(&value);

    /* Open a session and cursor pair. */
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    /*
     * open_cursor can return EBUSY if concurrent with a metadata operation, retry in that case.
     */
    while ((ret = session->open_cursor(session, g.uri, NULL, NULL, &cursor)) == EBUSY)
        __wt_yield();
    testutil_check(ret);

    /* Check a random subset of the records using the key. */
    for (keyno = 0; keyno < g.rows;) {
        keyno += mmrand(NULL, 1, 1000);
        if (keyno > g.rows)
            keyno = g.rows;

        switch (ret = read_row_worker(cursor, NULL, keyno, &key, &value, false)) {
        case 0:
        case WT_NOTFOUND:
        case WT_ROLLBACK:
        case WT_CACHE_FULL:
        case WT_PREPARE_CONFLICT:
            break;
        default:
            testutil_die(ret, "wts_read_scan: read row %" PRIu64, keyno);
        }
    }

    testutil_check(session->close(session, NULL));

    key_gen_teardown(&key);
    val_gen_teardown(&value);
}

/*
 * read_row_worker --
 *     Read and verify a single element in a row- or column-store file.
 */
static int
read_row_worker(
  WT_CURSOR *cursor, TINFO *tinfo, uint64_t keyno, WT_ITEM *key, WT_ITEM *value, bool sn)
{
    uint8_t bitfield;
    int exact, ret;

    /* Retrieve the key/value pair by key. */
    switch (g.type) {
    case FIX:
    case VAR:
        cursor->set_key(cursor, keyno);
        break;
    case ROW:
        key_gen(key, keyno);
        cursor->set_key(cursor, key);
        break;
    }

    if (sn) {
        ret = read_op(cursor, SEARCH_NEAR, &exact);
        if (ret == 0 && exact != 0)
            ret = WT_NOTFOUND;
    } else
        ret = read_op(cursor, SEARCH, NULL);
    switch (ret) {
    case 0:
        if (g.type == FIX) {
            testutil_check(cursor->get_value(cursor, &bitfield));
            *(uint8_t *)(value->data) = bitfield;
            value->size = 1;
        } else
            testutil_check(cursor->get_value(cursor, value));
        break;
    case WT_NOTFOUND:
        /*
         * In fixed length stores, zero values at the end of the key space are returned as
         * not-found. Treat this the same as a zero value in the key space, to match BDB's behavior.
         * The WiredTiger cursor has lost its position though, so we return not-found, the cursor
         * movement can't continue.
         */
        if (g.type == FIX) {
            *(uint8_t *)(value->data) = 0;
            value->size = 1;
        }
        break;
    default:
        return (ret);
    }

    /* Log the operation */
    if (ret == 0)
        switch (g.type) {
        case FIX:
            if (tinfo == NULL && g.trace_all)
                trace_msg("read %" PRIu64 " {0x%02x}", keyno, ((char *)value->data)[0]);
            if (tinfo != NULL)
                trace_op(tinfo, "read %" PRIu64 " {0x%02x}", keyno, ((char *)value->data)[0]);

            break;
        case ROW:
        case VAR:
            if (tinfo == NULL && g.trace_all)
                trace_msg("read %" PRIu64 " {%.*s}", keyno, (int)value->size, (char *)value->data);
            if (tinfo != NULL)
                trace_op(tinfo, "read %" PRIu64 " {%s}", keyno, trace_item(tinfo, value));
            break;
        }

    return (ret);
}

/*
 * read_row --
 *     Read and verify a single element in a row- or column-store file.
 */
static int
read_row(TINFO *tinfo, WT_CURSOR *cursor)
{
    /* 25% of the time we call search-near. */
    return (read_row_worker(
      cursor, tinfo, tinfo->keyno, tinfo->key, tinfo->value, mmrand(&tinfo->rnd, 0, 3) == 1));
}

/*
 * nextprev --
 *     Read and verify the next/prev element in a row- or column-store file.
 */
static int
nextprev(TINFO *tinfo, WT_CURSOR *cursor, bool next)
{
    WT_DECL_RET;
    WT_ITEM key, value;
    uint64_t keyno, keyno_prev;
    uint8_t bitfield;
    int cmp;
    const char *which;
    bool incrementing, record_gaps;

    keyno = 0;
    which = next ? "next" : "prev";

    switch (ret = read_op(cursor, next ? NEXT : PREV, NULL)) {
    case 0:
        switch (g.type) {
        case FIX:
            if ((ret = cursor->get_key(cursor, &keyno)) == 0 &&
              (ret = cursor->get_value(cursor, &bitfield)) == 0) {
                value.data = &bitfield;
                value.size = 1;
            }
            break;
        case ROW:
            if ((ret = cursor->get_key(cursor, &key)) == 0)
                ret = cursor->get_value(cursor, &value);
            break;
        case VAR:
            if ((ret = cursor->get_key(cursor, &keyno)) == 0)
                ret = cursor->get_value(cursor, &value);
            break;
        }
        if (ret != 0)
            testutil_die(ret, "nextprev: get_key/get_value");

        /* Check that keys are never returned out-of-order. */
        /*
         * XXX WT-3889 LSM has a bug that prevents cursor order checks from working, skip the test
         * for now.
         */
        if (DATASOURCE("lsm"))
            break;

        /*
         * Compare the returned key with the previously returned key, and assert the order is
         * correct. If not deleting keys, and the rows aren't in the column-store insert name space,
         * also assert we don't skip groups of records (that's a page-split bug symptom). Note a
         * previous run that performed salvage might have corrupted a chunk of space such that
         * records were removed. If this is a reopen of an existing database, assume salvage might
         * have happened.
         */
        record_gaps = g.c_delete_pct != 0 || g.reopen;
        switch (g.type) {
        case FIX:
        case VAR:
            if (tinfo->keyno > g.c_rows || keyno > g.c_rows)
                record_gaps = true;
            if (!next) {
                if (tinfo->keyno < keyno || (!record_gaps && keyno != tinfo->keyno - 1))
                    goto order_error_col;
            } else if (tinfo->keyno > keyno || (!record_gaps && keyno != tinfo->keyno + 1))
                goto order_error_col;
            if (0) {
order_error_col:
                testutil_die(
                  0, "%s returned %" PRIu64 " then %" PRIu64, which, tinfo->keyno, keyno);
            }

            tinfo->keyno = keyno;
            break;
        case ROW:
            incrementing = (next && !g.c_reverse) || (!next && g.c_reverse);
            cmp = memcmp(tinfo->key->data, key.data, WT_MIN(tinfo->key->size, key.size));
            if (incrementing) {
                if (cmp > 0 || (cmp == 0 && tinfo->key->size < key.size))
                    goto order_error_row;
            } else if (cmp < 0 || (cmp == 0 && tinfo->key->size > key.size))
                goto order_error_row;
            if (!record_gaps) {
                /*
                 * Convert the keys to record numbers and then compare less-than-or-equal. (It's not
                 * less-than, row-store inserts new rows in-between rows by appending a new suffix
                 * to the row's key.) Keys are strings with terminating '/' values, so absent key
                 * corruption, we can simply do the underlying string conversion on the key string.
                 */
                keyno_prev = strtoul((char *)tinfo->key->data + g.prefix_len, NULL, 10);
                keyno = strtoul((char *)key.data + g.prefix_len, NULL, 10);
                if (incrementing) {
                    if (keyno_prev != keyno && keyno_prev + 1 != keyno)
                        goto order_error_row;
                } else if (keyno_prev != keyno && keyno_prev - 1 != keyno)
                    goto order_error_row;
            }
            if (0) {
order_error_row:
#ifdef HAVE_DIAGNOSTIC
                testutil_check(__wt_debug_cursor_page(cursor, g.home_pagedump));
#endif
                testutil_die(0, "%s returned {%.*s} then {%.*s}", which, (int)tinfo->key->size,
                  (char *)tinfo->key->data, (int)key.size, (char *)key.data);
            }

            testutil_check(__wt_buf_set(CUR2S(cursor), tinfo->key, key.data, key.size));
            break;
        }
        break;
    case WT_NOTFOUND:
        break;
    default:
        return (ret);
    }

    if (g.trace_all && ret == 0)
        switch (g.type) {
        case FIX:
            trace_op(tinfo, "%s %" PRIu64 " {0x%02x}", which, keyno, ((char *)value.data)[0]);
            break;
        case ROW:
            trace_op(tinfo, "%s %" PRIu64 " {%.*s}, {%s}", which, keyno, (int)key.size,
              (char *)key.data, trace_item(tinfo, &value));
            break;
        case VAR:
            trace_op(tinfo, "%s %" PRIu64 " {%s}", which, keyno, trace_item(tinfo, &value));
            break;
        }

    return (ret);
}

/*
 * row_reserve --
 *     Reserve a row in a row-store file.
 */
static int
row_reserve(TINFO *tinfo, WT_CURSOR *cursor, bool positioned)
{
    WT_DECL_RET;

    if (!positioned) {
        key_gen(tinfo->key, tinfo->keyno);
        cursor->set_key(cursor, tinfo->key);
    }

    if ((ret = cursor->reserve(cursor)) != 0)
        return (ret);

    trace_op(tinfo, "reserve %" PRIu64 " {%.*s}", tinfo->keyno, (int)tinfo->key->size,
      (char *)tinfo->key->data);

    return (0);
}

/*
 * col_reserve --
 *     Reserve a row in a column-store file.
 */
static int
col_reserve(TINFO *tinfo, WT_CURSOR *cursor, bool positioned)
{
    WT_DECL_RET;

    if (!positioned)
        cursor->set_key(cursor, tinfo->keyno);

    if ((ret = cursor->reserve(cursor)) != 0)
        return (ret);

    trace_op(tinfo, "reserve %" PRIu64, tinfo->keyno);

    return (0);
}

/*
 * modify_build --
 *     Generate a set of modify vectors.
 */
static void
modify_build(TINFO *tinfo, WT_MODIFY *entries, int *nentriesp)
{
    int i, nentries;

    /* Randomly select a number of byte changes, offsets and lengths. */
    nentries = (int)mmrand(&tinfo->rnd, 1, MAX_MODIFY_ENTRIES);
    for (i = 0; i < nentries; ++i) {
        entries[i].data.data = modify_repl + mmrand(&tinfo->rnd, 1, sizeof(modify_repl) - 10);
        entries[i].data.size = (size_t)mmrand(&tinfo->rnd, 0, 10);
        /*
         * Start at least 11 bytes into the buffer so we skip leading key information.
         */
        entries[i].offset = (size_t)mmrand(&tinfo->rnd, 20, 40);
        entries[i].size = (size_t)mmrand(&tinfo->rnd, 0, 10);
    }

    *nentriesp = (int)nentries;
}

/*
 * modify --
 *     Cursor modify worker function.
 */
static int
modify(TINFO *tinfo, WT_CURSOR *cursor, bool positioned)
{
    WT_MODIFY entries[MAX_MODIFY_ENTRIES];
    int nentries;
    bool modify_check;

    /* Periodically verify the WT_CURSOR.modify return. */
    modify_check = positioned && mmrand(&tinfo->rnd, 1, 10) == 1;
    if (modify_check) {
        testutil_check(cursor->get_value(cursor, &tinfo->moda));
        testutil_check(
          __wt_buf_set(CUR2S(cursor), &tinfo->moda, tinfo->moda.data, tinfo->moda.size));
    }

    modify_build(tinfo, entries, &nentries);
    WT_RET(cursor->modify(cursor, entries, nentries));

    testutil_check(cursor->get_value(cursor, tinfo->value));
    if (modify_check) {
        testutil_modify_apply(&tinfo->moda, &tinfo->modb, entries, nentries);
        testutil_assert(tinfo->moda.size == tinfo->value->size &&
          memcmp(tinfo->moda.data, tinfo->value->data, tinfo->moda.size) == 0);
    }
    return (0);
}

/*
 * row_modify --
 *     Modify a row in a row-store file.
 */
static int
row_modify(TINFO *tinfo, WT_CURSOR *cursor, bool positioned)
{

    if (!positioned) {
        key_gen(tinfo->key, tinfo->keyno);
        cursor->set_key(cursor, tinfo->key);
    }

    WT_RET(modify(tinfo, cursor, positioned));

    trace_op(tinfo, "modify %" PRIu64 " {%.*s}, {%s}", tinfo->keyno, (int)tinfo->key->size,
      (char *)tinfo->key->data, trace_item(tinfo, tinfo->value));

    return (0);
}

/*
 * col_modify --
 *     Modify a row in a column-store file.
 */
static int
col_modify(TINFO *tinfo, WT_CURSOR *cursor, bool positioned)
{
    if (!positioned)
        cursor->set_key(cursor, tinfo->keyno);

    WT_RET(modify(tinfo, cursor, positioned));

    trace_op(tinfo, "modify %" PRIu64 ", {%s}", tinfo->keyno, trace_item(tinfo, tinfo->value));

    return (0);
}

/*
 * row_truncate --
 *     Truncate rows in a row-store file.
 */
static int
row_truncate(TINFO *tinfo, WT_CURSOR *cursor)
{
    WT_CURSOR *c2;
    WT_DECL_RET;
    WT_SESSION *session;

    session = cursor->session;

    /*
     * The code assumes we're never truncating the entire object, assert that fact.
     */
    testutil_assert(tinfo->keyno != 0 || tinfo->last != 0);

    c2 = NULL;
    if (tinfo->keyno == 0) {
        key_gen(tinfo->key, tinfo->last);
        cursor->set_key(cursor, tinfo->key);
        ret = session->truncate(session, NULL, NULL, cursor, NULL);
    } else if (tinfo->last == 0) {
        key_gen(tinfo->key, tinfo->keyno);
        cursor->set_key(cursor, tinfo->key);
        ret = session->truncate(session, NULL, cursor, NULL, NULL);
    } else {
        key_gen(tinfo->key, tinfo->keyno);
        cursor->set_key(cursor, tinfo->key);

        testutil_check(session->open_cursor(session, g.uri, NULL, NULL, &c2));
        key_gen(tinfo->lastkey, tinfo->last);
        cursor->set_key(c2, tinfo->lastkey);

        ret = session->truncate(session, NULL, cursor, c2, NULL);
        testutil_check(c2->close(c2));
    }

    if (ret != 0)
        return (ret);

    trace_op(tinfo, "truncate %" PRIu64 ", %" PRIu64, "truncate", tinfo->keyno, tinfo->last);

    return (0);
}

/*
 * col_truncate --
 *     Truncate rows in a column-store file.
 */
static int
col_truncate(TINFO *tinfo, WT_CURSOR *cursor)
{
    WT_CURSOR *c2;
    WT_DECL_RET;
    WT_SESSION *session;

    session = cursor->session;

    /*
     * The code assumes we're never truncating the entire object, assert that fact.
     */
    testutil_assert(tinfo->keyno != 0 || tinfo->last != 0);

    c2 = NULL;
    if (tinfo->keyno == 0) {
        cursor->set_key(cursor, tinfo->last);
        ret = session->truncate(session, NULL, NULL, cursor, NULL);
    } else if (tinfo->last == 0) {
        cursor->set_key(cursor, tinfo->keyno);
        ret = session->truncate(session, NULL, cursor, NULL, NULL);
    } else {
        cursor->set_key(cursor, tinfo->keyno);

        testutil_check(session->open_cursor(session, g.uri, NULL, NULL, &c2));
        cursor->set_key(c2, tinfo->last);

        ret = session->truncate(session, NULL, cursor, c2, NULL);
        testutil_check(c2->close(c2));
    }
    if (ret != 0)
        return (ret);

    trace_op(tinfo, "truncate %" PRIu64 "-%" PRIu64, tinfo->keyno, tinfo->last);

    return (0);
}

/*
 * row_update --
 *     Update a row in a row-store file.
 */
static int
row_update(TINFO *tinfo, WT_CURSOR *cursor, bool positioned)
{
    WT_DECL_RET;

    if (!positioned) {
        key_gen(tinfo->key, tinfo->keyno);
        cursor->set_key(cursor, tinfo->key);
    }
    val_gen(&tinfo->rnd, tinfo->value, tinfo->keyno);
    cursor->set_value(cursor, tinfo->value);

    if ((ret = cursor->update(cursor)) != 0)
        return (ret);

    trace_op(tinfo, "update %" PRIu64 " {%.*s}, {%s}", tinfo->keyno, (int)tinfo->key->size,
      (char *)tinfo->key->data, trace_item(tinfo, tinfo->value));

    return (0);
}

/*
 * col_update --
 *     Update a row in a column-store file.
 */
static int
col_update(TINFO *tinfo, WT_CURSOR *cursor, bool positioned)
{
    WT_DECL_RET;

    if (!positioned)
        cursor->set_key(cursor, tinfo->keyno);
    val_gen(&tinfo->rnd, tinfo->value, tinfo->keyno);
    if (g.type == FIX)
        cursor->set_value(cursor, *(uint8_t *)tinfo->value->data);
    else
        cursor->set_value(cursor, tinfo->value);

    if ((ret = cursor->update(cursor)) != 0)
        return (ret);

    if (g.type == FIX)
        trace_op(tinfo, "update %" PRIu64 " {0x%02" PRIx8 "}", tinfo->keyno,
          ((uint8_t *)tinfo->value->data)[0]);
    else
        trace_op(tinfo, "update %" PRIu64 " {%s}", tinfo->keyno, trace_item(tinfo, tinfo->value));

    return (0);
}

/*
 * row_insert --
 *     Insert a row in a row-store file.
 */
static int
row_insert(TINFO *tinfo, WT_CURSOR *cursor, bool positioned)
{
    WT_DECL_RET;

    /*
     * If we positioned the cursor already, it's a test of an update using the insert method.
     * Otherwise, generate a unique key and insert.
     */
    if (!positioned) {
        key_gen_insert(&tinfo->rnd, tinfo->key, tinfo->keyno);
        cursor->set_key(cursor, tinfo->key);
    }
    val_gen(&tinfo->rnd, tinfo->value, tinfo->keyno);
    cursor->set_value(cursor, tinfo->value);

    if ((ret = cursor->insert(cursor)) != 0)
        return (ret);

    /* Log the operation */
    trace_op(tinfo, "insert %" PRIu64 " {%.*s}, {%s}", tinfo->keyno, (int)tinfo->key->size,
      (char *)tinfo->key->data, trace_item(tinfo, tinfo->value));

    return (0);
}

/*
 * col_insert_resolve --
 *     Resolve newly inserted records.
 */
static void
col_insert_resolve(TINFO *tinfo)
{
    uint64_t v, *p;
    u_int i;

    /*
     * We don't want to ignore column-store records we insert, which requires we update the "last
     * row" so other threads consider them. Threads allocating record numbers can race with other
     * threads, so the thread allocating record N may return after the thread allocating N + 1. We
     * can't update a record before it's been inserted, and so we can't leave gaps when the count of
     * records in the table is incremented.
     *
     * The solution is a per-thread array which contains an unsorted list of inserted records. If
     * there are pending inserts, we review the table after every operation, trying to update the
     * total rows. This is wasteful, but we want to give other threads immediate access to the row,
     * ideally they'll collide with our insert before we resolve.
     *
     * Process the existing records and advance the last row count until we can't go further.
     */
    do {
        WT_ORDERED_READ(v, g.rows);
        for (i = 0, p = tinfo->insert_list; i < WT_ELEMENTS(tinfo->insert_list); ++i, ++p) {
            if (*p == v + 1) {
                testutil_assert(__wt_atomic_casv64(&g.rows, v, v + 1));
                *p = 0;
                --tinfo->insert_list_cnt;
                break;
            }
            testutil_assert(*p == 0 || *p > v);
        }
    } while (tinfo->insert_list_cnt > 0 && i < WT_ELEMENTS(tinfo->insert_list));
}

/*
 * col_insert_add --
 *     Add newly inserted records.
 */
static void
col_insert_add(TINFO *tinfo)
{
    u_int i;

    /* Add the inserted record to the array. */
    for (i = 0; i < WT_ELEMENTS(tinfo->insert_list); ++i)
        if (tinfo->insert_list[i] == 0) {
            tinfo->insert_list[i] = tinfo->keyno;
            ++tinfo->insert_list_cnt;
            break;
        }
    testutil_assert(i < WT_ELEMENTS(tinfo->insert_list));
}

/*
 * col_insert --
 *     Insert an element in a column-store file.
 */
static int
col_insert(TINFO *tinfo, WT_CURSOR *cursor)
{
    WT_DECL_RET;

    val_gen(&tinfo->rnd, tinfo->value, g.rows + 1);
    if (g.type == FIX)
        cursor->set_value(cursor, *(uint8_t *)tinfo->value->data);
    else
        cursor->set_value(cursor, tinfo->value);

    /* Create a record, then add the key to our list of new records for later resolution. */
    if ((ret = cursor->insert(cursor)) != 0)
        return (ret);

    testutil_check(cursor->get_key(cursor, &tinfo->keyno));

    col_insert_add(tinfo); /* Extend the object. */

    if (g.type == FIX)
        trace_op(tinfo, "insert %" PRIu64 " {0x%02" PRIx8 "}", tinfo->keyno,
          ((uint8_t *)tinfo->value->data)[0]);
    else
        trace_op(tinfo, "insert %" PRIu64 " {%s}", tinfo->keyno, trace_item(tinfo, tinfo->value));

    return (0);
}

/*
 * row_remove --
 *     Remove an row from a row-store file.
 */
static int
row_remove(TINFO *tinfo, WT_CURSOR *cursor, bool positioned)
{
    WT_DECL_RET;

    if (!positioned) {
        key_gen(tinfo->key, tinfo->keyno);
        cursor->set_key(cursor, tinfo->key);
    }

    /* We use the cursor in overwrite mode, check for existence. */
    if ((ret = read_op(cursor, SEARCH, NULL)) == 0)
        ret = cursor->remove(cursor);

    if (ret != 0 && ret != WT_NOTFOUND)
        return (ret);

    trace_op(tinfo, "remove %" PRIu64, tinfo->keyno);

    return (ret);
}

/*
 * col_remove --
 *     Remove a row from a column-store file.
 */
static int
col_remove(TINFO *tinfo, WT_CURSOR *cursor, bool positioned)
{
    WT_DECL_RET;

    if (!positioned)
        cursor->set_key(cursor, tinfo->keyno);

    /* We use the cursor in overwrite mode, check for existence. */
    if ((ret = read_op(cursor, SEARCH, NULL)) == 0)
        ret = cursor->remove(cursor);

    if (ret != 0 && ret != WT_NOTFOUND)
        return (ret);

    trace_op(tinfo, "remove %" PRIu64, tinfo->keyno);

    return (ret);
}
