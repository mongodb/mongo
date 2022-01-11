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

#include "test_util.h"

#include <sys/wait.h>
#include <signal.h>

static char home[1024]; /* Program working dir */

/*
 * Create three tables that we will write the same data to and verify that
 * all the types of usage have the expected data in them after a crash and
 * recovery.  We want:
 * 1. A table that is logged and is not involved in timestamps.  This table
 * simulates a user local table.
 * 2. A table that is logged and involved in timestamps.  This simulates
 * the oplog.
 * 3. A table that is not logged and involved in timestamps.  This simulates
 * a typical collection file.
 *
 * We also have most threads perform schema operations such as create/drop.
 *
 * We also create several files that are not WiredTiger tables.  The checkpoint
 * thread creates a file indicating that a checkpoint has completed.  The parent
 * process uses this to know when at least one checkpoint is done and it can
 * start the timer to abort.
 *
 * Each worker thread creates its own records file that records the data it
 * inserted and it records the timestamp that was used for that insertion.
 */
#define INVALID_KEY UINT64_MAX
#define MAX_CKPT_INVL 2 /* Maximum interval between checkpoints */
/* Set large, some slow I/O systems take tens of seconds to fsync. */
#define MAX_STARTUP 30 /* Seconds to start up and set stable */
#define MAX_TH 12
#define MAX_TIME 40
#define MAX_VAL 1024
#define MIN_TH 5
#define MIN_TIME 10
#define PREPARE_FREQ 5
#define PREPARE_YIELD (PREPARE_FREQ * 10)
#define RECORDS_FILE "records-%" PRIu32
#define STABLE_PERIOD 100

static const char *const uri = "table:wt";
static const char *const uri_local = "table:local";
static const char *const uri_oplog = "table:oplog";
static const char *const uri_collection = "table:collection";

static const char *const ckpt_file = "checkpoint_done";

static bool compat, inmem, use_columns, use_ts, use_txn;
static volatile bool stable_set;
static volatile uint64_t global_ts = 1;
static volatile uint64_t uid = 1;
typedef struct {
    uint64_t ts;
    const char *op;
} THREAD_TS;
static volatile THREAD_TS th_ts[MAX_TH];

#define ENV_CONFIG_COMPAT ",compatibility=(release=\"2.9\")"
#define ENV_CONFIG_DEF                                        \
    "create,"                                                 \
    "eviction_updates_trigger=95,eviction_updates_target=80," \
    "log=(archive=false,file_max=10M,enabled)"
#define ENV_CONFIG_TXNSYNC \
    ENV_CONFIG_DEF         \
    ",transaction_sync=(enabled,method=none)"
#define ENV_CONFIG_REC "log=(archive=false,recover=on)"

/*
 * A minimum width of 10, along with zero filling, means that all the keys sort according to their
 * integer value, making each thread's key space distinct. For column-store we just use the integer
 * values and that has the same effect.
 */
#define ROW_KEY_FORMAT ("%010" PRIu64)

typedef struct {
    uint64_t absent_key; /* Last absent key */
    uint64_t exist_key;  /* First existing key after miss */
    uint64_t first_key;  /* First key in range */
    uint64_t first_miss; /* First missing key */
    uint64_t last_key;   /* Last key in range */
} REPORT;

typedef struct {
    WT_CONNECTION *conn;
    uint64_t start;
    uint32_t info;
    const char *op;
} THREAD_DATA;

#define NOOP "noop"
#define BULK "bulk"
#define BULK_UNQ "bulk_unique"
#define CREATE "create"
#define CREATE_UNQ "create_unique"
#define CURSOR "cursor"
#define DROP "drop"
#define UPGRADE "upgrade"
#define VERIFY "verify"

static void sig_handler(int) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
static void usage(void) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
/*
 * usage --
 *     TODO: Add a comment describing this function.
 */
static void
usage(void)
{
    fprintf(stderr, "usage: %s [-h dir] [-T threads] [-t time] [-Cmvxz]\n", progname);
    exit(EXIT_FAILURE);
}

static const char *const config = NULL;

/*
 * subtest_error_handler --
 *     Error event handler.
 */
static int
subtest_error_handler(
  WT_EVENT_HANDLER *handler, WT_SESSION *session, int error, const char *message)
{
    (void)(handler);
    (void)(session);
    (void)(error);

    /* Filter out errors about bulk load usage - they are annoying */
    if (strstr(message, "bulk-load is only supported on newly") == NULL)
        fprintf(stderr, "%s", message);
    return (0);
}

static WT_EVENT_HANDLER event_handler = {
  subtest_error_handler, NULL, /* Message handler */
  NULL,                        /* Progress handler */
  NULL                         /* Close handler */
};

/*
 * The following are various schema-related functions to have some threads performing during the
 * test. The goal is to make sure that after a random abort, the database is left in a recoverable
 * state. Yield during the schema operations to increase chance of abort during them.
 *
 * TODO: Currently only verifies insert data, it would be ideal to modify the schema operations so
 * that we can verify the state of the schema too.
 */

/*
 * dump_ts --
 *     TODO: Add a comment describing this function.
 */
static void
dump_ts(uint64_t nth)
{
    uint64_t i;

    for (i = 0; i < nth; ++i)
        fprintf(stderr, "THREAD %" PRIu64 ": ts: %" PRIu64 " op %s\n", i, th_ts[i].ts, th_ts[i].op);
}

/*
 * test_bulk --
 *     Test creating a bulk cursor.
 */
