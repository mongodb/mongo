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

#define MAX_RETRIES 5
#define TIMEOUT 1
/* Have enough records to give us time to interrupt compaction. */
#define NUM_RECORDS (800 * WT_THOUSAND)

static const char compact_file[] = "compact_started";
/*
 * You may want to add "verbose=[compact,compact_progress]" to the connection config string to get
 * better view on what is happening.
 */
static const char conn_config[] =
  "create,cache_size=1GB,timing_stress_for_test=[compact_slow],statistics=(all),statistics_log=("
  "json,on_close,wait=1),debug_mode=(background_compact)";
static char data_str[1024] = "";
static const char table_config_row[] =
  "allocation_size=4KB,leaf_page_max=4KB,key_format=Q,value_format=QQQS";
static const char table_config_col[] =
  "allocation_size=4KB,leaf_page_max=4KB,key_format=r,value_format=QQQS";
static char value_a[] = "AA";
static char value_b[] = "BB";
static char value_c[] = "CC";
static char value_d[] = "DD";
static const char working_dir_row[] = "WT_TEST.data-correctness-row";
static const char working_dir_col[] = "WT_TEST.data-correctness-col";

static void check(WT_SESSION *session, const char *uri, char *value, int commit_ts);
static void large_updates(WT_SESSION *session, const char *uri, char *value, int commit_ts);
static void populate(WT_SESSION *session, const char *uri);
static void remove_records(WT_SESSION *session, const char *uri, int commit_ts);
static int run_test(bool column_store, bool background_compact, const char *uri, bool preserve);
static void sig_handler(int) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
static void workload_compact(const char *, bool, const char *, const char *uri);

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
    /* The core file will indicate why the child exited, choose EINVAL here. */
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
    int i, j;

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));

    for (i = 0; i < 2; ++i) {
        for (j = 0; j < 2; ++j)
            /*
             * Use i and j to alternate between column/row store and foreground/background
             * compaction scenarios.
             */
            testutil_assert(run_test(i, j, opts->uri, opts->preserve) == EXIT_SUCCESS);
    }

    testutil_cleanup(opts);

    return (EXIT_SUCCESS);
}

/*
 * run_test --
 *     Child: starts compaction. Parent: kills the child as soon as the child has started compaction
 *     and verifies the unclean database.
 */
static int
run_test(bool column_store, bool background_compact, const char *uri, bool preserve)
{
    struct sigaction sa;
    WT_CONNECTION *conn;
    WT_SESSION *session;
    pid_t pid;
    int status;
    char home[1024];

    testutil_work_dir_from_path(
      home, sizeof(home), column_store ? working_dir_col : working_dir_row);

    printf("%s store, %s compaction, work directory: %s.\n", column_store ? "Column" : "Row",
      background_compact ? "background" : "foreground", home);
    testutil_recreate_dir(home);

    /* Fork a child to create tables and perform operations on them. */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    testutil_assert_errno(sigaction(SIGCHLD, &sa, NULL) == 0);
    testutil_assert_errno((pid = fork()) >= 0);

    /* Child. */
    if (pid == 0) {

        workload_compact(
          home, background_compact, column_store ? table_config_col : table_config_row, uri);

        printf("Child finished processing...\n");
        /*
         * We do not expect test to reach here. The child process should have been killed by the
         * parent process.
         */
        return (EXIT_FAILURE);
    }

    /* Parent */
    /*
     * This test uses timing stress in compact so sleep for the configured amount of time before
     * killing the child. Start the timeout from the time we notice that child process has started
     * compact. That allows the test to run correctly on really slow machines.
     */
    while (!testutil_exists(home, compact_file))
        testutil_sleep_wait(TIMEOUT, pid);

    /* Sleep for a specific time to give compaction a chance to do some work. */
    sleep(TIMEOUT);
    sa.sa_handler = SIG_DFL;
    testutil_assert_errno(sigaction(SIGCHLD, &sa, NULL) == 0);

    /* Kill the child process. */
    printf("Kill child\n");
    testutil_assert_errno(kill(pid, SIGKILL) == 0);
    testutil_assert_errno(waitpid(pid, &status, 0) != -1);
    printf("Compact process interrupted and killed...\n");

    /* Open the connection which forces recovery to be run. */
    printf("Open database and run recovery\n");
    testutil_check(wiredtiger_open(home, NULL, TESTUTIL_ENV_CONFIG_REC, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /*
     * Verify data is visible and correct after compact operation was killed and RTS is performed in
     * recovery. Stable timestamp is used to check the visibility of data, stable timestamp is
     * retrieved from the connection using query_timestamp.
     */
    check(session, uri, value_a, 20);
    check(session, uri, value_b, 30);
    check(session, uri, value_b, 40);
    check(session, uri, value_b, 50);

    testutil_check(session->close(session, NULL));
    session = NULL;

    testutil_check(conn->close(conn, NULL));
    conn = NULL;

    /* Cleanup */
    if (!preserve)
        testutil_remove(home);

    return (EXIT_SUCCESS);
}

/*
 * workload_compact --
 *     Create a table with content that can be compacted. Create a sentinel file when compaction is
 *     about to start.
 */
static void
workload_compact(
  const char *home, bool background_compact, const char *table_config, const char *uri)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    char tscfg[64], compact_cfg[512];

    testutil_check(wiredtiger_open(home, NULL, conn_config, &conn));

    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    testutil_snprintf(tscfg, sizeof(tscfg), "oldest_timestamp=%d,stable_timestamp=%d", 10, 10);
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
    testutil_snprintf(tscfg, sizeof(tscfg), "stable_timestamp=%d", 30);
    testutil_check(conn->set_timestamp(conn, tscfg));

    /* Remove records to give compaction some work to do. */
    remove_records(session, uri, 60);

    /*
     * Checkpoint is the first step in the compact operation, we do the same thing here to save some
     time in the compact operation.
     */
    testutil_check(session->checkpoint(session, NULL));

    /* Set a low threshold to ensure compaction runs. */
    testutil_snprintf(compact_cfg, sizeof(compact_cfg), "free_space_target=1MB%s",
      background_compact ? ",background=true" : "");

    /*
     * Because foreground and background compaction behave differently, we don't create the sentinel
     * file at the same time for each scenario. In the foreground compaction scenario, the compact
     * API returns once compaction is done, therefore we need to create the sentinel file before
     * compacting the file. On the other hand, when enabling background compaction, the API returns
     * straight away so we can create the sentinel file once the service has been enabled but we
     * have to wait for some time to make sure compaction started.
     */
    if (!background_compact)
        testutil_sentinel(home, compact_file);

    printf(
      "%s starting...\n", background_compact ? "Background compaction" : "Foreground compaction");
    testutil_check(session->compact(session, background_compact ? NULL : uri, compact_cfg));
    if (background_compact) {
        testutil_sentinel(home, compact_file);
        /* Give some time for compaction to start. */
        sleep(5 * TIMEOUT);
    } else
        printf("Foreground compaction ended...\n");
}

/*
 * check --
 *     TODO: Add a comment describing this function.
 */
static void
check(WT_SESSION *session, const char *uri, char *value, int read_ts)
{
    WT_CURSOR *cursor;
    size_t val_1_size;
    uint64_t val1, val2, val3;
    int i;
    char *str_val;
    char tscfg[64];

    printf("Checking value : %s...\n", value);

    testutil_snprintf(tscfg, sizeof(tscfg), "read_timestamp=%d", read_ts);
    testutil_check(session->begin_transaction(session, tscfg));

    val_1_size = strlen(value);
    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));

    for (i = 0; i < NUM_RECORDS; i++) {
        cursor->set_key(cursor, i + 1);
        testutil_check(cursor->search(cursor));
        testutil_check(cursor->get_value(cursor, &val1, &val2, &val3, &str_val));
        testutil_assert(val_1_size == strlen(str_val));
        testutil_assert(memcmp(value, str_val, val_1_size) == 0);
    }

    testutil_check(session->commit_transaction(session, NULL));
    testutil_check(cursor->close(cursor));
    cursor = NULL;
}

