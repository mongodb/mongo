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
 * We also create a fourth table that is not logged and not involved directly
 * in timestamps to store the stable timestamp.  That way we can know what the
 * latest stable timestamp is on checkpoint.
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
#define MAX_CKPT_INVL 5 /* Maximum interval between checkpoints */
#define MAX_TH 200      /* Maximum configurable threads */
#define MAX_TIME 40
#define MAX_VAL 1024
#define MIN_TH 5
#define MIN_TIME 10
#define PREPARE_DURABLE_AHEAD_COMMIT 10
#define PREPARE_FREQ 5
#define PREPARE_PCT 10
#define PREPARE_YIELD (PREPARE_FREQ * 10)
#define RECORDS_FILE "records-%" PRIu32
/* Include worker threads and prepare extra sessions */
#define SESSION_MAX (MAX_TH + 3 + MAX_TH * PREPARE_PCT)

static const char *table_pfx = "table";
static const char *const uri_collection = "collection";
static const char *const uri_local = "local";
static const char *const uri_oplog = "oplog";
static const char *const uri_shadow = "shadow";

static const char *const ckpt_file = "checkpoint_done";

static bool columns, compat, inmem, stress, use_ts;
static volatile uint64_t global_ts = 1;

/*
 * The configuration sets the eviction update and dirty targets at 20% so that on average, each
 * thread can have a couple of dirty pages before eviction threads kick in. See below where these
 * symbols are used for cache sizing - we'll have about 10 pages allocated per thread. On the other
 * side, the eviction update and dirty triggers are 90%, so application threads aren't involved in
 * eviction until we're close to running out of cache.
 */
#define ENV_CONFIG_ADD_COMPAT ",compatibility=(release=\"2.9\")"
#define ENV_CONFIG_ADD_EVICT_DIRTY ",eviction_dirty_target=20,eviction_dirty_trigger=90"
#define ENV_CONFIG_ADD_STRESS ",timing_stress_for_test=[prepare_checkpoint_delay]"

#define ENV_CONFIG_DEF                                        \
    "cache_size=%" PRIu32                                     \
    "M,create,"                                               \
    "debug_mode=(table_logging=true,checkpoint_retention=5)," \
    "eviction_updates_target=20,eviction_updates_trigger=90," \
    "log=(archive=true,file_max=10M,enabled),session_max=%d," \
    "statistics=(fast),statistics_log=(wait=1,json=true)"
#define ENV_CONFIG_TXNSYNC \
    ENV_CONFIG_DEF         \
    ",transaction_sync=(enabled,method=none)"
#define ENV_CONFIG_REC "log=(archive=false,recover=on)"

/*
 * A minimum width of 10, along with zero filling, means that all the keys sort according to their
 * integer value, making each thread's key space distinct. For column-store we just use the integer
 * values and that has the same effect.
 */
#define KEY_STRINGFORMAT ("%010" PRIu64)

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
} THREAD_DATA;

/* Lock for transactional ops that set or query a timestamp. */
static pthread_rwlock_t ts_lock;

static void handler(int) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
static void usage(void) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
static void
usage(void)
{
    fprintf(stderr, "usage: %s [-h dir] [-T threads] [-t time] [-Cmvz]\n", progname);
    exit(EXIT_FAILURE);
}

/*
 * thread_ts_run --
 *     Runner function for a timestamp thread.
 */