static void
test_bulk(THREAD_DATA *td)
{
    WT_CURSOR *c;
    WT_DECL_RET;
    WT_SESSION *session;
    bool create;

    testutil_check(td->conn->open_session(td->conn, NULL, NULL, &session));

    if (use_txn)
        testutil_check(session->begin_transaction(session, NULL));
    create = false;
    if ((ret = session->create(session, uri, config)) != 0)
        if (ret != EEXIST && ret != EBUSY)
            testutil_die(ret, "session.create");

    if (ret == 0) {
        create = true;
        if ((ret = session->open_cursor(session, uri, NULL, "bulk", &c)) == 0) {
            __wt_yield();
            testutil_check(c->close(c));
        } else if (ret != ENOENT && ret != EBUSY && ret != EINVAL)
            testutil_die(ret, "session.open_cursor bulk");
    }

    if (use_txn) {
        /* If create fails, rollback else will commit.*/
        if (!create)
            ret = session->rollback_transaction(session, NULL);
        else
            ret = session->commit_transaction(session, NULL);

        if (ret == EINVAL) {
            fprintf(stderr, "BULK: EINVAL on %s. ABORT\n", create ? "commit" : "rollback");
            testutil_die(ret, "session.commit bulk");
        }
    }
    testutil_check(session->close(session, NULL));
}

/*
 * test_bulk_unique --
 *     Test creating a bulk cursor with a unique name.
 */
static void
test_bulk_unique(THREAD_DATA *td, int force)
{
    WT_CURSOR *c;
    WT_DECL_RET;
    WT_SESSION *session;
    uint64_t my_uid;
    char new_uri[64];

    testutil_check(td->conn->open_session(td->conn, NULL, NULL, &session));

    /* Generate a unique object name. */
    my_uid = __wt_atomic_addv64(&uid, 1);
    testutil_check(__wt_snprintf(new_uri, sizeof(new_uri), "%s.%" PRIu64, uri, my_uid));

    if (use_txn)
        testutil_check(session->begin_transaction(session, NULL));
    testutil_check(session->create(session, new_uri, config));

    __wt_yield();
    /*
     * Opening a bulk cursor may have raced with a forced checkpoint which created a checkpoint of
     * the empty file, and triggers an EINVAL.
     */
    if ((ret = session->open_cursor(session, new_uri, NULL, "bulk", &c)) == 0)
        testutil_check(c->close(c));
    else if (ret != EINVAL)
        testutil_die(ret, "session.open_cursor bulk unique: %s, new_uri");

    while ((ret = session->drop(session, new_uri, force ? "force" : NULL)) != 0)
        if (ret != EBUSY)
            testutil_die(ret, "session.drop: %s", new_uri);

    if (use_txn && (ret = session->commit_transaction(session, NULL)) != 0 && ret != EINVAL)
        testutil_die(ret, "session.commit bulk unique");
    testutil_check(session->close(session, NULL));
}

/*
 * test_cursor --
 *     Open a cursor on a data source.
 */
static void
test_cursor(THREAD_DATA *td)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_SESSION *session;

    testutil_check(td->conn->open_session(td->conn, NULL, NULL, &session));

    if (use_txn)
        testutil_check(session->begin_transaction(session, NULL));
    if ((ret = session->open_cursor(session, uri, NULL, NULL, &cursor)) != 0) {
        if (ret != ENOENT && ret != EBUSY)
            testutil_die(ret, "session.open_cursor");
    } else {
        __wt_yield();
        testutil_check(cursor->close(cursor));
    }

    if (use_txn && (ret = session->commit_transaction(session, NULL)) != 0 && ret != EINVAL)
        testutil_die(ret, "session.commit cursor");
    testutil_check(session->close(session, NULL));
}

/*
 * test_create --
 *     Create a table.
 */
static void
test_create(THREAD_DATA *td)
{
    WT_DECL_RET;
    WT_SESSION *session;

    testutil_check(td->conn->open_session(td->conn, NULL, NULL, &session));

    if (use_txn)
        testutil_check(session->begin_transaction(session, NULL));
    if ((ret = session->create(session, uri, config)) != 0)
        if (ret != EEXIST && ret != EBUSY)
            testutil_die(ret, "session.create");
    __wt_yield();
    if (use_txn && (ret = session->commit_transaction(session, NULL)) != 0 && ret != EINVAL)
        testutil_die(ret, "session.commit create");
    testutil_check(session->close(session, NULL));
}

/*
 * test_create_unique --
 *     Create a uniquely named table.
 */
static void
test_create_unique(THREAD_DATA *td, int force)
{
    WT_DECL_RET;
    WT_SESSION *session;
    uint64_t my_uid;
    char new_uri[64];

    testutil_check(td->conn->open_session(td->conn, NULL, NULL, &session));

    /* Generate a unique object name. */
    my_uid = __wt_atomic_addv64(&uid, 1);
    testutil_check(__wt_snprintf(new_uri, sizeof(new_uri), "%s.%" PRIu64, uri, my_uid));

    if (use_txn)
        testutil_check(session->begin_transaction(session, NULL));
    testutil_check(session->create(session, new_uri, config));
    if (use_txn && (ret = session->commit_transaction(session, NULL)) != 0 && ret != EINVAL)
        testutil_die(ret, "session.commit create unique");

    __wt_yield();
    if (use_txn)
        testutil_check(session->begin_transaction(session, NULL));
    while ((ret = session->drop(session, new_uri, force ? "force" : NULL)) != 0)
        if (ret != EBUSY)
            testutil_die(ret, "session.drop: %s", new_uri);
    if (use_txn && (ret = session->commit_transaction(session, NULL)) != 0 && ret != EINVAL)
        testutil_die(ret, "session.commit create unique");

    testutil_check(session->close(session, NULL));
}

