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

#define TIMEOUT 1

#define NUM_RECORDS 800000

#define ENV_CONFIG_REC "log=(archive=false,recover=on)"
/* Constants and variables declaration. */
/*
 * You may want to add "verbose=[compact,compact_progress]" to the connection config string to get
 * better view on what is happening.
 */
static const char conn_config[] =
  "create,cache_size=1GB,timing_stress_for_test=[compact_slow],statistics=(all),statistics_log=("
  "wait=1,json=true,on_close=true)";
static const char table_config_row[] =
  "allocation_size=4KB,leaf_page_max=4KB,key_format=Q,value_format=QQQS";
static const char table_config_col[] =
  "allocation_size=4KB,leaf_page_max=4KB,key_format=r,value_format=QQQS";
static char data_str[1024] = "";
static const char working_dir_row[] = "WT_TEST.data-correctness-row";
static const char working_dir_col[] = "WT_TEST.data-correctness-col";
static const char compact_file_fmt[] = "%s/compact_started";
static char value_a[] = "AA";
static char value_b[] = "BB";
static char value_c[] = "CC";
static char value_d[] = "DD";

static void sig_handler(int) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

/* Forward declarations. */
static int run_test(bool column_store, const char *uri, bool preserve);
static void populate(WT_SESSION *session, const char *uri);
static void workload_compact(const char *, const char *, const char *uri);
static void remove_records(WT_SESSION *session, const char *uri, int commit_ts);
static void run_compact(WT_SESSION *session, const char *uri);
static void large_updates(WT_SESSION *session, const char *uri, char *value, int commit_ts);
static void check(WT_SESSION *session, const char *uri, char *value, int commit_ts);

/*
 * sig_handler --
 *     Signal handler to catch if the child died unexpectedly.
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
 *     Methods implementation.
 */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));

    testutil_assert(run_test(false, opts->uri, opts->preserve) == EXIT_SUCCESS);

    testutil_assert(run_test(true, opts->uri, opts->preserve) == EXIT_SUCCESS);

    testutil_cleanup(opts);

    return (EXIT_SUCCESS);
}

/*
 * run_compact --
 *     TODO: Add a comment describing this function.
 */
static void
run_compact(WT_SESSION *session, const char *uri)
{
    printf("Compact start...\n");
    /* Perform compact operation. */
    testutil_check(session->compact(session, uri, NULL));
    printf("Compact end...\n");
}

/*
 * run_test --
 *     TODO: Add a comment describing this function.
 */
