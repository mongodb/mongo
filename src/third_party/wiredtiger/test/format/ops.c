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

static void apply_bounds(WT_CURSOR *, TABLE *, WT_RAND_STATE *);
static void clear_bounds(WT_CURSOR *, TABLE *);
static int col_insert(TINFO *);
static void col_insert_resolve(TABLE *, void *);
static int col_modify(TINFO *, bool);
static int col_remove(TINFO *, bool);
static int col_reserve(TINFO *, bool);
static int col_truncate(TINFO *);
static int col_update(TINFO *, bool);
static int nextprev(TINFO *, bool);
static WT_THREAD_RET ops(void *);
static int read_row(TINFO *);
static void rollback_transaction(TINFO *);
static int row_insert(TINFO *, bool);
static int row_modify(TINFO *, bool);
static int row_remove(TINFO *, bool);
static int row_reserve(TINFO *, bool);
static int row_truncate(TINFO *);
static int row_update(TINFO *, bool);

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
 * modify_build --
 *     Generate a set of modify vectors.
 */
static void
modify_build(TINFO *tinfo)
{
    int i, nentries;

    /* Randomly select a number of byte changes, offsets and lengths. */
    nentries = (int)mmrand(&tinfo->data_rnd, 1, MAX_MODIFY_ENTRIES);
    for (i = 0; i < nentries; ++i) {
        tinfo->entries[i].data.data =
          modify_repl + mmrand(&tinfo->data_rnd, 1, sizeof(modify_repl) - 10);
        tinfo->entries[i].data.size = (size_t)mmrand(&tinfo->data_rnd, 0, 10);
        /*
         * Start at least 11 bytes into the buffer so we skip leading key information.
         */
        tinfo->entries[i].offset = (size_t)mmrand(&tinfo->data_rnd, 20, 40);
        tinfo->entries[i].size = (size_t)mmrand(&tinfo->data_rnd, 0, 10);
    }

    tinfo->nentries = nentries;
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
    set_core(true);

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
        tinfo_list = dcalloc((size_t)GV(RUNS_THREADS) + 1, sizeof(TINFO *));
        for (i = 0; i < GV(RUNS_THREADS); ++i) {
            tinfo_list[i] = dcalloc(1, sizeof(TINFO));
            tinfo = tinfo_list[i];

            tinfo->id = (int)i + 1;

            tinfo->cursors = dcalloc(WT_MAX(ntables, 1), sizeof(tinfo->cursors[0]));
            tinfo->col_insert = dcalloc(WT_MAX(ntables, 1), sizeof(tinfo->col_insert[0]));

            /* Set up the default key and value buffers. */
            tinfo->key = &tinfo->_key;
            key_gen_init(tinfo->key);
            tinfo->value = &tinfo->_value;
            val_gen_init(tinfo->value);
            tinfo->new_value = &tinfo->_new_value;
            val_gen_init(tinfo->new_value);
            tinfo->lastkey = &tinfo->_lastkey;
            key_gen_init(tinfo->lastkey);

            snap_init(tinfo);
        }
    }

    /* Cleanup for each new run. */
    for (i = 0; i < GV(RUNS_THREADS); ++i) {
        tinfo = tinfo_list[i];

        tinfo->ops = 0;
        tinfo->commit = 0;
        tinfo->insert = 0;
        tinfo->modify = 0;
        tinfo->prepare = 0;
        tinfo->remove = 0;
        tinfo->rollback = 0;
        tinfo->search = 0;
        tinfo->truncate = 0;
        tinfo->update = 0;

        tinfo->session = NULL;
        memset(tinfo->cursors, 0, WT_MAX(ntables, 1) * sizeof(tinfo->cursors[0]));
        memset(tinfo->col_insert, 0, WT_MAX(ntables, 1) * sizeof(tinfo->col_insert[0]));

        tinfo->state = TINFO_RUNNING;
        tinfo->quit = false;

        testutil_random_from_random(&tinfo->data_rnd, &g.data_rnd);
        testutil_random_from_random(&tinfo->extra_rnd, &g.extra_rnd);
    }
}

/*
 * lanes_init --
 *     Initialize the lanes structures.
 */