/*
 * test_drop --
 *     Test dropping a table.
 */
static void
test_drop(THREAD_DATA *td, int force)
{
    WT_DECL_RET;
    WT_SESSION *session;

    testutil_check(td->conn->open_session(td->conn, NULL, NULL, &session));

    if (use_txn)
        testutil_check(session->begin_transaction(session, NULL));
    if ((ret = session->drop(session, uri, force ? "force" : NULL)) != 0)
        if (ret != ENOENT && ret != EBUSY)
            testutil_die(ret, "session.drop");

    if (use_txn) {
        /*
         * As the operations are being performed concurrently, return value can be ENOENT or EBUSY
         * will set error to transaction opened by session. In these cases the transaction has to be
         * aborted.
         */
        if (ret != ENOENT && ret != EBUSY)
            ret = session->commit_transaction(session, NULL);
        else
            ret = session->rollback_transaction(session, NULL);
        if (ret == EINVAL)
            testutil_die(ret, "session.commit drop");
    }
    testutil_check(session->close(session, NULL));
}

/*
 * test_upgrade --
 *     Upgrade a tree.
 */
static void
test_upgrade(THREAD_DATA *td)
{
    WT_DECL_RET;
    WT_SESSION *session;

    testutil_check(td->conn->open_session(td->conn, NULL, NULL, &session));

    if ((ret = session->upgrade(session, uri, NULL)) != 0)
        if (ret != ENOENT && ret != EBUSY)
            testutil_die(ret, "session.upgrade");

    testutil_check(session->close(session, NULL));
}

/*
 * test_verify --
 *     Verify a tree.
 */
static void
test_verify(THREAD_DATA *td)
{
    WT_DECL_RET;
    WT_SESSION *session;

    testutil_check(td->conn->open_session(td->conn, NULL, NULL, &session));

    if ((ret = session->verify(session, uri, NULL)) != 0)
        if (ret != ENOENT && ret != EBUSY)
            testutil_die(ret, "session.verify");

    testutil_check(session->close(session, NULL));
}

/*
 * thread_ts_run --
 *     Runner function for a timestamp thread.
 */
static WT_THREAD_RET
thread_ts_run(void *arg)
{
    WT_SESSION *session;
    THREAD_DATA *td;
    uint64_t i, last_ts, oldest_ts, this_ts;
    char tscfg[64];

    td = (THREAD_DATA *)arg;
    last_ts = 0;

    testutil_check(td->conn->open_session(td->conn, NULL, NULL, &session));
    /*
     * Every N records we will record our stable timestamp into the stable table. That will define
     * our threshold where we expect to find records after recovery.
     */
    for (;;) {
        oldest_ts = UINT64_MAX;
        /*
         * For the timestamp thread, the info field contains the number of worker threads.
         */
        for (i = 0; i < td->info; ++i) {
            /*
             * We need to let all threads get started, so if we find any thread still with a zero
             * timestamp we go to sleep.
             */
            this_ts = th_ts[i].ts;
            if (this_ts == 0)
                goto ts_wait;
            else if (this_ts < oldest_ts)
                oldest_ts = this_ts;
        }

        if (oldest_ts != UINT64_MAX && oldest_ts - last_ts > STABLE_PERIOD) {
            /*
             * Set both the oldest and stable timestamp so that we don't need to maintain read
             * availability at older timestamps.
             */
            testutil_check(__wt_snprintf(tscfg, sizeof(tscfg),
              "oldest_timestamp=%" PRIx64 ",stable_timestamp=%" PRIx64, oldest_ts, oldest_ts));
            testutil_check(td->conn->set_timestamp(td->conn, tscfg));
            last_ts = oldest_ts;
            if (!stable_set) {
                stable_set = true;
                printf("SET STABLE: %" PRIx64 " %" PRIu64 "\n", oldest_ts, oldest_ts);
            }
        } else
ts_wait:
            __wt_sleep(0, 1000);
    }
    /* NOTREACHED */
}

/*
 * thread_ckpt_run --
 *     Runner function for the checkpoint thread.
 */
