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
static bool use_columns = false;

/*
 * Spin up a child process to do operations and checkpoint. For each set of operations on a key,
 * insert key X at timestamp X, move the stable timestamp to X, and delete the key at timestamp X+1.
 * If the amount of data is larger than MAX_DATA, move the oldest timestamp to make half of the data
 * obsolete.
 *
 * In verification, kill the child process and reopen the database to run recovery. Query the
 * database to get the stable and oldest timestamp. For keys from oldest to stable timestamp, make
 * sure each key X is visible at timestamp X.
 *
 * The test is only for non-logged tables. For logged tables, the current implementation only
 * guarantees the user to see a consistent snapshot view of data at the last successful commit after
 * recovery by reading without a timestamp. Whether it is possible to read historical versions based
 * on timestamps from a logged table after recovery is not defined and implemented yet.
 */
#define ROW_KEY_FORMAT ("%010" PRIu64)

#define MAX_CKPT_INVL 5 /* Maximum interval between checkpoints */
#define MAX_DATA 1000
#define MAX_TIME 40
#define MIN_TIME 10

static const char *const uri = "table:wt6616-checkpoint-oldest-ts";

static const char *const ckpt_file = "checkpoint_done";

#define ENV_CONFIG                                            \
    "cache_size=50M,"                                         \
    "create,"                                                 \
    "eviction_updates_target=20,eviction_updates_trigger=90," \
    "log=(archive=true,file_max=10M,enabled),"                \
    "statistics=(fast),statistics_log=(wait=1,json=true),"    \
    "timing_stress_for_test=[checkpoint_slow]"

#define ENV_CONFIG_REC "log=(archive=false,recover=on)"

static void handler(int) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
static void usage(void) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
static void
usage(void)
{
    fprintf(stderr, "usage: %s [-h dir] [-t time]\n", progname);
    exit(EXIT_FAILURE);
}

/*
 * thread_ckpt_run --
 *     Runner function for the checkpoint thread.
 */
static WT_THREAD_RET
thread_ckpt_run(void *arg)
{
    FILE *fp;
    WT_CONNECTION *conn;
    WT_RAND_STATE rnd;
    WT_SESSION *session;
    uint64_t stable;
    uint32_t sleep_time;
    int i;
    char ts_string[WT_TS_HEX_STRING_SIZE];
    bool first_ckpt;

    __wt_random_init(&rnd);

    conn = (WT_CONNECTION *)arg;
    /* Keep a separate file with the records we wrote for checking. */
    (void)unlink(ckpt_file);
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    first_ckpt = true;
    for (i = 0;; ++i) {
        sleep_time = __wt_random(&rnd) % MAX_CKPT_INVL;
        sleep(sleep_time);
        /* Run checkpoint with timestamps. */
        testutil_check(session->checkpoint(session, "use_timestamp=true"));
        testutil_check(conn->query_timestamp(conn, ts_string, "get=last_checkpoint"));
        testutil_assert(sscanf(ts_string, "%" SCNx64, &stable) == 1);
        printf("Checkpoint %d complete at stable %" PRIu64 ".\n", i, stable);
        fflush(stdout);
        /*
         * Create the checkpoint file so that the parent process knows at least one checkpoint has
         * finished and can start its timer.
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
 *     Runner function for the worker thread.
 */
static WT_THREAD_RET
thread_run(void *arg)
{
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_ITEM data;
    WT_SESSION *session;
    uint64_t oldest_ts, ts;
    char kname[64], tscfg[64];

    conn = (WT_CONNECTION *)arg;
    memset(kname, 0, sizeof(kname));

    testutil_check(conn->open_session(conn, NULL, "isolation=snapshot", &session));
    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));

    /* Insert and then delete the keys until we're killed. */
    printf("Worker thread started.\n");
    for (oldest_ts = 0, ts = 1;; ++ts) {
        testutil_check(__wt_snprintf(kname, sizeof(kname), ROW_KEY_FORMAT, ts));

        /* Insert the same value for key and value. */
        testutil_check(session->begin_transaction(session, NULL));
        if (use_columns)
            cursor->set_key(cursor, ts);
        else
            cursor->set_key(cursor, kname);
        data.data = kname;
        data.size = sizeof(kname);
        cursor->set_value(cursor, &data);
        testutil_check(cursor->insert(cursor));
        testutil_check(__wt_snprintf(tscfg, sizeof(tscfg), "commit_timestamp=%" PRIx64, ts));
        testutil_check(session->commit_transaction(session, tscfg));

        /* Update stable timestamp to the current timestamp. */
        testutil_check(__wt_snprintf(tscfg, sizeof(tscfg), "stable_timestamp=%" PRIx64, ts));
        testutil_check(conn->set_timestamp(conn, tscfg));

        /* Remove the key using a higher timestamp. */
        testutil_check(session->begin_transaction(session, NULL));
        cursor->set_key(cursor, kname);
        testutil_check(cursor->remove(cursor));
        testutil_check(__wt_snprintf(tscfg, sizeof(tscfg), "commit_timestamp=%" PRIx64, ts + 1));
        testutil_check(session->commit_transaction(session, tscfg));

        /* Set the oldest timestamp to make half of the data obsolete. */
        if (ts - oldest_ts > MAX_DATA) {
            oldest_ts = ts - MAX_DATA / 2;
            testutil_check(
              __wt_snprintf(tscfg, sizeof(tscfg), "oldest_timestamp=%" PRIx64, oldest_ts));
            testutil_check(conn->set_timestamp(conn, tscfg));
        }
    }
    /* NOTREACHED */
}