static int
run_test(bool column_store, const char *uri, bool preserve)
{
    struct sigaction sa;
    struct stat sb;
    WT_CONNECTION *conn;
    WT_SESSION *session;
    pid_t pid;
    uint64_t oldest_ts, stable_ts;
    int status;
    char compact_file[2048];
    char home[1024];
    char ts_string[WT_TS_HEX_STRING_SIZE];

    testutil_work_dir_from_path(
      home, sizeof(home), column_store ? working_dir_col : working_dir_row);

    printf("Work directory: %s\n", home);
    testutil_make_work_dir(home);

    /* Fork a child to create tables and perform operations on them. */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    testutil_assert_errno(sigaction(SIGCHLD, &sa, NULL) == 0);
    testutil_assert_errno((pid = fork()) >= 0);

    if (pid == 0) { /* child */

        workload_compact(home, column_store ? table_config_col : table_config_row, uri);

        /*
         * We do not expect test to reach here. The child process should have been killed by the
         * parent process.
         */
        printf("Child finished processing...\n");
        return (EXIT_FAILURE);
    }

    /* parent */
    /*
     * This test uses timing stress in compact so sleep for the configured amount of time before
     * killing the child. Start the timeout from the time we notice that child process has started
     * compact. That allows the test to run correctly on really slow machines.
     */
    testutil_check(__wt_snprintf(compact_file, sizeof(compact_file), compact_file_fmt, home));
    while (stat(compact_file, &sb) != 0)
        testutil_sleep_wait(1, pid);

    /* Sleep for a while. Let the child process do some operations on the tables. */
    sleep(TIMEOUT);
    sa.sa_handler = SIG_DFL;
    testutil_assert_errno(sigaction(SIGCHLD, &sa, NULL) == 0);

    /* Kill the child process. */
    printf("Kill child\n");
    testutil_assert_errno(kill(pid, SIGKILL) == 0);
    testutil_assert_errno(waitpid(pid, &status, 0) != -1);

    printf("Compact process interrupted and killed...\n");
    printf("Open database and run recovery\n");

    /* Open the connection which forces recovery to be run. */
    testutil_check(wiredtiger_open(home, NULL, ENV_CONFIG_REC, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Get the stable timestamp from the stable timestamp of the last successful checkpoint. */
    testutil_check(conn->query_timestamp(conn, ts_string, "get=stable_timestamp"));
    testutil_timestamp_parse(ts_string, &stable_ts);

    /* Get the oldest timestamp from the oldest timestamp of the last successful checkpoint. */
    testutil_check(conn->query_timestamp(conn, ts_string, "get=oldest_timestamp"));
    testutil_timestamp_parse(ts_string, &oldest_ts);

    /*
     * Verify data is visible and correct after compact operation was killed and RTS is performed in
     * recovery. Stable timestamp is used to check the visibility of data, stable timestamp is
     * retrieved from the connection using query_timestamp.
     */
    check(session, uri, value_a, 20);
    check(session, uri, value_b, 30);
    check(session, uri, value_b, 40);
    check(session, uri, value_b, 50);

    testutil_check(session->checkpoint(session, NULL));

    testutil_check(session->close(session, NULL));
    session = NULL;

    testutil_check(conn->close(conn, NULL));
    conn = NULL;

    /* Cleanup */
    if (!preserve) {
        testutil_clean_work_dir(home);
        testutil_clean_test_artifacts(home);
    }

    return (EXIT_SUCCESS);
}

/*
 * workload_compact --
 *     TODO: Add a comment describing this function.
 */
static void
workload_compact(const char *home, const char *table_config, const char *uri)
{
    FILE *fp;
    WT_CONNECTION *conn;
    WT_SESSION *session;
    char compact_file[2048], tscfg[64];

    testutil_check(wiredtiger_open(home, NULL, conn_config, &conn));

    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    testutil_check(__wt_snprintf(tscfg, sizeof(tscfg),
      "oldest_timestamp=%d"
      ",stable_timestamp=%d",
      10, 10));
    testutil_check(conn->set_timestamp(conn, tscfg));

    /* Create and populate table. Checkpoint the data after that. */
    testutil_check(session->create(session, uri, table_config));

    populate(session, uri);
    testutil_check(session->checkpoint(session, NULL));

    /* Perform several updates. */
    large_updates(session, uri, value_a, 20);
    large_updates(session, uri, value_b, 30);
    large_updates(session, uri, value_c, 40);
    large_updates(session, uri, value_d, 50);

    /* Verify data is visible and correct. */
    check(session, uri, value_a, 20);
    check(session, uri, value_b, 30);
    check(session, uri, value_c, 40);
    check(session, uri, value_d, 50);

    /* Pin stable to timestamp 30. */
    testutil_check(__wt_snprintf(tscfg, sizeof(tscfg), "stable_timestamp=%d", 30));
    testutil_check(conn->set_timestamp(conn, tscfg));

    /*
     * Remove 1/3 of data from the middle of the key range to let compact relocate blocks from the
     * end of the file. Checkpoint the changes after the removal.
     */
    remove_records(session, uri, 60);

    /*
     * Force checkpoint is the first step in the compact operation, we do the same thing here to
     * save some time in the compact operation.
     */
    testutil_check(session->checkpoint(session, "force"));

    /*
     * Create the compact_started file so that the parent process can start its timer.
     */
    testutil_check(__wt_snprintf(compact_file, sizeof(compact_file), compact_file_fmt, home));
    testutil_assert_errno((fp = fopen(compact_file, "w")) != NULL);
    testutil_assert_errno(fclose(fp) == 0);

    run_compact(session, uri);
}

/*
 * check --
 *     TODO: Add a comment describing this function.
 */
static void
check(WT_SESSION *session, const char *uri, char *value, int read_ts)
{
    WT_CURSOR *cursor;
    size_t val_1_size, val_2_size;
    uint64_t val1, val2, val3;
    int i;
    char *str_val;
    char tscfg[64];

    printf("Checking value : %s...\n", value);

    testutil_check(__wt_snprintf(tscfg, sizeof(tscfg), "read_timestamp=%d", read_ts));
    testutil_check(session->begin_transaction(session, tscfg));

    val_1_size = strlen(value);
    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));

    for (i = 0; i < NUM_RECORDS; i++) {
        cursor->set_key(cursor, i + 1);
        testutil_check(cursor->search(cursor));
        testutil_check(cursor->get_value(cursor, &val1, &val2, &val3, &str_val));

        val_2_size = strlen(str_val);

        testutil_assert(val_1_size == val_2_size);
        testutil_assert(memcmp(value, str_val, val_1_size) == 0);
    }

    testutil_check(session->commit_transaction(session, NULL));
    testutil_check(cursor->close(cursor));
    cursor = NULL;
}