static WT_THREAD_RET
thread_ckpt_run(void *arg)
{
    struct timespec now, start;
    FILE *fp;
    WT_RAND_STATE rnd;
    WT_SESSION *session;
    THREAD_DATA *td;
    uint64_t ts;
    uint32_t sleep_time;
    int i;
    bool first_ckpt;

    __wt_random_init(&rnd);

    td = (THREAD_DATA *)arg;
    /*
     * Keep a separate file with the records we wrote for checking.
     */
    (void)unlink(ckpt_file);
    testutil_check(td->conn->open_session(td->conn, NULL, NULL, &session));
    first_ckpt = true;
    ts = 0;
    /*
     * Keep writing checkpoints until killed by parent.
     */
    __wt_epoch(NULL, &start);
    for (i = 0;;) {
        sleep_time = __wt_random(&rnd) % MAX_CKPT_INVL;
        sleep(sleep_time);
        if (use_ts) {
            ts = global_ts;
            /*
             * If we're using timestamps wait for the stable timestamp to get set the first time.
             */
            if (!stable_set) {
                __wt_epoch(NULL, &now);
                if (WT_TIMEDIFF_SEC(now, start) >= 1)
                    printf("CKPT: !stable_set time %" PRIu64 "\n", WT_TIMEDIFF_SEC(now, start));
                if (WT_TIMEDIFF_SEC(now, start) > MAX_STARTUP) {
                    fprintf(
                      stderr, "After %d seconds stable still not set. Aborting.\n", MAX_STARTUP);
                    /*
                     * For the checkpoint thread the info contains the number of threads.
                     */
                    dump_ts(td->info);
                    abort();
                }
                continue;
            }
        }
        /*
         * Since this is the default, send in this string even if running without timestamps.
         */
        testutil_check(session->checkpoint(session, "use_timestamp=true"));
        printf("Checkpoint %d complete.  Minimum ts %" PRIu64 "\n", ++i, ts);
        fflush(stdout);
        /*
         * Create the checkpoint file so that the parent process knows at least one checkpoint has
         * finished and can start its timer. Start the timer for stable after the first checkpoint
         * completes because a slow I/O lag during the checkpoint can cause a false positive for a
         * timeout.
         */
        if (first_ckpt) {
            testutil_assert_errno((fp = fopen(ckpt_file, "w")) != NULL);
            first_ckpt = false;
            testutil_assert_errno(fclose(fp) == 0);
        }
    }
    /* NOTREACHED */
}

/*
 * thread_run --
 *     Runner function for the worker threads.
 */