/*
 * Child process creates the database and table, and then creates the worker thread to add data
 * until it is killed by the parent.
 */
static void run_workload(void) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
static void
run_workload(void)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    wt_thread_t *thr;
    uint32_t i;
    char envconf[512], tableconf[512];

    thr = dcalloc(2, sizeof(*thr));

    if (chdir(home) != 0)
        testutil_die(errno, "Child chdir: %s", home);
    testutil_check(__wt_snprintf(envconf, sizeof(envconf), ENV_CONFIG));

    printf("wiredtiger_open configuration: %s\n", envconf);
    testutil_check(wiredtiger_open(NULL, NULL, envconf, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Create the table. */
    testutil_check(__wt_snprintf(tableconf, sizeof(tableconf),
      "key_format=%s,value_format=u,log=(enabled=false)", use_columns ? "r" : "S"));
    testutil_check(session->create(session, uri, tableconf));
    testutil_check(session->close(session, NULL));

    /* The checkpoint thread is added at the end. */
    printf("Create checkpoint thread\n");
    testutil_check(__wt_thread_create(NULL, &thr[0], thread_ckpt_run, conn));
    printf("Create the worker thread\n");
    testutil_check(__wt_thread_create(NULL, &thr[1], thread_run, conn));

    /* The threads never exit, so the child will just wait here until it is killed. */
    fflush(stdout);
    for (i = 0; i <= 2; ++i)
        testutil_check(__wt_thread_join(NULL, &thr[i]));

    /* NOTREACHED */
    free(thr);
    _exit(EXIT_SUCCESS);
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
    /* The core file will indicate why the child exited. Choose EINVAL here. */
    testutil_die(EINVAL, "Child process %" PRIu64 " abnormally exited", (uint64_t)pid);
}

extern int __wt_optind;
extern char *__wt_optarg;

int
main(int argc, char *argv[])
{
    struct sigaction sa;
    struct stat sb;
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_RAND_STATE rnd;
    WT_SESSION *session;
    pid_t pid;
    uint64_t oldest_ts, stable_ts, ts;
    uint32_t timeout;
    int ch, status;
    char kname[64], statname[1024], tscfg[64];
    char ts_string[WT_TS_HEX_STRING_SIZE];
    const char *working_dir;
    bool fatal, preserve, rand_time;

    (void)testutil_set_progname(argv);

    preserve = false;
    rand_time = true;
    timeout = MIN_TIME;
    working_dir = "WT_TEST.wt6616-checkpoint-oldest-ts";

    while ((ch = __wt_getopt(progname, argc, argv, "ch:pt:")) != EOF)
        switch (ch) {
        case 'c':
            /* Variable-length columns only (for now) */
            use_columns = true;
            break;
        case 'h':
            working_dir = __wt_optarg;
            break;
        case 'p':
            preserve = true;
            break;
        case 't':
            rand_time = false;
            timeout = (uint32_t)atoi(__wt_optarg);
            break;
        default:
            usage();
        }
    argc -= __wt_optind;
    if (argc != 0)
        usage();

    testutil_work_dir_from_path(home, sizeof(home), working_dir);
    testutil_make_work_dir(home);

    __wt_random_init_seed(NULL, &rnd);
    if (rand_time) {
        timeout = __wt_random(&rnd) % MAX_TIME;
        if (timeout < MIN_TIME)
            timeout = MIN_TIME;
    }

    printf("CONFIG: %s -h %s -t %" PRIu32 "\n", progname, working_dir, timeout);
    /*
     * Fork a child to insert and delete as many items. We will then randomly kill the child, run
     * recovery and make sure all items from the oldest to stable timestamps of the checkpoint exist
     * after recovery runs.
     */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    testutil_assert_errno(sigaction(SIGCHLD, &sa, NULL) == 0);
    testutil_assert_errno((pid = fork()) >= 0);

    if (pid == 0) { /* child */
        run_workload();
        /* NOTREACHED */
    }

    /* parent */
    /*
     * Sleep for the configured amount of time before killing the child. Start the timeout from the
     * time we notice that the file has been created. That allows the test to run correctly on
     * really slow machines.
     */
    testutil_check(__wt_snprintf(statname, sizeof(statname), "%s/%s", home, ckpt_file));
    while (stat(statname, &sb) != 0)
        testutil_sleep_wait(1, pid);
    sleep(timeout);
    sa.sa_handler = SIG_DFL;
    testutil_assert_errno(sigaction(SIGCHLD, &sa, NULL) == 0);

    printf("Kill child\n");
    testutil_assert_errno(kill(pid, SIGKILL) == 0);
    testutil_assert_errno(waitpid(pid, &status, 0) != -1);

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

    printf("Open database and run recovery\n");

    /* Open the connection which forces recovery to be run. */
    testutil_check(wiredtiger_open(NULL, NULL, ENV_CONFIG_REC, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Get the stable timestamp from the stable timestamp of the last successful checkpoint. */
    testutil_check(conn->query_timestamp(conn, ts_string, "get=stable"));
    testutil_timestamp_parse(ts_string, &stable_ts);

    /* Get the oldest timestamp from the oldest timestamp of the last successful checkpoint. */
    testutil_check(conn->query_timestamp(conn, ts_string, "get=oldest"));
    testutil_timestamp_parse(ts_string, &oldest_ts);

    printf("Verify data from oldest timestamp %" PRIu64 " to stable timestamp %" PRIu64 "\n",
      oldest_ts, stable_ts);

    /* Open a cursor on the table. */
    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));

    fatal = false;
    for (ts = oldest_ts; ts <= stable_ts; ++ts) {
        testutil_check(__wt_snprintf(tscfg, sizeof(tscfg), "read_timestamp=%" PRIx64, ts));
        testutil_check(session->begin_transaction(session, tscfg));
        testutil_check(__wt_snprintf(kname, sizeof(kname), ROW_KEY_FORMAT, ts));
        if (use_columns)
            cursor->set_key(cursor, ts);
        else
            cursor->set_key(cursor, kname);
        ret = cursor->search(cursor);
        if (ret == WT_NOTFOUND) {
            fatal = true;
            printf("Data corruption detected: missing key: %s\n", kname);
        } else
            testutil_check(ret);
        testutil_check(session->commit_transaction(session, NULL));
    }
    testutil_check(conn->close(conn, NULL));
    if (fatal)
        return (EXIT_FAILURE);
    printf("Verification successful\n");
    if (!preserve) {
        testutil_clean_test_artifacts(home);
        /* At this point $PATH is inside `home`, which we intend to delete. cd to the parent dir. */
        if (chdir("../") != 0)
            testutil_die(errno, "root chdir: %s", home);
        testutil_clean_work_dir(home);
    }
    return (EXIT_SUCCESS);
}