/*
 * large_updates --
 *     Update all the records with a given value at a specific timestamp.
 */
static void
large_updates(WT_SESSION *session, const char *uri, char *value, int commit_ts)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_RAND_STATE rnd;
    uint64_t val;
    int i, retry_attempts;
    char tscfg[64];

    retry_attempts = 0;
    __wt_random_init((WT_SESSION_IMPL *)session, &rnd);
    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));

    testutil_snprintf(tscfg, sizeof(tscfg), "commit_timestamp=%d", commit_ts);
    for (i = 0; i < NUM_RECORDS; i++) {
        testutil_check(session->begin_transaction(session, NULL));

        cursor->set_key(cursor, i + 1);
        val = (uint64_t)__wt_random(&rnd);
        cursor->set_value(cursor, val, val, val, value);

        while (((ret = cursor->insert(cursor)) == WT_ROLLBACK) && retry_attempts++ < MAX_RETRIES) {
            printf("Rollback transaction for key %d\n", i);
            testutil_check(session->rollback_transaction(session, NULL));
            testutil_check(session->begin_transaction(session, NULL));
        }

        testutil_assertfmt(retry_attempts < MAX_RETRIES,
          "Cursor insert returned WT_ROLLBACK for %d times", retry_attempts);

        testutil_check(ret);
        testutil_check(session->commit_transaction(session, tscfg));
    }

    testutil_check(cursor->close(cursor));
    cursor = NULL;
}

/*
 * populate --
 *     Populate a table with random strings.
 */
static void
populate(WT_SESSION *session, const char *uri)
{
    WT_CURSOR *cursor;
    WT_RAND_STATE rnd;
    uint64_t val;
    int i, str_len;

    printf("Populating table...\n");

    __wt_random_init((WT_SESSION_IMPL *)session, &rnd);

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
 *     Remove a third of the records from the start of the key range.
 */
static void
remove_records(WT_SESSION *session, const char *uri, int commit_ts)
{
    WT_CURSOR *cursor;
    int i;
    char tscfg[64];

    printf("Removing records...\n");
    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));

    testutil_snprintf(tscfg, sizeof(tscfg), "commit_timestamp=%d", commit_ts);
    for (i = 1; i <= NUM_RECORDS / 3; i++) {
        testutil_check(session->begin_transaction(session, NULL));
        cursor->set_key(cursor, i);
        testutil_check(cursor->remove(cursor));
        testutil_check(session->commit_transaction(session, tscfg));
    }

    testutil_check(cursor->close(cursor));
    cursor = NULL;
}