static WT_THREAD_RET
thread_run(void *arg)
{
    FILE *fp;
    WT_CURSOR *cur_coll, *cur_local, *cur_oplog;
    WT_ITEM data;
    WT_RAND_STATE rnd;
    WT_SESSION *oplog_session, *session;
    THREAD_DATA *td;
    uint64_t i, stable_ts;
    char cbuf[MAX_VAL], lbuf[MAX_VAL], obuf[MAX_VAL];
    char kname[64], tscfg[64];
    bool use_prep;

    __wt_random_init(&rnd);
    memset(cbuf, 0, sizeof(cbuf));
    memset(lbuf, 0, sizeof(lbuf));
    memset(obuf, 0, sizeof(obuf));
    memset(kname, 0, sizeof(kname));

    td = (THREAD_DATA *)arg;
    /*
     * Set up the separate file for checking.
     */
    testutil_check(__wt_snprintf(cbuf, sizeof(cbuf), RECORDS_FILE, td->info));
    (void)unlink(cbuf);
    testutil_assert_errno((fp = fopen(cbuf, "w")) != NULL);
    /*
     * Set to line buffering. But that is advisory only. We've seen cases where the result files end
     * up with partial lines.
     */
    __wt_stream_set_line_buffer(fp);

    /*
     * Have half the threads use prepared transactions if timestamps are in use.
     */
    use_prep = (use_ts && td->info % 2 == 0) ? true : false;
    /*
     * We may have two sessions so that the oplog session can have its own transaction in parallel
     * with the collection session for threads that are going to be using prepared transactions. We
     * need this because prepared transactions cannot have any operations that modify a table that
     * is logged. But we also want to test mixed logged and not-logged transactions.
     */
    testutil_check(td->conn->open_session(td->conn, NULL, "isolation=snapshot", &session));
    /*
     * Open a cursor to each table.
     */
    testutil_check(session->open_cursor(session, uri_collection, NULL, NULL, &cur_coll));
    testutil_check(session->open_cursor(session, uri_local, NULL, NULL, &cur_local));
    oplog_session = NULL;
    if (use_prep) {
        testutil_check(
          td->conn->open_session(td->conn, NULL, "isolation=snapshot", &oplog_session));
        testutil_check(session->open_cursor(oplog_session, uri_oplog, NULL, NULL, &cur_oplog));
    } else
        testutil_check(session->open_cursor(session, uri_oplog, NULL, NULL, &cur_oplog));

    /*
     * Write our portion of the key space until we're killed.
     */
    printf("Thread %" PRIu32 " starts at %" PRIu64 "\n", td->info, td->start);
    stable_ts = 0;
    for (i = td->start;; ++i) {
        /*
         * Allow some threads to skip schema operations so that they are generating sufficient dirty
         * data.
         */
        WT_PUBLISH(th_ts[td->info].op, NOOP);
        if (td->info != 0 && td->info != 1)
            /*
             * Do a schema operation about 50% of the time by having a case for only about half the
             * possible mod values.
             */
            switch (__wt_random(&rnd) % 20) {
            case 0:
                WT_PUBLISH(th_ts[td->info].op, BULK);
                test_bulk(td);
                break;
            case 1:
                WT_PUBLISH(th_ts[td->info].op, BULK_UNQ);
                test_bulk_unique(td, __wt_random(&rnd) & 1);
                break;
            case 2:
                WT_PUBLISH(th_ts[td->info].op, CREATE);
                test_create(td);
                break;
            case 3:
                WT_PUBLISH(th_ts[td->info].op, CREATE_UNQ);
                test_create_unique(td, __wt_random(&rnd) & 1);
                break;
            case 4:
                WT_PUBLISH(th_ts[td->info].op, CURSOR);
                test_cursor(td);
                break;
            case 5:
                WT_PUBLISH(th_ts[td->info].op, DROP);
                test_drop(td, __wt_random(&rnd) & 1);
                break;
            case 6:
                WT_PUBLISH(th_ts[td->info].op, UPGRADE);
                test_upgrade(td);
                break;
            case 7:
                WT_PUBLISH(th_ts[td->info].op, VERIFY);
                test_verify(td);
                break;
            }
        if (use_ts)
            stable_ts = __wt_atomic_addv64(&global_ts, 1);

        testutil_check(session->begin_transaction(session, NULL));
        if (use_prep)
            testutil_check(oplog_session->begin_transaction(oplog_session, NULL));
        if (use_columns) {
            cur_coll->set_key(cur_coll, i + 1);
            cur_local->set_key(cur_local, i + 1);
            cur_oplog->set_key(cur_oplog, i + 1);
        } else {
            testutil_check(__wt_snprintf(kname, sizeof(kname), ROW_KEY_FORMAT, i));
            cur_coll->set_key(cur_coll, kname);
            cur_local->set_key(cur_local, kname);
            cur_oplog->set_key(cur_oplog, kname);
        }
        /*
         * Put an informative string into the value so that it can be viewed well in a binary dump.
         */
        testutil_check(__wt_snprintf(cbuf, sizeof(cbuf),
          "COLL: thread:%" PRIu32 " ts:%" PRIu64 " key: %" PRIu64, td->info, stable_ts, i));
        testutil_check(__wt_snprintf(lbuf, sizeof(lbuf),
          "LOCAL: thread:%" PRIu32 " ts:%" PRIu64 " key: %" PRIu64, td->info, stable_ts, i));
        testutil_check(__wt_snprintf(obuf, sizeof(obuf),
          "OPLOG: thread:%" PRIu32 " ts:%" PRIu64 " key: %" PRIu64, td->info, stable_ts, i));
        data.size = __wt_random(&rnd) % MAX_VAL;
        data.data = cbuf;
        cur_coll->set_value(cur_coll, &data);
        testutil_check(cur_coll->insert(cur_coll));
        data.size = __wt_random(&rnd) % MAX_VAL;
        data.data = obuf;
        cur_oplog->set_value(cur_oplog, &data);
        testutil_check(cur_oplog->insert(cur_oplog));
        if (use_ts) {
            /*
             * Run with prepare every once in a while. And also yield after prepare sometimes too.
             * This is only done on the regular session.
             */
            if (use_prep && i % PREPARE_FREQ == 0) {
                testutil_check(
                  __wt_snprintf(tscfg, sizeof(tscfg), "prepare_timestamp=%" PRIx64, stable_ts));
                testutil_check(session->prepare_transaction(session, tscfg));
                if (i % PREPARE_YIELD == 0)
                    __wt_yield();

                testutil_check(__wt_snprintf(tscfg, sizeof(tscfg),
                  "commit_timestamp=%" PRIx64 ",durable_timestamp=%" PRIx64, stable_ts, stable_ts));
            } else
                testutil_check(
                  __wt_snprintf(tscfg, sizeof(tscfg), "commit_timestamp=%" PRIx64, stable_ts));

            testutil_check(session->commit_transaction(session, tscfg));
            if (use_prep) {
                /*
                 * Durable timestamp should not be passed as oplog transaction is a non-prepared
                 * transaction.
                 */
                testutil_check(
                  __wt_snprintf(tscfg, sizeof(tscfg), "commit_timestamp=%" PRIx64, stable_ts));
                testutil_check(oplog_session->commit_transaction(oplog_session, tscfg));
            }
            /*
             * Update the thread's last-committed timestamp. Don't let the compiler re-order this
             * statement, if we were to race with the timestamp thread, it might see our thread
             * update before the commit.
             */
            WT_PUBLISH(th_ts[td->info].ts, stable_ts);
        } else {
            testutil_check(session->commit_transaction(session, NULL));
            if (use_prep)
                testutil_check(oplog_session->commit_transaction(oplog_session, NULL));
        }
        /*
         * Insert into the local table outside the timestamp txn.
         */
        data.size = __wt_random(&rnd) % MAX_VAL;
        data.data = lbuf;
        cur_local->set_value(cur_local, &data);
        testutil_check(cur_local->insert(cur_local));

        /*
         * Save the timestamp and key separately for checking later.
         */
        if (fprintf(fp, "%" PRIu64 " %" PRIu64 "\n", stable_ts, i) < 0)
            testutil_die(EIO, "fprintf");
    }
    /* NOTREACHED */
}

/*
 * Child process creates the database and table, and then creates worker threads to add data until
 * it is killed by the parent.
 */
static void run_workload(uint32_t) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
/*
 * run_workload --
 *     TODO: Add a comment describing this function.
 */