static void
lanes_init(void)
{
    uint32_t lane;

    /* Cleanup for each new run. */
    for (lane = 0; lane < LANE_COUNT; ++lane) {
        g.lanes[lane].in_use = false;
        g.lanes[lane].last_commit_ts = 0;
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

    for (i = 0; i < GV(RUNS_THREADS); ++i) {
        tinfo = tinfo_list[i];

        free(tinfo->cursors);
        free(tinfo->col_insert);

        __wt_buf_free(NULL, &tinfo->moda);
        __wt_buf_free(NULL, &tinfo->modb);

        snap_teardown(tinfo);
        key_gen_teardown(tinfo->key);
        val_gen_teardown(tinfo->value);
        val_gen_teardown(tinfo->new_value);
        key_gen_teardown(tinfo->lastkey);

        free(tinfo);
    }
    free(tinfo_list);
    tinfo_list = NULL;
}

/*
 * rollback_to_stable --
 *     Do a rollback to stable and verify operations.
 */
static void
rollback_to_stable(WT_SESSION *session)
{
    /* Rollback-to-stable only makes sense for timestamps. */
    if (!g.transaction_timestamps_config)
        return;

    /* Rollback the system. */
    testutil_check(g.wts_conn->rollback_to_stable(g.wts_conn, NULL));

    /*
     * Get the stable timestamp, and update ours. They should be the same, but there's no point in
     * debugging the race.
     */
    timestamp_query("get=stable", &g.stable_timestamp);
    trace_msg(session, "rollback-to-stable: stable timestamp %" PRIu64, g.stable_timestamp);

    /* Check the saved snap operations for consistency. */
    snap_repeat_rollback(session, tinfo_list, GV(RUNS_THREADS));

    /*
     * For a predictable run, the final stable timestamp is known and fixed, but individual threads
     * may have gone beyond that. Now that we've rolled back, set the current timestamp to the
     * stable so that next run starts from a known value.
     */
    if (GV(RUNS_PREDICTABLE_REPLAY))
        g.timestamp = g.stable_timestamp;
}

/*
 * operations --
 *     Perform a number of operations in a set of threads.
 */
void
operations(u_int ops_seconds, u_int run_current, u_int run_total)
{
    SAP sap;
    TINFO *tinfo, total;
    WT_CONNECTION *conn;
    WT_SESSION *session;
    wt_thread_t alter_tid, backup_tid, checkpoint_tid, compact_tid, hs_tid, import_tid, random_tid;
    wt_thread_t timestamp_tid;
    int64_t fourths, quit_fourths, thread_ops;
    uint32_t i;
    bool lastrun, running;

    conn = g.wts_conn;
    lastrun = (run_current == run_total);

    /* Make the modify pad character printable to simplify debugging and logging. */
    __wt_process.modify_pad_byte = FORMAT_PAD_BYTE;

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
     * If we have a number of operations with predictable replay, we set a stop timestamp. Without
     * predictable replay, each thread does an equal share of the total operations (and make sure
     * that it's not 0).
     *
     * With a timer, calculate how many fourth-of-a-second sleeps until the timer expires. If the
     * timer expires and threads don't return in 15 minutes, assume there is something hung, and
     * force the quit.
     */
    g.stop_timestamp = 0;
    if (GV(RUNS_OPS) == 0)
        thread_ops = -1;
    else {
        if (GV(RUNS_OPS) < GV(RUNS_THREADS))
            GV(RUNS_OPS) = GV(RUNS_THREADS);
        if (GV(RUNS_PREDICTABLE_REPLAY)) {
            /*
             * If running with an operation count for predictable replay, ignore other ways of
             * stopping.
             */
            thread_ops = -1;
            ops_seconds = 0;
            g.stop_timestamp = (GV(RUNS_OPS) * run_current) / run_total;
        } else
            thread_ops = GV(RUNS_OPS) / GV(RUNS_THREADS);
    }
    if (ops_seconds == 0)
        fourths = quit_fourths = -1;
    else {
        fourths = ops_seconds * 4;
        quit_fourths = fourths + 15 * 4 * 60;
    }

    /* Get a session. */
    memset(&sap, 0, sizeof(sap));
    wt_wrap_open_session(conn, &sap, NULL, &session);

    /* Initialize and start the worker threads. */
    lanes_init();
    tinfo_init();
    trace_msg(session, "%s", "=============== thread ops start");

    replay_run_begin(session);

    for (i = 0; i < GV(RUNS_THREADS); ++i) {
        tinfo = tinfo_list[i];
        testutil_check(__wt_thread_create(NULL, &tinfo->tid, ops, tinfo));
    }

    /* Start optional special-purpose threads. */
    if (GV(OPS_ALTER))
        testutil_check(__wt_thread_create(NULL, &alter_tid, alter, NULL));
    if (GV(BACKUP))
        testutil_check(__wt_thread_create(NULL, &backup_tid, backup, NULL));
    if (GV(OPS_COMPACTION))
        testutil_check(__wt_thread_create(NULL, &compact_tid, compact, NULL));
    if (GV(OPS_HS_CURSOR))
        testutil_check(__wt_thread_create(NULL, &hs_tid, hs_cursor, NULL));
    if (GV(IMPORT))
        testutil_check(__wt_thread_create(NULL, &import_tid, import, NULL));
    if (GV(OPS_RANDOM_CURSOR))
        testutil_check(__wt_thread_create(NULL, &random_tid, random_kv, NULL));
    if (g.transaction_timestamps_config)
        testutil_check(__wt_thread_create(NULL, &timestamp_tid, timestamp, tinfo_list));

    if (g.checkpoint_config == CHECKPOINT_ON)
        testutil_check(__wt_thread_create(NULL, &checkpoint_tid, checkpoint, NULL));

    /* Spin on the threads, calculating the totals. */
    for (;;) {
        /* Clear out the totals each pass. */
        memset(&total, 0, sizeof(total));
        for (i = 0, running = false; i < GV(RUNS_THREADS); ++i) {
            tinfo = tinfo_list[i];
            total.ops += tinfo->ops;
            total.commit += tinfo->commit;
            total.insert += tinfo->insert;
            total.modify += tinfo->modify;
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
                if (lastrun && GV(FORMAT_ABORT))
                    random_failure();

                /*
                 * Predictable replay cannot independently tag every thread to stop, we would end up
                 * with a mix of commits at the end of the run. Rather, later in this loop, when we
                 * see we are finishing, we give all threads stop timestamp that they must run to,
                 * but not exceed.
                 */
                if (!GV(RUNS_PREDICTABLE_REPLAY))
                    tinfo->quit = true;
            }
        }
        track_ops(&total);
        if (!running)
            break;
        __wt_sleep(0, 250 * WT_THOUSAND); /* 1/4th of a second */

        if (fourths == 1 && GV(RUNS_PREDICTABLE_REPLAY))
            replay_end_timed_run();
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
    if (GV(OPS_ALTER))
        testutil_check(__wt_thread_join(NULL, &alter_tid));
    if (GV(BACKUP))
        testutil_check(__wt_thread_join(NULL, &backup_tid));
    if (g.checkpoint_config == CHECKPOINT_ON)
        testutil_check(__wt_thread_join(NULL, &checkpoint_tid));
    if (GV(OPS_COMPACTION))
        testutil_check(__wt_thread_join(NULL, &compact_tid));
    if (GV(OPS_HS_CURSOR))
        testutil_check(__wt_thread_join(NULL, &hs_tid));
    if (GV(IMPORT))
        testutil_check(__wt_thread_join(NULL, &import_tid));
    if (GV(OPS_RANDOM_CURSOR))
        testutil_check(__wt_thread_join(NULL, &random_tid));
    if (g.transaction_timestamps_config)
        testutil_check(__wt_thread_join(NULL, &timestamp_tid));
    g.workers_finished = false;

    trace_msg(session, "%s", "=============== thread ops stop");

    /* Sanity check the truncation gate. */
    testutil_assert(g.truncate_cnt == 0);

    /*
     * The system should be quiescent at this point, call rollback to stable. Generally, we expect
     * applications to do rollback-to-stable as part of the database open, but calling it outside of
     * the open path is expected in the case of applications that are "restarting" but skipping the
     * close/re-open pair. Note we are not advancing the oldest timestamp, otherwise we wouldn't be
     * able to replay operations from after rollback-to-stable completes.
     */
    rollback_to_stable(session);

    replay_run_end(session);

    if (lastrun) {
        tinfo_teardown();
        timestamp_teardown(session);
    }

    wt_wrap_close_session(session);
}

/*
 * begin_transaction_ts --
 *     Begin a timestamped transaction.
 */
static void
begin_transaction_ts(TINFO *tinfo)
{
    WT_DECL_RET;
    WT_SESSION *session;
    uint64_t ts;

    session = tinfo->session;

    /* Pick a read timestamp. */
    if (GV(RUNS_PREDICTABLE_REPLAY))
        ts = replay_read_ts(tinfo);
    else
        /*
         * Transaction timestamp reads are repeatable, but read timestamps must be before any
         * possible commit timestamp. Without a read timestamp, reads are based on the transaction
         * snapshot, which will include the latest values as of when the snapshot is taken. Test in
         * both modes: 75% of the time, pick a read timestamp before any commit timestamp still in
         * use, 25% of the time don't set a timestamp at all.
         */
        ts = mmrand(&tinfo->data_rnd, 1, 4) == 1 ? 0 : timestamp_maximum_committed();
    if (ts != 0) {
        wt_wrap_begin_transaction(session, NULL);

        /*
         * If the timestamp has aged out of the system, we'll get EINVAL when we try and set it.
         * That kills the transaction, we have to restart.
         */
        ret = session->timestamp_transaction_uint(session, WT_TS_TXN_TYPE_READ, ts);
        if (ret == 0) {
            snap_op_init(tinfo, ts, true);
            trace_uri_op(tinfo, NULL, "begin snapshot read-ts=%" PRIu64 " (repeatable)", ts);
            return;
        }

        testutil_assert(ret == EINVAL);
        testutil_check(session->rollback_transaction(session, NULL));
    }

    wt_wrap_begin_transaction(session, NULL);

    snap_op_init(tinfo, ts, false);
    trace_uri_op(tinfo, NULL, "begin snapshot read-ts=%" PRIu64 " (not repeatable)", ts);
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

    wt_wrap_begin_transaction(session, iso_config);

    snap_op_init(tinfo, WT_TS_NONE, false);
    trace_uri_op(tinfo, NULL, "begin %s", iso_config);
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

    session = tinfo->session;

    ++tinfo->commit;

    ts = 0; /* -Wconditional-uninitialized */
    if (g.transaction_timestamps_config) {
        if (prepared)
            lock_readlock(session, &g.prepare_commit_lock);

        if (GV(RUNS_PREDICTABLE_REPLAY))
            ts = replay_commit_ts(tinfo);
        else
            ts = __wt_atomic_addv64(&g.timestamp, 1);
        testutil_check(session->timestamp_transaction_uint(session, WT_TS_TXN_TYPE_COMMIT, ts));

        if (prepared)
            testutil_check(
              session->timestamp_transaction_uint(session, WT_TS_TXN_TYPE_DURABLE, ts));

        testutil_check(session->commit_transaction(session, NULL));
        if (prepared)
            lock_readunlock(session, &g.prepare_commit_lock);
        replay_committed(tinfo);
    } else
        testutil_check(session->commit_transaction(session, NULL));

    /*
     * Remember our oldest commit timestamp. Updating the thread's commit timestamp allows read,
     * oldest and stable timestamps to advance, ensure we don't race.
     */
    WT_PUBLISH(tinfo->commit_ts, ts);

    trace_uri_op(tinfo, NULL, "commit read-ts=%" PRIu64 ", commit-ts=%" PRIu64, tinfo->read_ts,
      tinfo->commit_ts);
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
    replay_rollback(tinfo);

    trace_uri_op(tinfo, NULL, "abort read-ts=%" PRIu64, tinfo->read_ts);
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

    session = tinfo->session;

    ++tinfo->prepare;

    if (GV(RUNS_PREDICTABLE_REPLAY))
        ts = replay_prepare_ts(tinfo);
    else
        /*
         * Prepare timestamps must be less than or equal to the eventual commit timestamp. Set the
         * prepare timestamp to whatever the global value is now. The subsequent commit will
         * increment it, ensuring correctness.
         */
        ts = __wt_atomic_fetch_addv64(&g.timestamp, 1);
    testutil_check(session->timestamp_transaction_uint(session, WT_TS_TXN_TYPE_PREPARE, ts));
    ret = session->prepare_transaction(session, NULL);

    trace_uri_op(tinfo, NULL, "prepare ts=%" PRIu64, ts);

    return (ret);
}

/*
 * OP_FAILED --
 *	Error handling.
 */
#define OP_FAILED(notfound_ok)                                                               \
    do {                                                                                     \
        positioned = false;                                                                  \
        if (intxn && (ret == WT_CACHE_FULL || ret == WT_ROLLBACK))                           \
            return (WT_ROLLBACK);                                                            \
        testutil_assertfmt(                                                                  \
          (notfound_ok && ret == WT_NOTFOUND) || ret == WT_CACHE_FULL || ret == WT_ROLLBACK, \
          "operation failed: %d", ret);                                                      \
    } while (0)

/* Isolation configuration. */
typedef enum {
    ISOLATION_IMPLICIT,
    ISOLATION_READ_COMMITTED,
    ISOLATION_READ_UNCOMMITTED,
    ISOLATION_SNAPSHOT
} iso_level_t;

/* When in an explicit snapshot isolation transaction, track operations for later repetition. */
#define SNAP_TRACK(tinfo, op)                         \
    do {                                              \
        if (intxn && iso_level == ISOLATION_SNAPSHOT) \
            snap_track(tinfo, op);                    \
    } while (0)

/*
 * table_op --
 *     Per-thread table operation.
 */
static int
table_op(TINFO *tinfo, bool intxn, iso_level_t iso_level, thread_op op)
{
    WT_DECL_RET;
    TABLE *table;
    u_int i, j;
    bool bound_set, evict_page, next, positioned;

    bound_set = false;
    table = tinfo->table;

    /* Acquire a cursor. */
    tinfo->cursor = table_cursor(tinfo, table->id);

    /*
     * Predictable replay has some restrictions. Someday we may be able to resolve some of these
     * restrictions, this may require adding complexity.
     *
     * We disallow inserts into column stores, as column stores do inserts by expanding the number
     * of keys in the table. This has an interplay with other threads that are trying to predictably
     * generate key numbers since the key space is growing at a random time. Thus column stores are
     * restricted to accessing keys that were inserted via bulk load.
     */
    if (GV(RUNS_PREDICTABLE_REPLAY)) {
        if (table->type != ROW && op == INSERT)
            op = READ;
    }

    /*
     * Truncate has the key set to before/after rows in the table, skip pre-fetch and reserve for
     * simplicity.
     *
     * When the cursor is positioned in a row-store, inserts update existing records rather than
     * inserting new records. Inserted records are ignored during mirror checks (and updates to
     * those inserted records are ignored as well). The problem is if a row-store table updates an
     * original record and a different row-store or column-store table inserts a new record instead.
     * For this reason, always insert new records (or update previously inserted new records), when
     * inserting into a mirror group. For the same reason, don't reserve a row, that will position
     * the cursor and lead us into an update.
     */
    positioned = false;
    if (op != TRUNCATE && (op != INSERT || !table->mirror)) {
        /*
         * Inserts, removes and updates can be done following a cursor set-key, or based on a cursor
         * position taken from a previous search. If not already doing a read, position the cursor
         * at an existing point in the tree 20% of the time.
         */
        if (op != READ && mmrand(&tinfo->data_rnd, 1, 5) == 1) {
            ++tinfo->search;
            ret = read_row(tinfo);
            if (ret == 0) {
                positioned = true;
                SNAP_TRACK(tinfo, READ);
            } else
                OP_FAILED(true);
        }

        /*
         * If we're in a snapshot-isolation transaction, optionally reserve a row (it's an update so
         * can't be done at lower isolation levels). Reserving a row in an implicit transaction will
         * work, but doesn't make sense. Reserving a row before a read won't be useful but it's not
         * unexpected.
         */
        if (intxn && iso_level == ISOLATION_SNAPSHOT && mmrand(&tinfo->data_rnd, 0, 20) == 1) {
            switch (table->type) {
            case ROW:
                ret = row_reserve(tinfo, positioned);
                break;
            case FIX:
            case VAR:
                ret = col_reserve(tinfo, positioned);
                break;
            }
            if (ret == 0) {
                positioned = true;
                __wt_yield(); /* Encourage races */
            } else
                OP_FAILED(true);
        }
    }

    /* Perform the operation. */
    switch (op) {
    case INSERT:
        ++tinfo->insert;
        switch (table->type) {
        case ROW:
            ret = row_insert(tinfo, positioned);
            break;
        case FIX:
        case VAR:
            ret = col_insert(tinfo);
            break;
        }
        positioned = false; /* Insert never leaves the cursor positioned. */
        if (ret == 0) {
            SNAP_TRACK(tinfo, INSERT);
        } else
            OP_FAILED(false);
        break;
    case MODIFY:
        switch (table->type) {
        case FIX:
            ++tinfo->update; /* FLCS does an update instead of a modify. */
            ret = col_update(tinfo, positioned);
            break;
        case ROW:
            ++tinfo->modify;
            ret = row_modify(tinfo, positioned);
            break;
        case VAR:
            ++tinfo->modify;
            ret = col_modify(tinfo, positioned);
            break;
        }
        if (ret == 0) {
            positioned = true;
            SNAP_TRACK(tinfo, MODIFY);
        } else
            OP_FAILED(true);
        break;
    case READ:
        ++tinfo->search;

        if (!positioned && GV(OPS_BOUND_CURSOR) && mmrand(&tinfo->extra_rnd, 1, 2) == 1) {
            bound_set = true;
            /*
             * FIXME-WT-9883: It is possible that the underlying cursor is still positioned even
             * though the positioned variable is false. Reset the position through reset for now.
             */
            testutil_check(tinfo->cursor->reset(tinfo->cursor));
            apply_bounds(tinfo->cursor, tinfo->table, &tinfo->extra_rnd);
        }

        ret = read_row(tinfo);
        if (ret == 0) {
            positioned = true;
            if (!bound_set)
                SNAP_TRACK(tinfo, READ);
        } else {
            clear_bounds(tinfo->cursor, tinfo->table);
            OP_FAILED(true);
        }
        clear_bounds(tinfo->cursor, tinfo->table);
        break;
    case REMOVE:
        ++tinfo->remove;
        switch (table->type) {
        case ROW:
            ret = row_remove(tinfo, positioned);
            break;
        case FIX:
        case VAR:
            ret = col_remove(tinfo, positioned);
            break;
        }
        if (ret == 0) {
            /*
             * Don't set positioned: it's unchanged from the previous state, but not necessarily
             * set.
             */
            SNAP_TRACK(tinfo, REMOVE);
        } else
            OP_FAILED(true);
        break;
    case TRUNCATE:
        ++tinfo->truncate;
        switch (table->type) {
        case ROW:
            ret = row_truncate(tinfo);
            break;
        case FIX:
        case VAR:
            ret = col_truncate(tinfo);
            break;
        }
        positioned = false; /* Truncate never leaves the cursor positioned. */
        if (ret == 0) {
            SNAP_TRACK(tinfo, TRUNCATE);
        } else
            OP_FAILED(false);
        break;
    case UPDATE:
        ++tinfo->update;
        switch (table->type) {
        case ROW:
            ret = row_update(tinfo, positioned);
            break;
        case FIX:
        case VAR:
            ret = col_update(tinfo, positioned);
            break;
        }
        if (ret == 0) {
            positioned = true;
            SNAP_TRACK(tinfo, UPDATE);
        } else
            OP_FAILED(false);
        break;
    }

    /* Track the return, our caller needs it for modify cleanup. */
    tinfo->op_ret = ret;

    /*
     * If the cursor is positioned, do a small number of next/prev cursor operations in a random
     * direction.
     */
    if (positioned) {
        next = mmrand(&tinfo->extra_rnd, 0, 1) == 1;
        j = mmrand(&tinfo->extra_rnd, 1, 100);
        for (i = 0; i < j; ++i) {
            if ((ret = nextprev(tinfo, next)) == 0)
                continue;

            OP_FAILED(true);
            break;
        }
    }

    /*
     * Reset the cursor: there is no reason to keep pages pinned, periodically forcibly evict the
     * underlying page.
     */
    evict_page = mmrand(&tinfo->extra_rnd, 1, 20) == 1;
    if (evict_page)
        F_SET(tinfo->cursor, WT_CURSTD_DEBUG_RESET_EVICT);
    testutil_check(tinfo->cursor->reset(tinfo->cursor));
    if (evict_page)
        F_CLR(tinfo->cursor, WT_CURSTD_DEBUG_RESET_EVICT);

    /* Failure already returned if in a transaction (meaning failure requires action). */
    return (0);
}

/*
 * ops_session_open --
 *     Create a new session for the thread.
 */
static void
ops_session_open(TINFO *tinfo)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;

    conn = g.wts_conn;

    /* Close any open session (which closes all open cursors as well). */
    if ((session = tinfo->session) != NULL)
        wt_wrap_close_session(session);
    tinfo->session = NULL;
    memset(tinfo->cursors, 0, WT_MAX(ntables, 1) * sizeof(tinfo->cursors[0]));

    wt_wrap_open_session(conn, &tinfo->sap, NULL, &session);
    tinfo->session = session;
}

/*
 * ops --
 *     Per-thread operations.
 */
static WT_THREAD_RET
ops(void *arg)
{
    TINFO *tinfo;
    TABLE *skip1, *skip2, *table;
    WT_DECL_RET;
    WT_SESSION *session;
    iso_level_t iso_level;
    thread_op op;
    uint64_t reset_op, session_op, truncate_op;
    uint32_t max_rows, ntries, range, rnd;
    u_int i;
    const char *iso_config;
    bool greater_than, intxn, prepared;

    tinfo = arg;

    /*
     * Characterize the per-thread random number generator. Normally we want independent behavior so
     * threads start in different parts of the RNG space, but we've found bugs by having the threads
     * pound on the same key/value pairs, that is, by making them traverse the same RNG space. 75%
     * of the time we run in independent RNG space.
     */
    if (GV(FORMAT_INDEPENDENT_THREAD_RNG)) {
        testutil_random_from_seed(&tinfo->data_rnd, GV(RANDOM_DATA_SEED) + (u_int)tinfo->id);
        testutil_random_from_seed(&tinfo->extra_rnd, GV(RANDOM_EXTRA_SEED) + (u_int)tinfo->id);
    } else {
        testutil_random_from_seed(&tinfo->data_rnd, GV(RANDOM_DATA_SEED));
        testutil_random_from_seed(&tinfo->extra_rnd, GV(RANDOM_EXTRA_SEED));
    }

    iso_level = ISOLATION_SNAPSHOT; /* -Wconditional-uninitialized */
    tinfo->replay_again = false;
    tinfo->lane = LANE_NONE;

    /* Set the first operation where we'll create a new session and cursors. */
    session = NULL;
    session_op = 0;
    ntries = 0;

    /* Set the first operation where we'll reset the session. */
    reset_op = mmrand(&tinfo->extra_rnd, 100, 10 * WT_THOUSAND);
    /* Set the first operation where we'll truncate a range. */
    truncate_op = mmrand(&tinfo->data_rnd, 100, 10 * WT_THOUSAND);

    for (intxn = false; !tinfo->quit;) {
rollback_retry:
        if (tinfo->quit)
            break;

        ++tinfo->ops;

        if (!tinfo->replay_again)
            /*
             * Number of failures so far for the current operation and key. In predictable replay,
             * unless we have a read operation, we cannot give up on any operation and maintain the
             * integrity of the replay.
             */
            ntries = 0;

        /* Number of tries only gets incremented during predictable replay. */
        testutil_assert(ntries == 0 || (!intxn && tinfo->replay_again));

        /*
         * In predictable replay, put each operation in its own transaction. It's possible we could
         * make multiple operations work predictably in the future.
         */
        if (intxn && GV(RUNS_PREDICTABLE_REPLAY)) {
            commit_transaction(tinfo, false);
            intxn = false;
        }

        replay_loop_begin(tinfo, intxn);
        if (tinfo->quit)
            break;

        /* Periodically open up a new session and cursors. */
        if (tinfo->ops > session_op) {
            /* Resolve any running transaction. */
            if (intxn) {
                commit_transaction(tinfo, false);
                intxn = false;
            }

            ops_session_open(tinfo);
            session = tinfo->session;

            /* Pick the next session/cursor close/open. */
            session_op += mmrand(&tinfo->extra_rnd, 100, 5 * WT_THOUSAND);
        }

        /* If not in a transaction, reset the session periodically so that operation is tested. */
        if (!intxn && tinfo->ops > reset_op) {
            testutil_check(session->reset(session));

            /* Pick the next reset operation. */
            reset_op += mmrand(&tinfo->extra_rnd, 40 * WT_THOUSAND, 60 * WT_THOUSAND);
        }

        /*
         * If not in a transaction and in a timestamp world, occasionally repeat timestamped
         * operations.
         */
        if (!intxn && g.transaction_timestamps_config && mmrand(&tinfo->extra_rnd, 1, 15) == 1) {
            ++tinfo->search;
            snap_repeat_single(tinfo);
        }

        /* Select a table. */
        table = tinfo->table = table_select(tinfo, true);

        /*
         * If not in a transaction and in a timestamp world, start a transaction (which is always at
         * snapshot-isolation).
         *
         * If not in a transaction and not in a timestamp world, start a transaction some percentage
         * of the time, otherwise it's an implicit transaction. (Mirror operations require explicit
         * transactions.)
         */
        if (!intxn && g.transaction_timestamps_config) {
            iso_level = ISOLATION_SNAPSHOT;
            begin_transaction_ts(tinfo);
            intxn = true;
        }
        if (!intxn) {
            testutil_assert(!GV(RUNS_PREDICTABLE_REPLAY));
            iso_level = ISOLATION_IMPLICIT;

            if (table->mirror || mmrand(&tinfo->data_rnd, 1, 100) < GV(TRANSACTION_IMPLICIT)) {
                iso_level = ISOLATION_SNAPSHOT;
                iso_config = "isolation=snapshot";

                /* Occasionally do reads at an isolation level lower than snapshot. */
                switch (mmrand(&tinfo->data_rnd, 1, 20)) {
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
        }

        /*
         * Select an operation: updates cannot happen at lower isolation levels and modify must be
         * in an explicit transaction.
         */
        op = READ;
        if (iso_level == ISOLATION_IMPLICIT || iso_level == ISOLATION_SNAPSHOT) {
            i = mmrand(&tinfo->data_rnd, 1, 100);
            if (i < TV(OPS_PCT_DELETE)) {
                op = REMOVE;
                if (TV(OPS_TRUNCATE) && tinfo->ops > truncate_op) {
                    /* Limit test runs to a maximum of 4 truncation operations at a time. */
                    if (__wt_atomic_addv64(&g.truncate_cnt, 1) > 4)
                        (void)__wt_atomic_subv64(&g.truncate_cnt, 1);
                    else
                        op = TRUNCATE;

                    /* Pick the next truncate operation. */
                    truncate_op += mmrand(&tinfo->data_rnd, 20 * WT_THOUSAND, 100 * WT_THOUSAND);
                }
            } else if (i < TV(OPS_PCT_DELETE) + TV(OPS_PCT_INSERT))
                op = INSERT;
            else if (intxn && i < TV(OPS_PCT_DELETE) + TV(OPS_PCT_INSERT) + TV(OPS_PCT_MODIFY))
                op = MODIFY;
            else if (i <
              TV(OPS_PCT_DELETE) + TV(OPS_PCT_INSERT) + TV(OPS_PCT_MODIFY) + TV(OPS_PCT_WRITE))
                op = UPDATE;
        }
        tinfo->op = op; /* Keep the op in the thread info for debugging */

        /* Make sure this is an operation that is permitted for this kind of run. */
        testutil_assert(replay_operation_enabled(op));

        /*
         * Get the number of rows. Column-store extends the object, use that extended count if this
         * isn't a mirrored operation. (Ignore insert column-store insert operations in this check,
         * column-store will allocate a key after the end of the current table inside WiredTiger.)
         */
        max_rows = TV(RUNS_ROWS);
        if (table->type != ROW && !table->mirror)
            WT_ORDERED_READ(max_rows, table->rows_current);
        tinfo->keyno = mmrand(&tinfo->data_rnd, 1, (u_int)max_rows);
        if (TV(OPS_PARETO)) {
            tinfo->keyno = testutil_pareto(tinfo->keyno, (u_int)max_rows, TV(OPS_PARETO_SKEW));
            if (tinfo->keyno == 0)
                tinfo->keyno++;
        }
        replay_adjust_key(tinfo, max_rows);

        /*
         * If the operation is a truncate, select a range.
         *
         * Truncate up to 2% of the table (keep truncate ranges relatively short so they complete
         * without colliding with other operations, but still cross page boundaries. If the range
         * overlaps the beginning/end of the table, set the key to 0 (the truncate function then
         * sets a cursor to NULL so that code is tested).
         *
         * This gets tricky: there are 2 directions (truncating from lower keys to the current
         * position or from the current position to higher keys), and collation order (truncating
         * from lower keys to higher keys or vice-versa).
         */
        if (op == TRUNCATE) {
            tinfo->last = tinfo->keyno = mmrand(&tinfo->data_rnd, 1, (u_int)max_rows);
            greater_than = mmrand(&tinfo->data_rnd, 0, 1) == 1;
            range = max_rows < 20 ? 0 : mmrand(&tinfo->data_rnd, 0, (u_int)max_rows / 50);
            if (greater_than) {
                if (TV(BTREE_REVERSE)) {
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
                if (TV(BTREE_REVERSE)) {
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
        }

        /*
         * If an insert or update, create a value.
         *
         * If the first table we're updating is FLCS and a mirrored table, use the base table (which
         * must be ROW or VLCS), to create a value usable for any table. Because every FLCS table
         * tracks a different number of bits, we can't figure out the specific bits we're going to
         * use until the insert or update call that's going to do the modify.
         *
         * If the first table we're updating is FLCS and not a mirrored table, we use the table
         * we're modifying and acquire the bits for the table immediately.
         *
         * See the column-store update/insert calls for the matching work, if the table is mirrored,
         * we derive the bits based on the ROW/VLCS value, otherwise, there's nothing to do, we have
         * the bits we need.
         *
         * If the first table we're updating isn't FLCS, generate the new value for the table, no
         * special work is done here and the column-store insert/update calls will create derive the
         * necessary bits if/when a mirrored FLCS table is updated in this operation.
         */
        if (op == INSERT || op == UPDATE) {
            if (table->type == FIX && table->mirror)
                val_gen(
                  g.base_mirror, &tinfo->data_rnd, tinfo->new_value, &tinfo->bitv, tinfo->keyno);
            else
                val_gen(table, &tinfo->data_rnd, tinfo->new_value, &tinfo->bitv, tinfo->keyno);
        }

        /*
         * If modify, build a modify change vector. FLCS operations do updates instead of modifies,
         * if we're not in a mirrored group, generate a bit value for the FLCS table. If we are in a
         * mirrored group or not modifying an FLCS table, we'll need a change vector and we will
         * have to modify a ROW/VLCS table first to get a new value from which we can derive the
         * FLCS value.
         */
        if (op == MODIFY) {
            if (table->type != FIX || table->mirror)
                modify_build(tinfo);
            else
                val_gen(table, &tinfo->data_rnd, tinfo->new_value, &tinfo->bitv, tinfo->keyno);
        }

        /*
         * For modify we haven't created the new value when we queue up the operation; we have to
         * modify a RS or VLCS table first so we have a value from which we can set any FLCS values
         * we need. In that case, do the modify on the base mirror table first. Then, do the
         * operation on the selected table, then any remaining tables.
         */
        ret = 0;
        skip1 = skip2 = NULL;
        if (op == MODIFY && table->mirror) {
            tinfo->table = g.base_mirror;
            ret = table_op(tinfo, intxn, iso_level, op);
            testutil_assert(ret == 0 || ret == WT_ROLLBACK);

            /*
             * We make blind modifies and the record may not exist. If the base modify returns DNE,
             * skip the operation. This isn't to avoid wasted work: any FLCS table in the mirrored
             * will do an update as FLCS doesn't support modify, and we'll fail when we compare the
             * remove to the FLCS value.
             *
             * For predictable replay if the record doesn't exist (that's predictable), and we must
             * force a rollback, we always finish a loop iteration in a committed or rolled back
             * state.
             */
            if (GV(RUNS_PREDICTABLE_REPLAY) && (ret == WT_ROLLBACK || tinfo->op_ret == WT_NOTFOUND))
                goto rollback;

            if (tinfo->op_ret == WT_NOTFOUND)
                goto skip_operation;

            skip1 = g.base_mirror;
        }
        if (ret == 0 && table != skip1) {
            tinfo->table = table;
            ret = table_op(tinfo, intxn, iso_level, op);
            testutil_assert(ret == 0 || ret == WT_ROLLBACK);
            if (GV(RUNS_PREDICTABLE_REPLAY) && ret == WT_ROLLBACK)
                goto rollback;
            skip2 = table;
        }
        if (ret == 0 && table->mirror)
            for (i = 1; i <= ntables; ++i)
                if (tables[i] != skip1 && tables[i] != skip2 && tables[i]->mirror) {
                    tinfo->table = tables[i];
                    ret = table_op(tinfo, intxn, iso_level, op);
                    testutil_assert(ret == 0 || ret == WT_ROLLBACK);
                    if (GV(RUNS_PREDICTABLE_REPLAY) && ret == WT_ROLLBACK)
                        goto rollback;
                    if (ret == WT_ROLLBACK)
                        break;
                }
skip_operation:
        table = tinfo->table = NULL;

        /* Release the truncate operation counter. */
        if (op == TRUNCATE)
            (void)__wt_atomic_subv64(&g.truncate_cnt, 1);

        /* Drain any pending column-store inserts. */
        if (g.column_store_config)
            tables_apply(col_insert_resolve, tinfo);

        /* On failure, rollback any running transaction. */
        if (intxn && ret != 0)
            goto rollback;

        /*
         * If not in a transaction, we're done with this operation. If in a transaction, add more
         * operations to the transaction half the time. For predictable replay runs, always complete
         * the transaction.
         */
        if (GV(RUNS_PREDICTABLE_REPLAY)) {
            rnd = mmrand(&tinfo->data_rnd, 1, 5);

            /*
             * Note that a random value of 5 would result in a rollback per the switch below. For
             * predictable replay, only do that once per timestamp. If we didn't have this check, a
             * retry would start again with the same timestamp and RNG state, and get the same dice
             * roll. This would happen every time and the thread will be get stuck doing continuous
             * rollbacks.
             */
            if (rnd == 5 && ntries != 0)
                rnd = 4; /* Choose to do a commit this time. */
        } else if (!intxn || (rnd = mmrand(&tinfo->data_rnd, 1, 10)) > 5)
            continue;

        /*
         * Ending a transaction. If configured for snapshot isolation, redo the reads and confirm
         * the values are unchanged.
         */
        if (iso_level == ISOLATION_SNAPSHOT) {
            __wt_yield(); /* Encourage races */

            ret = snap_repeat_txn(tinfo);
            testutil_assertfmt(
              ret == 0 || ret == WT_ROLLBACK || ret == WT_CACHE_FULL, "operation failed: %d", ret);
            if (ret == WT_ROLLBACK || ret == WT_CACHE_FULL)
                goto rollback;
        }

        /*
         * If prepare configured, prepare the transaction 10% of the time. Note prepare requires a
         * timestamped world, which means we're in a snapshot-isolation transaction by definition.
         */
        prepared = false;
        if (GV(OPS_PREPARE) && mmrand(&tinfo->data_rnd, 1, 10) == 1) {
            if ((ret = prepare_transaction(tinfo)) != 0) {
                testutil_assert(ret == WT_ROLLBACK);
                goto rollback;
            }
            prepared = true;
        }

        /*
         * If we're in a transaction, commit 40% of the time and rollback 10% of the time (we
         * already continued to add operations to the transaction the remaining half of the time).
         */
        switch (rnd) {
        case 1:
        case 2:
        case 3:
        case 4:           /* 40% */
            __wt_yield(); /* Encourage races */
            commit_transaction(tinfo, prepared);
            snap_repeat_update(tinfo, true);
            break;
        case 5: /* 10% */
rollback:
            if (GV(RUNS_PREDICTABLE_REPLAY)) {
                if (tinfo->quit)
                    goto loop_exit;
                /* Force a rollback */
                testutil_assert(intxn);
                rollback_transaction(tinfo);
                intxn = false;
                ++ntries;
                replay_pause_after_rollback(tinfo, ntries);
                ret = 0;
                goto rollback_retry;
            }
            __wt_yield(); /* Encourage races */
            rollback_transaction(tinfo);
            snap_repeat_update(tinfo, false);
            break;
        }

        intxn = false;
    }

loop_exit:
    if (session != NULL)
        testutil_check(session->close(session, NULL));
    tinfo->session = NULL;
    memset(tinfo->cursors, 0, WT_MAX(ntables, 1) * sizeof(tinfo->cursors[0]));

    tinfo->state = TINFO_COMPLETE;
    return (WT_THREAD_RET_VALUE);
}

/*
 * read_row_worker --
 *     Read and verify a single element in a row- or column-store file.
 */
static int
read_row_worker(TINFO *tinfo, TABLE *table, WT_CURSOR *cursor, uint64_t keyno, WT_ITEM *key,
  WT_ITEM *value, uint8_t *bitvp, bool sn)
{
    int exact, ret;

    *bitvp = FIX_VALUE_WRONG; /* -Wconditional-uninitialized */
    value->data = NULL;
    value->size = 0;

    if (table == NULL)
        table = tinfo->table;

    /* Retrieve the key/value pair by key. */
    switch (table->type) {
    case FIX:
    case VAR:
        cursor->set_key(cursor, keyno);
        break;
    case ROW:
        key_gen(table, key, keyno);
        cursor->set_key(cursor, key);
        break;
    }

    /*
     * We don't use search near for predictable replay runs, as the return key can be variable
     * depending on the structure of the Btree.
     */
    if (sn && !GV(RUNS_PREDICTABLE_REPLAY)) {
        ret = read_op(cursor, SEARCH_NEAR, &exact);
        if (ret == 0 && exact != 0)
            ret = WT_NOTFOUND;
    } else
        ret = read_op(cursor, SEARCH, NULL);
    switch (ret) {
    case 0:
        if (table->type == FIX)
            testutil_check(cursor->get_value(cursor, bitvp));
        else
            testutil_check(cursor->get_value(cursor, value));
        break;
    case WT_NOTFOUND:
        /*
         * Zero values at the end of the key space in fixed length stores are returned as not-found.
         * The WiredTiger cursor has lost its position though, so we return not-found, the cursor
         * movement can't continue.
         */
        if (table->type == FIX)
            *bitvp = 0;
        break;
    }
    if (ret != 0)
        return (ret);

    /* Log the operation */
    if (!FLD_ISSET(g.trace_flags, TRACE_READ))
        return (0);
    switch (table->type) {
    case FIX:
        if (tinfo == NULL)
            trace_msg(cursor->session, "read %" PRIu64 " {0x%02" PRIx8 "}", keyno, *bitvp);
        else
            trace_op(tinfo, "read %" PRIu64 " {0x%02" PRIx8 "}", keyno, *bitvp);

        break;
    case ROW:
        if (tinfo == NULL)
            trace_msg(cursor->session, "read %" PRIu64 " {%.*s}, {%.*s}", keyno, (int)key->size,
              (char *)key->data, (int)value->size, (char *)value->data);
        else
            trace_op(tinfo, "read %" PRIu64 " {%.*s}, {%.*s}", keyno, (int)key->size,
              (char *)key->data, (int)value->size, (char *)value->data);
        break;
    case VAR:
        if (tinfo == NULL)
            trace_msg(cursor->session, "read %" PRIu64 " {%.*s}", keyno, (int)value->size,
              (char *)value->data);
        else
            trace_op(
              tinfo, "read %" PRIu64 " {%.*s}", keyno, (int)value->size, (char *)value->data);
        break;
    }
    return (0);
}

/*
 * apply_bounds --
 *     Apply lower and upper bounds on the cursor. The lower and upper bound is randomly generated.
 */
static void
apply_bounds(WT_CURSOR *cursor, TABLE *table, WT_RAND_STATE *rnd)
{
    WT_ITEM key;
    uint32_t lower_keyno, max_rows, upper_keyno;

    /* FLCS is not supported with bounds. */
    if (table->type == FIX)
        return;

    /* Set up the default key buffer. */
    key_gen_init(&key);
    WT_ORDERED_READ(max_rows, table->rows_current);

    /*
     * Generate a random lower key and apply to the lower bound or upper bound depending on the
     * reverse collator.
     */
    lower_keyno = mmrand(rnd, 1, max_rows);
    /* Retrieve the key/value pair by key. */
    switch (table->type) {
    case FIX:
    case VAR:
        cursor->set_key(cursor, lower_keyno);
        break;
    case ROW:
        key_gen(table, &key, lower_keyno);
        cursor->set_key(cursor, &key);
        break;
    }
    if (TV(BTREE_REVERSE))
        testutil_check(cursor->bound(cursor, "action=set,bound=upper"));
    else
        testutil_check(cursor->bound(cursor, "action=set,bound=lower"));

    /*
     * Generate a random upper key and apply to the upper bound or lower bound depending on the
     * reverse collator.
     */
    upper_keyno = mmrand(rnd, lower_keyno, max_rows);

    /* Retrieve the key/value pair by key. */
    switch (table->type) {
    case FIX:
    case VAR:
        cursor->set_key(cursor, upper_keyno);
        break;
    case ROW:
        key_gen(table, &key, upper_keyno);
        cursor->set_key(cursor, &key);
        break;
    }
    if (TV(BTREE_REVERSE))
        testutil_check(cursor->bound(cursor, "action=set,bound=upper"));
    else
        testutil_check(cursor->bound(cursor, "action=set,bound=lower"));

    key_gen_teardown(&key);
}

/*
 * clear_bounds --
 *     Clear both the lower and upper bounds on the cursor.
 */
static void
clear_bounds(WT_CURSOR *cursor, TABLE *table)
{
    /* FLCS is not supported with bounds. */
    if (table->type == FIX)
        return;

    cursor->bound(cursor, "action=clear");
}

/*
 * wts_read_scan --
 *     Read and verify a subset of the elements in a file.
 */
void
wts_read_scan(TABLE *table, void *args)
{
    SAP sap;
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_ITEM key, value;
    WT_RAND_STATE *rnd;
    WT_SESSION *session;
    uint64_t keyno;
    uint32_t max_rows;
    uint8_t bitv;

    testutil_assert(table != NULL);
    conn = ((READ_SCAN_ARGS *)args)->conn;
    rnd = ((READ_SCAN_ARGS *)args)->rnd;

    /*
     * We're not configuring transactions or read timestamps: if there's a diagnostic check that all
     * operations are timestamped transactions, skip the scan.
     */
    if (GV(ASSERT_READ_TIMESTAMP))
        return;

    /* Set up the default key/value buffers. */
    key_gen_init(&key);
    val_gen_init(&value);

    /* Open a session and cursor pair. */
    memset(&sap, 0, sizeof(sap));
    wt_wrap_open_session(conn, &sap, NULL, &session);
    wt_wrap_open_cursor(session, table->uri, NULL, &cursor);

    /* Scan the first 50 rows for tiny, debugging runs, then scan a random subset of records. */
    WT_ORDERED_READ(max_rows, table->rows_current);
    for (keyno = 0; keyno < max_rows;) {
        if (++keyno > 50)
            keyno += mmrand(rnd, 1, WT_THOUSAND);
        if (keyno > max_rows)
            keyno = max_rows;

        if (GV(OPS_BOUND_CURSOR) && mmrand(rnd, 1, 10) == 1) {
            /* Reset the position of the cursor, so that we can apply bounds on the cursor. */
            testutil_check(cursor->reset(cursor));
            apply_bounds(cursor, table, rnd);
        }

        switch (ret = read_row_worker(NULL, table, cursor, keyno, &key, &value, &bitv, false)) {
        case 0:
        case WT_NOTFOUND:
        case WT_ROLLBACK:
        case WT_CACHE_FULL:
        case WT_PREPARE_CONFLICT:
            break;
        default:
            testutil_die(ret, "%s: read row %" PRIu64, __func__, keyno);
        }
        clear_bounds(cursor, table);
    }

    wt_wrap_close_session(session);

    key_gen_teardown(&key);
    val_gen_teardown(&value);
}

/*
 * read_row --
 *     Read and verify a single element in a row- or column-store file.
 */
static int
read_row(TINFO *tinfo)
{
    /* 25% of the time we call search-near. */
    return (read_row_worker(tinfo, NULL, tinfo->cursor, tinfo->keyno, tinfo->key, tinfo->value,
      &tinfo->bitv, mmrand(&tinfo->extra_rnd, 0, 3) == 1));
}

/*
 * nextprev --
 *     Read and verify the next/prev element in a row- or column-store file.
 */
static int
nextprev(TINFO *tinfo, bool next)
{
    TABLE *table;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_ITEM key, value;
    uint64_t keyno;
    uint8_t bitv;
    const char *which;

    table = tinfo->table;
    cursor = tinfo->cursor;
    keyno = 0;
    bitv = FIX_VALUE_WRONG; /* -Wconditional-uninitialized */

    if ((ret = read_op(cursor, next ? NEXT : PREV, NULL)) != 0)
        return (ret);

    switch (table->type) {
    case FIX:
        testutil_check(cursor->get_key(cursor, &keyno));
        testutil_check(cursor->get_value(cursor, &bitv));
        break;
    case ROW:
        testutil_check(cursor->get_key(cursor, &key));
        testutil_check(cursor->get_value(cursor, &value));
        break;
    case VAR:
        testutil_check(cursor->get_key(cursor, &keyno));
        testutil_check(cursor->get_value(cursor, &value));
        break;
    }

    if (FLD_ISSET(g.trace_flags, TRACE_CURSOR)) {
        which = next ? "next" : "prev";
        switch (table->type) {
        case FIX:
            trace_op(tinfo, "%s %" PRIu64 " {0x%02" PRIx8 "}", which, keyno, bitv);
            break;
        case ROW:
            trace_op(tinfo, "%s {%.*s}, {%.*s}", which, (int)key.size, (char *)key.data,
              (int)value.size, (char *)value.data);
            break;
        case VAR:
            trace_op(
              tinfo, "%s %" PRIu64 " {%.*s}", which, keyno, (int)value.size, (char *)value.data);
            break;
        }
    }
    return (0);
}

/*
 * row_reserve --
 *     Reserve a row in a row-store file.
 */
static int
row_reserve(TINFO *tinfo, bool positioned)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;

    cursor = tinfo->cursor;

    if (!positioned) {
        key_gen(tinfo->table, tinfo->key, tinfo->keyno);
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
col_reserve(TINFO *tinfo, bool positioned)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;

    cursor = tinfo->cursor;

    if (!positioned)
        cursor->set_key(cursor, tinfo->keyno);

    if ((ret = cursor->reserve(cursor)) != 0)
        return (ret);

    trace_op(tinfo, "reserve %" PRIu64, tinfo->keyno);

    return (0);
}

/*
 * modify --
 *     Cursor modify worker function.
 */
static int
modify(TINFO *tinfo, WT_CURSOR *cursor, bool positioned)
{
    bool modify_check;

    /* Periodically verify the WT_CURSOR.modify return. */
    modify_check = positioned && mmrand(&tinfo->extra_rnd, 1, 20) == 1;
    if (modify_check) {
        testutil_check(cursor->get_value(cursor, &tinfo->moda));
        testutil_check(
          __wt_buf_set(CUR2S(cursor), &tinfo->moda, tinfo->moda.data, tinfo->moda.size));
    }

    WT_RET(cursor->modify(cursor, tinfo->entries, tinfo->nentries));

    testutil_check(cursor->get_value(cursor, tinfo->new_value));

    if (modify_check) {
        testutil_modify_apply(
          &tinfo->moda, &tinfo->modb, tinfo->entries, tinfo->nentries, FORMAT_PAD_BYTE);
        testutil_assert(tinfo->moda.size == tinfo->new_value->size &&
          (tinfo->moda.size == 0 ||
            memcmp(tinfo->moda.data, tinfo->new_value->data, tinfo->moda.size) == 0));
    }
    return (0);
}

/*
 * row_modify --
 *     Modify a row in a row-store file.
 */
static int
row_modify(TINFO *tinfo, bool positioned)
{
    WT_CURSOR *cursor;

    cursor = tinfo->cursor;

    if (!positioned) {
        key_gen(tinfo->table, tinfo->key, tinfo->keyno);
        cursor->set_key(cursor, tinfo->key);
    }

    WT_RET(modify(tinfo, cursor, positioned));

    trace_op(tinfo, "modify %" PRIu64 " {%.*s}, {%.*s}", tinfo->keyno, (int)tinfo->key->size,
      (char *)tinfo->key->data, (int)tinfo->new_value->size, (char *)tinfo->new_value->data);

    return (0);
}

/*
 * col_modify --
 *     Modify a row in a column-store file.
 */
static int
col_modify(TINFO *tinfo, bool positioned)
{
    WT_CURSOR *cursor;

    cursor = tinfo->cursor;

    if (!positioned)
        cursor->set_key(cursor, tinfo->keyno);

    WT_RET(modify(tinfo, cursor, positioned));

    trace_op(tinfo, "modify %" PRIu64 ", {%.*s}", tinfo->keyno, (int)tinfo->new_value->size,
      (char *)tinfo->new_value->data);

    return (0);
}

/*
 * row_truncate --
 *     Truncate rows in a row-store file.
 */
static int
row_truncate(TINFO *tinfo)
{
    WT_CURSOR *cursor, *c2;
    WT_DECL_RET;
    WT_SESSION *session;

    cursor = tinfo->cursor;
    session = cursor->session;

    /* The code assumes we're never truncating the entire object, assert that fact. */
    testutil_assert(tinfo->keyno != 0 || tinfo->last != 0);

    trace_op(tinfo, "truncate %" PRIu64 "-%" PRIu64 " start", tinfo->keyno, tinfo->last);
    if (tinfo->keyno == 0) {
        key_gen(tinfo->table, tinfo->key, tinfo->last);
        cursor->set_key(cursor, tinfo->key);
        WT_ERR(session->truncate(session, NULL, NULL, cursor, NULL));
    } else if (tinfo->last == 0) {
        key_gen(tinfo->table, tinfo->key, tinfo->keyno);
        cursor->set_key(cursor, tinfo->key);
        WT_ERR(session->truncate(session, NULL, cursor, NULL, NULL));
    } else {
        key_gen(tinfo->table, tinfo->key, tinfo->keyno);
        cursor->set_key(cursor, tinfo->key);

        testutil_check(session->open_cursor(session, tinfo->table->uri, NULL, NULL, &c2));
        key_gen(tinfo->table, tinfo->lastkey, tinfo->last);
        cursor->set_key(c2, tinfo->lastkey);

        ret = session->truncate(session, NULL, cursor, c2, NULL);
        testutil_check(c2->close(c2));
        WT_ERR(ret);
    }

err:
    trace_op(tinfo, "truncate %" PRIu64 "-%" PRIu64 " stop ret %d", tinfo->keyno, tinfo->last, ret);

    return (ret);
}

/*
 * col_truncate --
 *     Truncate rows in a column-store file.
 */
static int
col_truncate(TINFO *tinfo)
{
    WT_CURSOR *cursor, *c2;
    WT_DECL_RET;
    WT_SESSION *session;

    cursor = tinfo->cursor;
    session = cursor->session;

    /* The code assumes we're never truncating the entire object, assert that fact. */
    testutil_assert(tinfo->keyno != 0 || tinfo->last != 0);

    if (tinfo->keyno == 0) {
        cursor->set_key(cursor, tinfo->last);
        WT_RET(session->truncate(session, NULL, NULL, cursor, NULL));
    } else if (tinfo->last == 0) {
        cursor->set_key(cursor, tinfo->keyno);
        WT_RET(session->truncate(session, NULL, cursor, NULL, NULL));
    } else {
        cursor->set_key(cursor, tinfo->keyno);

        testutil_check(session->open_cursor(session, tinfo->table->uri, NULL, NULL, &c2));
        cursor->set_key(c2, tinfo->last);

        ret = session->truncate(session, NULL, cursor, c2, NULL);
        testutil_check(c2->close(c2));
        WT_RET(ret);
    }

    trace_op(tinfo, "truncate %" PRIu64 "-%" PRIu64, tinfo->keyno, tinfo->last);

    return (0);
}

/*
 * row_update --
 *     Update a row in a row-store file.
 */
static int
row_update(TINFO *tinfo, bool positioned)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;

    cursor = tinfo->cursor;

    if (!positioned) {
        key_gen(tinfo->table, tinfo->key, tinfo->keyno);
        cursor->set_key(cursor, tinfo->key);
    }
    cursor->set_value(cursor, tinfo->new_value);

    if ((ret = cursor->update(cursor)) != 0)
        return (ret);

    trace_op(tinfo, "update %" PRIu64 " {%.*s}, {%.*s}", tinfo->keyno, (int)tinfo->key->size,
      (char *)tinfo->key->data, (int)tinfo->new_value->size, (char *)tinfo->new_value->data);

    return (0);
}

/*
 * col_update --
 *     Update a row in a column-store file.
 */
static int
col_update(TINFO *tinfo, bool positioned)
{
    TABLE *table;
    WT_CURSOR *cursor;
    WT_DECL_RET;

    table = tinfo->table;
    cursor = tinfo->cursor;

    if (!positioned)
        cursor->set_key(cursor, tinfo->keyno);
    if (table->type == FIX) {
        /* Mirrors will not have set the FLCS value. */
        if (table->mirror)
            val_to_flcs(table, tinfo->new_value, &tinfo->bitv);
        cursor->set_value(cursor, tinfo->bitv);
    } else
        cursor->set_value(cursor, tinfo->new_value);

    if ((ret = cursor->update(cursor)) != 0)
        return (ret);

    if (table->type == FIX)
        trace_op(tinfo, "update %" PRIu64 " {0x%02" PRIx8 "}", tinfo->keyno, tinfo->bitv);
    else
        trace_op(tinfo, "update %" PRIu64 " {%.*s}", tinfo->keyno, (int)tinfo->new_value->size,
          (char *)tinfo->new_value->data);

    return (0);
}

/*
 * row_insert --
 *     Insert a row in a row-store file.
 */
static int
row_insert(TINFO *tinfo, bool positioned)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;

    cursor = tinfo->cursor;

    /*
     * If we positioned the cursor already, it's a test of an update using the insert method.
     * Otherwise, generate a unique key and insert (or update an already inserted record).
     */
    if (!positioned) {
        key_gen_insert(tinfo->table, &tinfo->data_rnd, tinfo->key, tinfo->keyno);
        cursor->set_key(cursor, tinfo->key);
    }
    cursor->set_value(cursor, tinfo->new_value);

    if ((ret = cursor->insert(cursor)) != 0)
        return (ret);

    /* Log the operation */
    trace_op(tinfo, "insert %" PRIu64 " {%.*s}, {%.*s}", tinfo->keyno, (int)tinfo->key->size,
      (char *)tinfo->key->data, (int)tinfo->new_value->size, (char *)tinfo->new_value->data);

    return (0);
}

/*
 * col_insert_resolve --
 *     Resolve newly inserted records.
 */
static void
col_insert_resolve(TABLE *table, void *arg)
{
    struct col_insert *cip;
    TINFO *tinfo;
    uint32_t max_rows, *p;
    u_int i;

    tinfo = arg;
    testutil_assert(table != NULL);

    cip = &tinfo->col_insert[table->id - 1];
    if (cip->insert_list_cnt == 0)
        return;

    /*
     * We don't want to ignore column-store records we insert, which requires we update the "last
     * row" so other threads consider them. Threads allocating record numbers can race with other
     * threads, so the thread allocating record N may return after the thread allocating N + 1. We
     * can't update a record before it's been inserted, and so we can't leave gaps when the count of
     * records in the table is incremented.
     *
     * The solution is a per-table array which contains an unsorted list of inserted records. If
     * there are pending inserts, review the table and try to update the total rows. This is
     * wasteful, but we want to give other threads immediate access to the row, ideally they'll
     * collide with our insert before we resolve.
     *
     * Process the existing records and advance the last row count until we can't go further.
     */
    do {
        WT_ORDERED_READ(max_rows, table->rows_current);
        for (i = 0, p = cip->insert_list; i < WT_ELEMENTS(cip->insert_list); ++i, ++p) {
            /*
             * A thread may have allocated a record number that is now less than or equal to the
             * current maximum number of rows. In this case, simply reset the insert list.
             * Otherwise, update the maximum number of rows with the newly inserted record.
             */
            if (*p > 0 && *p <= max_rows + 1) {
                if (*p == max_rows + 1)
                    testutil_assert(
                      __wt_atomic_casv32(&table->rows_current, max_rows, max_rows + 1));
                *p = 0;
                --cip->insert_list_cnt;
                break;
            }
            testutil_assert(*p == 0 || *p > max_rows);
        }
    } while (cip->insert_list_cnt > 0 && i < WT_ELEMENTS(cip->insert_list));
}

/*
 * col_insert_add --
 *     Add newly inserted records.
 */
static void
col_insert_add(TINFO *tinfo)
{
    struct col_insert *cip;
    u_int i;

    /* Add the inserted record to the insert array. */
    cip = &tinfo->col_insert[tinfo->table->id - 1];
    for (i = 0; i < WT_ELEMENTS(cip->insert_list); ++i)
        if (cip->insert_list[i] == 0) {
            cip->insert_list[i] = (uint32_t)tinfo->keyno;
            ++cip->insert_list_cnt;
            break;
        }
    testutil_assert(i < WT_ELEMENTS(cip->insert_list));
}

/*
 * col_insert --
 *     Insert an element in a column-store file.
 */
static int
col_insert(TINFO *tinfo)
{
    struct col_insert *cip;
    TABLE *table;
    WT_CURSOR *cursor;
    WT_DECL_RET;

    table = tinfo->table;
    cursor = tinfo->cursor;

    /*
     * We can only append so many new records, check for the limit, and if we reach it, skip the
     * operation until some records drain.
     */
    cip = &tinfo->col_insert[table->id - 1];
    if (cip->insert_list_cnt >= WT_ELEMENTS(cip->insert_list))
        return (WT_ROLLBACK);

    if (table->type == FIX) {
        /* Mirrors will not have set the FLCS value. */
        if (table->mirror)
            val_to_flcs(table, tinfo->new_value, &tinfo->bitv);
        cursor->set_value(cursor, tinfo->bitv);
    } else
        cursor->set_value(cursor, tinfo->new_value);

    /* Create a record, then add the key to our list of new records for later resolution. */
    if ((ret = cursor->insert(cursor)) != 0)
        return (ret);

    testutil_check(cursor->get_key(cursor, &tinfo->keyno));

    col_insert_add(tinfo); /* Extend the object. */

    if (table->type == FIX)
        trace_op(tinfo, "insert %" PRIu64 " {0x%02" PRIx8 "}", tinfo->keyno, tinfo->bitv);
    else
        trace_op(tinfo, "insert %" PRIu64 " {%.*s}", tinfo->keyno, (int)tinfo->new_value->size,
          (char *)tinfo->new_value->data);

    return (0);
}

/*
 * row_remove --
 *     Remove an row from a row-store file.
 */
static int
row_remove(TINFO *tinfo, bool positioned)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;

    cursor = tinfo->cursor;

    if (!positioned) {
        key_gen(tinfo->table, tinfo->key, tinfo->keyno);
        cursor->set_key(cursor, tinfo->key);
    }

    /* We use the cursor in overwrite mode, check for existence. */
    if ((ret = read_op(cursor, SEARCH, NULL)) == 0)
        ret = cursor->remove(cursor);

    if (ret != 0 && ret != WT_NOTFOUND)
        return (ret);

    trace_op(tinfo, "remove %" PRIu64 " {%.*s}", tinfo->keyno, (int)tinfo->key->size,
      (char *)tinfo->key->data);

    return (ret);
}

/*
 * col_remove --
 *     Remove a row from a column-store file.
 */
static int
col_remove(TINFO *tinfo, bool positioned)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;

    cursor = tinfo->cursor;

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