static WT_THREAD_RET
thread_ts_run(void *arg)
{
    WT_DECL_RET;
    WT_RAND_STATE rnd;
    WT_SESSION *session;
    THREAD_DATA *td;
    uint32_t rand_op;
    int dbg;
    char tscfg[64], ts_string[WT_TS_HEX_STRING_SIZE];
    bool first;

    td = (THREAD_DATA *)arg;
    __wt_random_init(&rnd);

    testutil_check(td->conn->open_session(td->conn, NULL, NULL, &session));
    first = true;
    /* Update the oldest timestamp every 1 millisecond. */
    for (;;) {
        /*
         * We get the last committed timestamp periodically in order to update the oldest timestamp,
         * that requires locking out transactional ops that set or query a timestamp.
         */
        testutil_check(pthread_rwlock_wrlock(&ts_lock));
        ret = td->conn->query_timestamp(td->conn, ts_string, "get=all_durable");
        testutil_check(pthread_rwlock_unlock(&ts_lock));
        testutil_assert(ret == 0 || ret == WT_NOTFOUND);
        if (ret == 0) {
            rand_op = __wt_random(&rnd) % 4;
            /*
             * Periodically let the oldest timestamp lag. Other times set the stable and oldest
             * timestamps as separate API calls. The rest of the time set them both as one call.
             */
            if (rand_op == 0) {
                testutil_check(
                  __wt_snprintf(tscfg, sizeof(tscfg), "stable_timestamp=%s", ts_string));
                testutil_check(td->conn->set_timestamp(td->conn, tscfg));
                testutil_check(
                  __wt_snprintf(tscfg, sizeof(tscfg), "oldest_timestamp=%s", ts_string));
                testutil_check(td->conn->set_timestamp(td->conn, tscfg));
            } else {
                if (!first && rand_op == 1)
                    testutil_check(
                      __wt_snprintf(tscfg, sizeof(tscfg), "stable_timestamp=%s", ts_string));
                else
                    testutil_check(__wt_snprintf(tscfg, sizeof(tscfg),
                      "oldest_timestamp=%s,stable_timestamp=%s", ts_string, ts_string));
                testutil_check(td->conn->set_timestamp(td->conn, tscfg));
            }
            first = false;
            /*
             * Set and reset the checkpoint retention setting on a regular basis. We want to test
             * racing with the internal archive thread while we're here.
             */
            dbg = __wt_random(&rnd) % 2;
            if (dbg == 0)
                testutil_check(
                  __wt_snprintf(tscfg, sizeof(tscfg), "debug_mode=(checkpoint_retention=0)"));
            else
                testutil_check(
                  __wt_snprintf(tscfg, sizeof(tscfg), "debug_mode=(checkpoint_retention=5)"));
            testutil_check(td->conn->reconfigure(td->conn, tscfg));
        }
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
    FILE *fp;
    WT_RAND_STATE rnd;
    WT_SESSION *session;
    THREAD_DATA *td;
    uint64_t stable;
    uint32_t sleep_time;
    int i;
    bool first_ckpt;
    char ts_string[WT_TS_HEX_STRING_SIZE];

    __wt_random_init(&rnd);

    td = (THREAD_DATA *)arg;
    /*
     * Keep a separate file with the records we wrote for checking.
     */
    (void)unlink(ckpt_file);
    testutil_check(td->conn->open_session(td->conn, NULL, NULL, &session));
    first_ckpt = true;
    for (i = 0;; ++i) {
        sleep_time = __wt_random(&rnd) % MAX_CKPT_INVL;
        sleep(sleep_time);
        /*
         * Since this is the default, send in this string even if running without timestamps.
         */
        testutil_check(session->checkpoint(session, "use_timestamp=true"));
        testutil_check(td->conn->query_timestamp(td->conn, ts_string, "get=last_checkpoint"));
        testutil_assert(sscanf(ts_string, "%" SCNx64, &stable) == 1);
        printf("Checkpoint %d complete at stable %" PRIu64 ".\n", i, stable);
        fflush(stdout);
        /*
         * Create the checkpoint file so that the parent process knows at least one checkpoint has
         * finished and can start its timer. If running with timestamps, wait until the stable
         * timestamp has moved past WT_TS_NONE to give writer threads a chance to add something to
         * the database.
         */
        if (first_ckpt && (!use_ts || stable != WT_TS_NONE)) {
            testutil_checksys((fp = fopen(ckpt_file, "w")) == NULL);
            first_ckpt = false;
            testutil_checksys(fclose(fp) != 0);
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
    WT_CURSOR *cur_coll, *cur_local, *cur_oplog, *cur_shadow;
    WT_DECL_RET;
    WT_ITEM data;
    WT_RAND_STATE rnd;
    WT_SESSION *prepared_session, *session;
    THREAD_DATA *td;
    uint64_t i, active_ts;
    char cbuf[MAX_VAL], lbuf[MAX_VAL], obuf[MAX_VAL];
    char kname[64], tscfg[64], uri[128];
    bool durable_ahead_commit, use_prep;

    __wt_random_init(&rnd);
    memset(cbuf, 0, sizeof(cbuf));
    memset(lbuf, 0, sizeof(lbuf));
    memset(obuf, 0, sizeof(obuf));
    memset(kname, 0, sizeof(kname));

    prepared_session = NULL;
    td = (THREAD_DATA *)arg;
    /*
     * Set up the separate file for checking.
     */
    testutil_check(__wt_snprintf(cbuf, sizeof(cbuf), RECORDS_FILE, td->info));
    (void)unlink(cbuf);
    testutil_checksys((fp = fopen(cbuf, "w")) == NULL);
    /*
     * Set to line buffering. But that is advisory only. We've seen cases where the result files end
     * up with partial lines.
     */
    __wt_stream_set_line_buffer(fp);

    /*
     * Have 10% of the threads use prepared transactions if timestamps are in use. Thread numbers
     * start at 0 so we're always guaranteed that at least one thread is using prepared
     * transactions.
     */
    use_prep = (use_ts && td->info % PREPARE_PCT == 0) ? true : false;
    durable_ahead_commit = false;

    /*
     * For the prepared case we have two sessions so that the oplog session can have its own
     * transaction in parallel with the collection session We need this because prepared
     * transactions cannot have any operations that modify a table that is logged. But we also want
     * to test mixed logged and not-logged transactions.
     */
    testutil_check(td->conn->open_session(td->conn, NULL, "isolation=snapshot", &session));
    if (use_prep)
        testutil_check(
          td->conn->open_session(td->conn, NULL, "isolation=snapshot", &prepared_session));
    /*
     * Open a cursor to each table.
     */
    testutil_check(__wt_snprintf(uri, sizeof(uri), "%s:%s", table_pfx, uri_collection));
    if (use_prep)
        testutil_check(prepared_session->open_cursor(prepared_session, uri, NULL, NULL, &cur_coll));
    else
        testutil_check(session->open_cursor(session, uri, NULL, NULL, &cur_coll));
    testutil_check(__wt_snprintf(uri, sizeof(uri), "%s:%s", table_pfx, uri_shadow));
    if (use_prep)
        testutil_check(
          prepared_session->open_cursor(prepared_session, uri, NULL, NULL, &cur_shadow));
    else
        testutil_check(session->open_cursor(session, uri, NULL, NULL, &cur_shadow));

    testutil_check(__wt_snprintf(uri, sizeof(uri), "%s:%s", table_pfx, uri_local));
    if (use_prep)
        testutil_check(
          prepared_session->open_cursor(prepared_session, uri, NULL, NULL, &cur_local));
    else
        testutil_check(session->open_cursor(session, uri, NULL, NULL, &cur_local));
    testutil_check(__wt_snprintf(uri, sizeof(uri), "%s:%s", table_pfx, uri_oplog));
    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cur_oplog));

    /*
     * Write our portion of the key space until we're killed.
     */
    printf("Thread %" PRIu32 " starts at %" PRIu64 "\n", td->info, td->start);
    active_ts = 0;
    for (i = td->start;; ++i) {
        testutil_check(session->begin_transaction(session, NULL));
        if (use_prep)
            testutil_check(prepared_session->begin_transaction(prepared_session, NULL));

        if (use_ts) {
            testutil_check(pthread_rwlock_rdlock(&ts_lock));
            active_ts = __wt_atomic_addv64(&global_ts, 2);
            testutil_check(
              __wt_snprintf(tscfg, sizeof(tscfg), "commit_timestamp=%" PRIx64, active_ts));
            /*
             * Set the transaction's timestamp now before performing the operation. If we are using
             * prepared transactions, set the timestamp for the session used for oplog. The
             * collection session in that case would continue to use this timestamp.
             */
            testutil_check(session->timestamp_transaction(session, tscfg));
            testutil_check(pthread_rwlock_unlock(&ts_lock));
        }

        if (columns) {
            cur_coll->set_key(cur_coll, i + 1);
            cur_local->set_key(cur_local, i + 1);
            cur_oplog->set_key(cur_oplog, i + 1);
            cur_shadow->set_key(cur_shadow, i + 1);
        } else {
            testutil_check(__wt_snprintf(kname, sizeof(kname), KEY_STRINGFORMAT, i));
            cur_coll->set_key(cur_coll, kname);
            cur_local->set_key(cur_local, kname);
            cur_oplog->set_key(cur_oplog, kname);
            cur_shadow->set_key(cur_shadow, kname);
        }
        /*
         * Put an informative string into the value so that it can be viewed well in a binary dump.
         */
        testutil_check(__wt_snprintf(cbuf, sizeof(cbuf),
          "COLL: thread:%" PRIu32 " ts:%" PRIu64 " key: %" PRIu64, td->info, active_ts, i));
        testutil_check(__wt_snprintf(lbuf, sizeof(lbuf),
          "LOCAL: thread:%" PRIu32 " ts:%" PRIu64 " key: %" PRIu64, td->info, active_ts, i));
        testutil_check(__wt_snprintf(obuf, sizeof(obuf),
          "OPLOG: thread:%" PRIu32 " ts:%" PRIu64 " key: %" PRIu64, td->info, active_ts, i));
        data.size = __wt_random(&rnd) % MAX_VAL;
        data.data = cbuf;
        cur_coll->set_value(cur_coll, &data);
        if ((ret = cur_coll->insert(cur_coll)) == WT_ROLLBACK)
            goto rollback;
        testutil_check(ret);
        cur_shadow->set_value(cur_shadow, &data);
        if (use_ts) {
            /*
             * Change the timestamp in the middle of the transaction so that we simulate a
             * secondary.
             */
            ++active_ts;
            testutil_check(
              __wt_snprintf(tscfg, sizeof(tscfg), "commit_timestamp=%" PRIx64, active_ts));
            testutil_check(session->timestamp_transaction(session, tscfg));
        }
        if ((ret = cur_shadow->insert(cur_shadow)) == WT_ROLLBACK)
            goto rollback;
        data.size = __wt_random(&rnd) % MAX_VAL;
        data.data = obuf;
        cur_oplog->set_value(cur_oplog, &data);
        if ((ret = cur_oplog->insert(cur_oplog)) == WT_ROLLBACK)
            goto rollback;
        if (use_prep) {
            /*
             * Run with prepare every once in a while. And also yield after prepare sometimes too.
             * This is only done on the collection session.
             */
            if (i % PREPARE_FREQ == 0) {
                testutil_check(
                  __wt_snprintf(tscfg, sizeof(tscfg), "prepare_timestamp=%" PRIx64, active_ts));
                testutil_check(prepared_session->prepare_transaction(prepared_session, tscfg));
                if (i % PREPARE_YIELD == 0)
                    __wt_yield();
                /*
                 * Make half of the prepared transactions' durable timestamp larger than their
                 * commit timestamp.
                 */
                durable_ahead_commit = i % PREPARE_DURABLE_AHEAD_COMMIT == 0;
                testutil_check(__wt_snprintf(tscfg, sizeof(tscfg),
                  "commit_timestamp=%" PRIx64 ",durable_timestamp=%" PRIx64, active_ts,
                  durable_ahead_commit ? active_ts + 4 : active_ts));
                /* Ensure the global timestamp is not behind the all durable timestamp. */
                if (durable_ahead_commit)
                    __wt_atomic_addv64(&global_ts, 4);
            } else
                testutil_check(
                  __wt_snprintf(tscfg, sizeof(tscfg), "commit_timestamp=%" PRIx64, active_ts));

            testutil_check(prepared_session->commit_transaction(prepared_session, tscfg));
        }
        testutil_check(session->commit_transaction(session, NULL));
        /*
         * Insert into the local table outside the timestamp txn. This must occur after the
         * timestamp transaction, not before, because of the possibility of rollback in the
         * transaction. The local table must stay in sync with the other tables.
         */
        data.size = __wt_random(&rnd) % MAX_VAL;
        data.data = lbuf;
        cur_local->set_value(cur_local, &data);
        testutil_check(cur_local->insert(cur_local));

        /* Save the timestamps and key separately for checking later. */
        if (fprintf(fp, "%" PRIu64 " %" PRIu64 " %" PRIu64 "\n", active_ts,
              durable_ahead_commit ? active_ts + 4 : active_ts, i) < 0)
            testutil_die(EIO, "fprintf");

        if (0) {
rollback:
            testutil_check(session->rollback_transaction(session, NULL));
            if (use_prep)
                testutil_check(prepared_session->rollback_transaction(prepared_session, NULL));
        }
    }
    /* NOTREACHED */
}

/*
 * Child process creates the database and table, and then creates worker threads to add data until
 * it is killed by the parent.
 */
static void run_workload(uint32_t) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
static void
run_workload(uint32_t nth)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    THREAD_DATA *td;
    wt_thread_t *thr;
    uint32_t cache_mb, ckpt_id, i, ts_id;
    char envconf[512], uri[128];
    const char *table_config, *table_config_nolog;

    thr = dcalloc(nth + 2, sizeof(*thr));
    td = dcalloc(nth + 2, sizeof(THREAD_DATA));

    /*
     * Size the cache appropriately for the number of threads. Each thread adds keys sequentially to
     * its own portion of the key space, so each thread will be dirtying one page at a time. By
     * default, a leaf page grows to 32K in size before it splits and the thread begins to fill
     * another page. We'll budget for 10 full size leaf pages per thread in the cache plus a little
     * extra in the total for overhead.
     */
    cache_mb = ((32 * WT_KILOBYTE * 10) * nth) / WT_MEGABYTE + 20;

    if (chdir(home) != 0)
        testutil_die(errno, "Child chdir: %s", home);
    if (inmem)
        testutil_check(
          __wt_snprintf(envconf, sizeof(envconf), ENV_CONFIG_DEF, cache_mb, SESSION_MAX));
    else
        testutil_check(
          __wt_snprintf(envconf, sizeof(envconf), ENV_CONFIG_TXNSYNC, cache_mb, SESSION_MAX));
    if (compat)
        strcat(envconf, ENV_CONFIG_ADD_COMPAT);
    if (stress)
        strcat(envconf, ENV_CONFIG_ADD_STRESS);

    /*
     * The eviction dirty target and trigger configurations are not compatible with certain other
     * configurations.
     */
    if (!compat && !inmem)
        strcat(envconf, ENV_CONFIG_ADD_EVICT_DIRTY);

    printf("wiredtiger_open configuration: %s\n", envconf);
    testutil_check(wiredtiger_open(NULL, NULL, envconf, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /*
     * Create all the tables.
     */
    if (columns) {
        table_config_nolog = "key_format=r,value_format=u,log=(enabled=false)";
        table_config = "key_format=r,value_format=u";
    } else {
        table_config_nolog = "key_format=S,value_format=u,log=(enabled=false)";
        table_config = "key_format=S,value_format=u";
    }
    testutil_check(__wt_snprintf(uri, sizeof(uri), "%s:%s", table_pfx, uri_collection));
    testutil_check(session->create(session, uri, table_config_nolog));
    testutil_check(__wt_snprintf(uri, sizeof(uri), "%s:%s", table_pfx, uri_shadow));
    testutil_check(session->create(session, uri, table_config_nolog));
    testutil_check(__wt_snprintf(uri, sizeof(uri), "%s:%s", table_pfx, uri_local));
    testutil_check(session->create(session, uri, table_config));
    testutil_check(__wt_snprintf(uri, sizeof(uri), "%s:%s", table_pfx, uri_oplog));
    testutil_check(session->create(session, uri, table_config));
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
    exit(EXIT_SUCCESS);
}

extern int __wt_optind;
extern char *__wt_optarg;

/*
 * Initialize a report structure. Since zero is a valid key we cannot just clear it.
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
static void
handler(int sig)
{
    pid_t pid;

    WT_UNUSED(sig);
    pid = wait(NULL);
    /*
     * The core file will indicate why the child exited. Choose EINVAL here.
     */
    testutil_die(EINVAL, "Child process %" PRIu64 " abnormally exited", (uint64_t)pid);
}

int
main(int argc, char *argv[])
{
    struct sigaction sa;
    struct stat sb;
    FILE *fp;
    REPORT c_rep[MAX_TH], l_rep[MAX_TH], o_rep[MAX_TH];
    WT_CONNECTION *conn;
    WT_CURSOR *cur_coll, *cur_local, *cur_oplog, *cur_shadow;
    WT_RAND_STATE rnd;
    WT_SESSION *session;
    pid_t pid;
    uint64_t absent_coll, absent_local, absent_oplog, absent_shadow, count, key, last_key;
    uint64_t commit_fp, durable_fp, stable_val;
    uint32_t i, nth, timeout;
    int ch, status, ret;
    const char *working_dir;
    char buf[512], fname[64], kname[64], statname[1024];
    char ts_string[WT_TS_HEX_STRING_SIZE];
    bool fatal, rand_th, rand_time, verify_only;

    (void)testutil_set_progname(argv);

    columns = compat = inmem = stress = false;
    use_ts = true;
    nth = MIN_TH;
    rand_th = rand_time = true;
    timeout = MIN_TIME;
    verify_only = false;
    working_dir = "WT_TEST.timestamp-abort";

    while ((ch = __wt_getopt(progname, argc, argv, "Cch:LmsT:t:vz")) != EOF)
        switch (ch) {
        case 'C':
            compat = true;
            break;
        case 'c':
            /* Variable-length columns only (for now) */
            columns = true;
            break;
        case 'h':
            working_dir = __wt_optarg;
            break;
        case 'L':
            table_pfx = "lsm";
            break;
        case 'm':
            inmem = true;
            break;
        case 's':
            stress = true;
            break;
        case 'T':
            rand_th = false;
            nth = (uint32_t)atoi(__wt_optarg);
            if (nth > MAX_TH) {
                fprintf(
                  stderr, "Number of threads is larger than the maximum %" PRId32 "\n", MAX_TH);
                return (EXIT_FAILURE);
            }
            break;
        case 't':
            rand_time = false;
            timeout = (uint32_t)atoi(__wt_optarg);
            break;
        case 'v':
            verify_only = true;
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
    testutil_check(pthread_rwlock_init(&ts_lock, NULL));

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

        printf(
          "Parent: compatibility: %s, in-mem log sync: %s, add timing stress: %s, timestamp in "
          "use: %s\n",
          compat ? "true" : "false", inmem ? "true" : "false", stress ? "true" : "false",
          use_ts ? "true" : "false");
        printf("Parent: Create %" PRIu32 " threads; sleep %" PRIu32 " seconds\n", nth, timeout);
        printf("CONFIG: %s%s%s%s%s%s -h %s -T %" PRIu32 " -t %" PRIu32 "\n", progname,
          compat ? " -C" : "", columns ? " -c" : "", inmem ? " -m" : "", stress ? " -s" : "",
          !use_ts ? " -z" : "", working_dir, nth, timeout);
        /*
         * Fork a child to insert as many items. We will then randomly kill the child, run recovery
         * and make sure all items we wrote exist after recovery runs.
         */
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = handler;
        testutil_checksys(sigaction(SIGCHLD, &sa, NULL));
        testutil_checksys((pid = fork()) < 0);

        if (pid == 0) { /* child */
            run_workload(nth);
            return (EXIT_SUCCESS);
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
        testutil_checksys(sigaction(SIGCHLD, &sa, NULL));

        /*
         * !!! It should be plenty long enough to make sure more than
         * one log file exists.  If wanted, that check would be added
         * here.
         */
        printf("Kill child\n");
        testutil_checksys(kill(pid, SIGKILL) != 0);
        testutil_checksys(waitpid(pid, &status, 0) == -1);
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
    testutil_check(wiredtiger_open(NULL, NULL, ENV_CONFIG_REC, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    /*
     * Open a cursor on all the tables.
     */
    testutil_check(__wt_snprintf(buf, sizeof(buf), "%s:%s", table_pfx, uri_collection));
    testutil_check(session->open_cursor(session, buf, NULL, NULL, &cur_coll));
    testutil_check(__wt_snprintf(buf, sizeof(buf), "%s:%s", table_pfx, uri_shadow));
    testutil_check(session->open_cursor(session, buf, NULL, NULL, &cur_shadow));
    testutil_check(__wt_snprintf(buf, sizeof(buf), "%s:%s", table_pfx, uri_local));
    testutil_check(session->open_cursor(session, buf, NULL, NULL, &cur_local));
    testutil_check(__wt_snprintf(buf, sizeof(buf), "%s:%s", table_pfx, uri_oplog));
    testutil_check(session->open_cursor(session, buf, NULL, NULL, &cur_oplog));

    /*
     * Find the biggest stable timestamp value that was saved.
     */
    stable_val = 0;
    if (use_ts) {
        testutil_check(conn->query_timestamp(conn, ts_string, "get=recovery"));
        testutil_assert(sscanf(ts_string, "%" SCNx64, &stable_val) == 1);
        printf("Got stable_val %" PRIu64 "\n", stable_val);
    }

    count = 0;
    absent_coll = absent_local = absent_oplog = absent_shadow = 0;
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
            ret = fscanf(fp, "%" SCNu64 "%" SCNu64 "%" SCNu64 "\n", &commit_fp, &durable_fp, &key);
            if (last_key == INVALID_KEY) {
                c_rep[i].first_key = key;
                l_rep[i].first_key = key;
                o_rep[i].first_key = key;
            }
            if (ret != EOF && ret != 3) {
                /*
                 * If we find a partial line, consider it like an EOF.
                 */
                if (ret == 2 || ret == 1 || ret == 0)
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

            if (columns) {
                cur_coll->set_key(cur_coll, key + 1);
                cur_local->set_key(cur_local, key + 1);
                cur_oplog->set_key(cur_oplog, key + 1);
                cur_shadow->set_key(cur_shadow, key + 1);
            } else {
                testutil_check(__wt_snprintf(kname, sizeof(kname), KEY_STRINGFORMAT, key));
                cur_coll->set_key(cur_coll, kname);
                cur_local->set_key(cur_local, kname);
                cur_oplog->set_key(cur_oplog, kname);
                cur_shadow->set_key(cur_shadow, kname);
            }

            /*
             * The collection table should always only have the data as of the checkpoint. The
             * shadow table should always have the exact same data (or not) as the collection table,
             * except for the last key that may be committed after the stable timestamp.
             */
            if ((ret = cur_coll->search(cur_coll)) != 0) {
                if (ret != WT_NOTFOUND)
                    testutil_die(ret, "search");
                if ((ret = cur_shadow->search(cur_shadow)) == 0)
                    testutil_die(ret, "shadow search success");

                /*
                 * If we don't find a record, the durable timestamp written to our file better be
                 * larger than the saved one.
                 */
                if (!inmem && durable_fp != 0 && durable_fp <= stable_val) {
                    printf("%s: COLLECTION no record with key %" PRIu64
                           " record durable ts %" PRIu64 " <= stable ts %" PRIu64 "\n",
                      fname, key, durable_fp, stable_val);
                    absent_coll++;
                }
                if (c_rep[i].first_miss == INVALID_KEY)
                    c_rep[i].first_miss = key;
                c_rep[i].absent_key = key;
            } else if ((ret = cur_shadow->search(cur_shadow)) != 0) {
                if (ret != WT_NOTFOUND)
                    testutil_die(ret, "shadow search");
                /*
                 * We respectively insert the record to the collection table at timestamp t and to
                 * the shadow table at t + 1. If the checkpoint finishes at timestamp t, the last
                 * shadow table record will be removed by rollback to stable after restart.
                 */
                if (durable_fp <= stable_val) {
                    printf("%s: SHADOW no record with key %" PRIu64 "\n", fname, key);
                    absent_shadow++;
                }
            } else if (c_rep[i].absent_key != INVALID_KEY && c_rep[i].exist_key == INVALID_KEY) {
                /*
                 * If we get here we found a record that exists after absent records, a hole in our
                 * data.
                 */
                c_rep[i].exist_key = key;
                fatal = true;
            } else if (!inmem && commit_fp != 0 && commit_fp > stable_val) {
                /*
                 * If we found a record, the commit timestamp written to our file better be no
                 * larger than the checkpoint one.
                 */
                printf("%s: COLLECTION record with key %" PRIu64 " commit record ts %" PRIu64
                       " > stable ts %" PRIu64 "\n",
                  fname, key, commit_fp, stable_val);
                fatal = true;
            } else if ((ret = cur_shadow->search(cur_shadow)) != 0)
                /* Collection and shadow both have the data. */
                testutil_die(ret, "shadow search failure");

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
        testutil_checksys(fclose(fp) != 0);
        print_missing(&c_rep[i], fname, "COLLECTION");
        print_missing(&l_rep[i], fname, "LOCAL");
        print_missing(&o_rep[i], fname, "OPLOG");
    }
    testutil_check(conn->close(conn, NULL));
    if (!inmem && absent_coll) {
        printf("COLLECTION: %" PRIu64 " record(s) absent from %" PRIu64 "\n", absent_coll, count);
        fatal = true;
    }
    if (!inmem && absent_shadow) {
        printf("SHADOW: %" PRIu64 " record(s) absent from %" PRIu64 "\n", absent_shadow, count);
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
    testutil_check(pthread_rwlock_destroy(&ts_lock));
    if (fatal)
        return (EXIT_FAILURE);
    printf("%" PRIu64 " records verified\n", count);
    return (EXIT_SUCCESS);
}