static void
run_workload(uint32_t nth)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    THREAD_DATA *td;
    wt_thread_t *thr;
    uint32_t ckpt_id, i, ts_id;
    char envconf[512], tableconf[128];

    thr = dcalloc(nth + 2, sizeof(*thr));
    td = dcalloc(nth + 2, sizeof(THREAD_DATA));
    stable_set = false;
    if (chdir(home) != 0)
        testutil_die(errno, "Child chdir: %s", home);
    if (inmem)
        strcpy(envconf, ENV_CONFIG_DEF);
    else
        strcpy(envconf, ENV_CONFIG_TXNSYNC);
    if (compat)
        strcat(envconf, ENV_CONFIG_COMPAT);

    testutil_check(wiredtiger_open(NULL, &event_handler, envconf, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    /*
     * Create all the tables.
     */
    testutil_check(__wt_snprintf(tableconf, sizeof(tableconf),
      "key_format=%s,value_format=u,log=(enabled=false)", use_columns ? "r" : "S"));
    testutil_check(session->create(session, uri_collection, tableconf));
    testutil_check(__wt_snprintf(
      tableconf, sizeof(tableconf), "key_format=%s,value_format=u", use_columns ? "r" : "S"));
    testutil_check(session->create(session, uri_local, tableconf));
    testutil_check(session->create(session, uri_oplog, tableconf));
    /*
     * Don't log the stable timestamp table so that we know what timestamp was stored at the
     * checkpoint.
     */
    testutil_check(session->close(session, NULL));

    /*
     * The checkpoint thread and the timestamp threads are added at the end.
     */
    ckpt_id = nth;
    td[ckpt_id].conn = conn;
    td[ckpt_id].info = nth;
    printf("Create checkpoint thread\n");
    testutil_check(__wt_thread_create(NULL, &thr[ckpt_id], thread_ckpt_run, &td[ckpt_id]));
    ts_id = nth + 1;
    if (use_ts) {
        td[ts_id].conn = conn;
        td[ts_id].info = nth;
        printf("Create timestamp thread\n");
        testutil_check(__wt_thread_create(NULL, &thr[ts_id], thread_ts_run, &td[ts_id]));
    }
    printf("Create %" PRIu32 " writer threads\n", nth);
    for (i = 0; i < nth; ++i) {
        td[i].conn = conn;
        td[i].start = WT_BILLION * (uint64_t)i;
        td[i].info = i;
        testutil_check(__wt_thread_create(NULL, &thr[i], thread_run, &td[i]));
    }
    /*
     * The threads never exit, so the child will just wait here until it is killed.
     */
    fflush(stdout);
    for (i = 0; i <= ts_id; ++i)
        testutil_check(__wt_thread_join(NULL, &thr[i]));
    /*
     * NOTREACHED
     */
    free(thr);
    free(td);
    _exit(EXIT_SUCCESS);
}

extern int __wt_optind;
extern char *__wt_optarg;

/*
 * Initialize a report structure. Since zero is a valid key we cannot just clear it.
 */
/*
 * initialize_rep --
 *     TODO: Add a comment describing this function.
 */
static void
initialize_rep(REPORT *r)
{
    r->first_key = r->first_miss = INVALID_KEY;
    r->absent_key = r->exist_key = r->last_key = INVALID_KEY;
}

/*
 * Print out information if we detect missing records in the middle of the data of a report
 * structure.
 */
/*
 * print_missing --
 *     TODO: Add a comment describing this function.
 */
static void
print_missing(REPORT *r, const char *fname, const char *msg)
{
    if (r->exist_key != INVALID_KEY)
        printf("%s: %s error %" PRIu64 " absent records %" PRIu64 "-%" PRIu64 ". Then keys %" PRIu64
               "-%" PRIu64 " exist. Key range %" PRIu64 "-%" PRIu64 "\n",
          fname, msg, (r->exist_key - r->first_miss) - 1, r->first_miss, r->exist_key - 1,
          r->exist_key, r->last_key, r->first_key, r->last_key);
}

/*
 * Signal handler to catch if the child died unexpectedly.
 */
/*
 * sig_handler --
 *     TODO: Add a comment describing this function.
 */
static void
sig_handler(int sig)
{
    pid_t pid;

    WT_UNUSED(sig);
    pid = wait(NULL);
    /*
     * The core file will indicate why the child exited. Choose EINVAL here.
     */
    testutil_die(EINVAL, "Child process %" PRIu64 " abnormally exited", (uint64_t)pid);
}

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(int argc, char *argv[])
{
    struct sigaction sa;
    struct stat sb;
    FILE *fp;
    REPORT c_rep[MAX_TH], l_rep[MAX_TH], o_rep[MAX_TH];
    WT_CONNECTION *conn;
    WT_CURSOR *cur_coll, *cur_local, *cur_oplog;
    WT_DECL_RET;
    WT_RAND_STATE rnd;
    WT_SESSION *session;
    pid_t pid;
    uint64_t absent_coll, absent_local, absent_oplog, count, key, last_key;
    uint64_t stable_fp, stable_val;
    uint32_t i, nth, timeout;
    int ch, status;
    char buf[512], statname[1024];
    char fname[64], kname[64];
    const char *working_dir;
    bool fatal, preserve, rand_th, rand_time, verify_only;

    (void)testutil_set_progname(argv);

    compat = inmem = false;
    use_ts = true;
    /*
     * Setting this to false forces us to use internal library code. Allow an override but default
     * to using that code.
     */
    use_txn = false;
    nth = MIN_TH;
    preserve = false;
    rand_th = rand_time = true;
    timeout = MIN_TIME;
    verify_only = false;
    working_dir = "WT_TEST.schema-abort";

    while ((ch = __wt_getopt(progname, argc, argv, "Cch:mpT:t:vxz")) != EOF)
        switch (ch) {
        case 'C':
            compat = true;
            break;
        case 'c':
            /* Variable-length columns only; fixed would require considerable changes */
            use_columns = true;
            break;
        case 'h':
            working_dir = __wt_optarg;
            break;
        case 'm':
            inmem = true;
            break;
        case 'p':
            preserve = true;
            break;
        case 'T':
            rand_th = false;
            nth = (uint32_t)atoi(__wt_optarg);
            break;
        case 't':
            rand_time = false;
            timeout = (uint32_t)atoi(__wt_optarg);
            break;
        case 'v':
            verify_only = true;
            break;
        case 'x':
            use_txn = true;
            break;
        case 'z':
            use_ts = false;
            break;
        default:
            usage();
        }
    argc -= __wt_optind;
    if (argc != 0)
        usage();

    testutil_work_dir_from_path(home, sizeof(home), working_dir);
    /*
     * If the user wants to verify they need to tell us how many threads there were so we can find
     * the old record files.
     */
    if (verify_only && rand_th) {
        fprintf(stderr, "Verify option requires specifying number of threads\n");
        exit(EXIT_FAILURE);
    }
    if (!verify_only) {
        testutil_make_work_dir(home);

        __wt_random_init_seed(NULL, &rnd);
        if (rand_time) {
            timeout = __wt_random(&rnd) % MAX_TIME;
            if (timeout < MIN_TIME)
                timeout = MIN_TIME;
        }
        if (rand_th) {
            nth = __wt_random(&rnd) % MAX_TH;
            if (nth < MIN_TH)
                nth = MIN_TH;
        }

        printf("Parent: compatibility: %s, in-mem log sync: %s, timestamp in use: %s\n",
          compat ? "true" : "false", inmem ? "true" : "false", use_ts ? "true" : "false");
        printf("Parent: Create %" PRIu32 " threads; sleep %" PRIu32 " seconds\n", nth, timeout);
        printf("CONFIG: %s%s%s%s -h %s -T %" PRIu32 " -t %" PRIu32 "\n", progname,
          compat ? " -C" : "", inmem ? " -m" : "", !use_ts ? " -z" : "", working_dir, nth, timeout);
        /*
         * Fork a child to insert as many items. We will then randomly kill the child, run recovery
         * and make sure all items we wrote exist after recovery runs.
         */
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = sig_handler;
        testutil_assert_errno(sigaction(SIGCHLD, &sa, NULL) == 0);
        testutil_assert_errno((pid = fork()) >= 0);

        if (pid == 0) { /* child */
            run_workload(nth);
            /* NOTREACHED */
        }

        /* parent */
        /*
         * Sleep for the configured amount of time before killing the child. Start the timeout from
         * the time we notice that the file has been created. That allows the test to run correctly
         * on really slow machines.
         */
        testutil_check(__wt_snprintf(statname, sizeof(statname), "%s/%s", home, ckpt_file));
        while (stat(statname, &sb) != 0)
            testutil_sleep_wait(1, pid);
        sleep(timeout);
        sa.sa_handler = SIG_DFL;
        testutil_assert_errno(sigaction(SIGCHLD, &sa, NULL) == 0);

        /*
         * !!! It should be plenty long enough to make sure more than
         * one log file exists.  If wanted, that check would be added
         * here.
         */
        printf("Kill child\n");
        testutil_assert_errno(kill(pid, SIGKILL) == 0);
        testutil_assert_errno(waitpid(pid, &status, 0) != -1);
    }
    /*
     * !!! If we wanted to take a copy of the directory before recovery,
     * this is the place to do it. Don't do it all the time because
     * it can use a lot of disk space, which can cause test machine
     * issues.
     */
    if (chdir(home) != 0)
        testutil_die(errno, "parent chdir: %s", home);

    /* Copy the data to a separate folder for debugging purpose. */
    testutil_copy_data(home);
    printf("Open database, run recovery and verify content\n");

    /*
     * Open the connection which forces recovery to be run.
     */
    testutil_check(wiredtiger_open(NULL, &event_handler, ENV_CONFIG_REC, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    /*
     * Open a cursor on all the tables.
     */
    testutil_check(session->open_cursor(session, uri_collection, NULL, NULL, &cur_coll));
    testutil_check(session->open_cursor(session, uri_local, NULL, NULL, &cur_local));
    testutil_check(session->open_cursor(session, uri_oplog, NULL, NULL, &cur_oplog));

    /*
     * Find the biggest stable timestamp value that was saved.
     */
    stable_val = 0;
    if (use_ts) {
        testutil_check(conn->query_timestamp(conn, buf, "get=recovery"));
        sscanf(buf, "%" SCNx64, &stable_val);
        printf("Got stable_val %" PRIu64 "\n", stable_val);
    }

    count = 0;
    absent_coll = absent_local = absent_oplog = 0;
    fatal = false;
    for (i = 0; i < nth; ++i) {
        initialize_rep(&c_rep[i]);
        initialize_rep(&l_rep[i]);
        initialize_rep(&o_rep[i]);
        testutil_check(__wt_snprintf(fname, sizeof(fname), RECORDS_FILE, i));
        if ((fp = fopen(fname, "r")) == NULL)
            testutil_die(errno, "fopen: %s", fname);

        /*
         * For every key in the saved file, verify that the key exists in the table after recovery.
         * If we're doing in-memory log buffering we never expect a record missing in the middle,
         * but records may be missing at the end. If we did write-no-sync, we expect every key to
         * have been recovered.
         */
        for (last_key = INVALID_KEY;; ++count, last_key = key) {
            ret = fscanf(fp, "%" SCNu64 "%" SCNu64 "\n", &stable_fp, &key);
            if (last_key == INVALID_KEY) {
                c_rep[i].first_key = key;
                l_rep[i].first_key = key;
                o_rep[i].first_key = key;
            }
            if (ret != EOF && ret != 2) {
                /*
                 * If we find a partial line, consider it like an EOF.
                 */
                if (ret == 1 || ret == 0)
                    break;
                testutil_die(errno, "fscanf");
            }
            if (ret == EOF)
                break;
            /*
             * If we're unlucky, the last line may be a partially written key at the end that can
             * result in a false negative error for a missing record. Detect it.
             */
            if (last_key != INVALID_KEY && key != last_key + 1) {
                printf("%s: Ignore partial record %" PRIu64 " last valid key %" PRIu64 "\n", fname,
                  key, last_key);
                break;
            }
            if (use_columns) {
                cur_coll->set_key(cur_coll, key + 1);
                cur_local->set_key(cur_local, key + 1);
                cur_oplog->set_key(cur_oplog, key + 1);
            } else {
                testutil_check(__wt_snprintf(kname, sizeof(kname), ROW_KEY_FORMAT, key));
                cur_coll->set_key(cur_coll, kname);
                cur_local->set_key(cur_local, kname);
                cur_oplog->set_key(cur_oplog, kname);
            }
            /*
             * The collection table should always only have the data as of the checkpoint.
             */
            if ((ret = cur_coll->search(cur_coll)) != 0) {
                if (ret != WT_NOTFOUND)
                    testutil_die(ret, "search");
                /*
                 * If we don't find a record, the stable timestamp written to our file better be
                 * larger than the saved one.
                 */
                if (!inmem && stable_fp != 0 && stable_fp <= stable_val) {
                    printf("%s: COLLECTION no record with key %" PRIu64 " record ts %" PRIu64
                           " <= stable ts %" PRIu64 "\n",
                      fname, key, stable_fp, stable_val);
                    absent_coll++;
                }
                if (c_rep[i].first_miss == INVALID_KEY)
                    c_rep[i].first_miss = key;
                c_rep[i].absent_key = key;
            } else if (c_rep[i].absent_key != INVALID_KEY && c_rep[i].exist_key == INVALID_KEY) {
                /*
                 * If we get here we found a record that exists after absent records, a hole in our
                 * data.
                 */
                c_rep[i].exist_key = key;
                fatal = true;
            } else if (!inmem && stable_fp != 0 && stable_fp > stable_val) {
                /*
                 * If we found a record, the stable timestamp written to our file better be no
                 * larger than the checkpoint one.
                 */
                printf("%s: COLLECTION record with key %" PRIu64 " record ts %" PRIu64
                       " > stable ts %" PRIu64 "\n",
                  fname, key, stable_fp, stable_val);
                fatal = true;
            }
            /*
             * The local table should always have all data.
             */
            if ((ret = cur_local->search(cur_local)) != 0) {
                if (ret != WT_NOTFOUND)
                    testutil_die(ret, "search");
                if (!inmem)
                    printf("%s: LOCAL no record with key %" PRIu64 "\n", fname, key);
                absent_local++;
                if (l_rep[i].first_miss == INVALID_KEY)
                    l_rep[i].first_miss = key;
                l_rep[i].absent_key = key;
            } else if (l_rep[i].absent_key != INVALID_KEY && l_rep[i].exist_key == INVALID_KEY) {
                /*
                 * We should never find an existing key after we have detected one missing.
                 */
                l_rep[i].exist_key = key;
                fatal = true;
            }
            /*
             * The oplog table should always have all data.
             */
            if ((ret = cur_oplog->search(cur_oplog)) != 0) {
                if (ret != WT_NOTFOUND)
                    testutil_die(ret, "search");
                if (!inmem)
                    printf("%s: OPLOG no record with key %" PRIu64 "\n", fname, key);
                absent_oplog++;
                if (o_rep[i].first_miss == INVALID_KEY)
                    o_rep[i].first_miss = key;
                o_rep[i].absent_key = key;
            } else if (o_rep[i].absent_key != INVALID_KEY && o_rep[i].exist_key == INVALID_KEY) {
                /*
                 * We should never find an existing key after we have detected one missing.
                 */
                o_rep[i].exist_key = key;
                fatal = true;
            }
        }
        c_rep[i].last_key = last_key;
        l_rep[i].last_key = last_key;
        o_rep[i].last_key = last_key;
        testutil_assert_errno(fclose(fp) == 0);
        print_missing(&c_rep[i], fname, "COLLECTION");
        print_missing(&l_rep[i], fname, "LOCAL");
        print_missing(&o_rep[i], fname, "OPLOG");
    }
    testutil_check(conn->close(conn, NULL));
    if (!inmem && absent_coll) {
        printf("COLLECTION: %" PRIu64 " record(s) absent from %" PRIu64 "\n", absent_coll, count);
        fatal = true;
    }
    if (!inmem && absent_local) {
        printf("LOCAL: %" PRIu64 " record(s) absent from %" PRIu64 "\n", absent_local, count);
        fatal = true;
    }
    if (!inmem && absent_oplog) {
        printf("OPLOG: %" PRIu64 " record(s) absent from %" PRIu64 "\n", absent_oplog, count);
        fatal = true;
    }
    if (fatal)
        return (EXIT_FAILURE);
    printf("%" PRIu64 " records verified\n", count);
    if (!preserve) {
        testutil_clean_test_artifacts(home);
        /* At this point $PATH is inside `home`, which we intend to delete. cd to the parent dir. */
        if (chdir("../") != 0)
            testutil_die(errno, "root chdir: %s", home);
        testutil_clean_work_dir(home);
    }
    return (EXIT_SUCCESS);
}