/*
 * large_updates --
 *     TODO: Add a comment describing this function.
 */
static void
large_updates(WT_SESSION *session, const char *uri, char *value, int commit_ts)
{
    WT_CURSOR *cursor;
    WT_RAND_STATE rnd;
    uint64_t val;
    int i;
    char tscfg[64];

    __wt_random_init_seed((WT_SESSION_IMPL *)session, &rnd);
    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));

    testutil_check(__wt_snprintf(tscfg, sizeof(tscfg), "commit_timestamp=%d", commit_ts));
    for (i = 0; i < NUM_RECORDS; i++) {
        testutil_check(session->begin_transaction(session, NULL));
        cursor->set_key(cursor, i + 1);
        val = (uint64_t)__wt_random(&rnd);
        cursor->set_value(cursor, val, val, val, value);
        testutil_check(cursor->insert(cursor));
        testutil_check(session->commit_transaction(session, tscfg));
    }

    testutil_check(cursor->close(cursor));
    cursor = NULL;
}

/*
 * populate --
 *     TODO: Add a comment describing this function.
 */
static void
populate(WT_SESSION *session, const char *uri)
{
    WT_CURSOR *cursor;
    WT_RAND_STATE rnd;
    uint64_t val;
    int i, str_len;

    printf("Populating table...\n");

    __wt_random_init_seed((WT_SESSION_IMPL *)session, &rnd);

    str_len = sizeof(data_str) / sizeof(data_str[0]);
    for (i = 0; i < str_len - 1; i++)
        data_str[i] = 'a' + __wt_random(&rnd) % 26;

    data_str[str_len - 1] = '\0';

    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));
    for (i = 0; i < NUM_RECORDS; i++) {
        cursor->set_key(cursor, i + 1);
        val = (uint64_t)__wt_random(&rnd);
        cursor->set_value(cursor, val, val, val, data_str);
        testutil_check(cursor->insert(cursor));
    }

    testutil_check(cursor->close(cursor));
    cursor = NULL;
}

/*
 * remove_records --
 *     TODO: Add a comment describing this function.
 */
static void
remove_records(WT_SESSION *session, const char *uri, int commit_ts)
{
    WT_CURSOR *cursor;
    int i;
    char tscfg[64];

    printf("Removing records...\n");
    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));

    testutil_check(__wt_snprintf(tscfg, sizeof(tscfg), "commit_timestamp=%d", commit_ts));
    /* Remove 1/3 of the records from the middle of the key range. */
    for (i = NUM_RECORDS / 3; i < (NUM_RECORDS * 2) / 2; i++) {
        testutil_check(session->begin_transaction(session, NULL));
        cursor->set_key(cursor, i);
        testutil_check(cursor->remove(cursor));
        testutil_check(session->commit_transaction(session, tscfg));
    }

    testutil_check(cursor->close(cursor));
    cursor = NULL;
}
