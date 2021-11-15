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

/*
 * This test verifies that there are no data inconsistencies if compact operation is interrupted by
 * an unclean shutdown. To achieve this, the main process spawns a child process. The child process
 * performs certain operations on two identical tables. The parent process randomly kills the child
 * process and verifies that the data across two tables match after restart.
 */

#define NUM_RECORDS 100000
#define TIMEOUT 40

/* Constants and variables declaration. */
/*
 * You may want to add "verbose=[compact,compact_progress]" to the connection config string to get
 * better view on what is happening.
 */
static const char conn_config[] = "create,cache_size=2GB,statistics=(all)";
static const char table_config_row[] =
  "allocation_size=4KB,leaf_page_max=4KB,key_format=Q,value_format=" WT_UNCHECKED_STRING(QS);
static const char table_config_col[] =
  "allocation_size=4KB,leaf_page_max=4KB,key_format=r,value_format=" WT_UNCHECKED_STRING(QS);
static char data_str[1024] = "";

static const char ckpt_file_fmt[] = "%s/checkpoint_done";
static const char working_dir_row[] = "WT_TEST.compact-stress-row";
static const char working_dir_col[] = "WT_TEST.compact-stress-col";
static const char uri1[] = "table:compact1";
static const char uri2[] = "table:compact2";

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
    fprintf(stderr, "%s", message);
    return (0);
}

static WT_EVENT_HANDLER event_handler = {
  subtest_error_handler, NULL, /* Message handler */
  NULL,                        /* Progress handler */
  NULL                         /* Close handler */
};

static void sig_handler(int) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

/* Forward declarations. */
static void run_test(bool, bool);
static void workload_compact(const char *, const char *);
static void populate(WT_SESSION *, uint64_t, uint64_t);
static void remove_records(WT_SESSION *, const char *, uint64_t, uint64_t);
static void verify_tables(WT_SESSION *);
static int verify_tables_helper(WT_SESSION *, const char *, const char *);
static void get_file_stats(WT_SESSION *, const char *, uint64_t *, uint64_t *);
static void log_db_size(WT_SESSION *, const char *);
static void get_compact_progress(
  WT_SESSION *session, const char *, uint64_t *, uint64_t *, uint64_t *);

/*
 * Signal handler to catch if the child died unexpectedly.
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

/* Methods implementation. */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));

    run_test(false, opts->preserve);

    run_test(true, opts->preserve);

    testutil_cleanup(opts);

    return (EXIT_SUCCESS);
}

static void
run_test(bool column_store, bool preserve)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;

    char ckpt_file[2048], home[1024];
    int status;
    pid_t pid;
    struct sigaction sa;
    struct stat sb;

    testutil_work_dir_from_path(
      home, sizeof(home), column_store ? working_dir_col : working_dir_row);

    printf("\n");
    printf("Work directory: %s\n", home);
    testutil_make_work_dir(home);

    /* Fork a child to create tables and perform operations on them. */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    testutil_checksys(sigaction(SIGCHLD, &sa, NULL));
    testutil_checksys((pid = fork()) < 0);

    if (pid == 0) { /* child */

        workload_compact(home, column_store ? table_config_col : table_config_row);
        /*
         * We do not expect test to reach here. The child process should have been killed by the
         * parent process.
         */
        printf("Child finished processing...\n");
        _exit(EXIT_FAILURE);
    }

    /* parent */
    /*
     * Sleep for the configured amount of time before killing the child. Start the timeout from the
     * time we notice that child process has written a checkpoint. That allows the test to run
     * correctly on really slow machines.
     */
    testutil_check(__wt_snprintf(ckpt_file, sizeof(ckpt_file), ckpt_file_fmt, home));
    while (stat(ckpt_file, &sb) != 0)
        testutil_sleep_wait(1, pid);

    /* Sleep for a while. Let the child process do some operations on the tables. */
    sleep(TIMEOUT);
    sa.sa_handler = SIG_DFL;
    testutil_checksys(sigaction(SIGCHLD, &sa, NULL));

    /* Kill the child process. */
    printf("Kill child\n");
    testutil_checksys(kill(pid, SIGKILL) != 0);
    testutil_checksys(waitpid(pid, &status, 0) == -1);

    /* Reopen the connection and verify that the tables match each other. */
    testutil_check(wiredtiger_open(home, &event_handler, conn_config, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    verify_tables(session);

    /* Clean-up. */
    testutil_check(session->close(session, NULL));
    session = NULL;

    testutil_check(conn->close(conn, NULL));
    conn = NULL;

    if (!preserve)
        testutil_clean_work_dir(home);
}

static void
workload_compact(const char *home, const char *table_config)
{
    FILE *fp;
    WT_CONNECTION *conn;
    WT_RAND_STATE rnd;
    WT_SESSION *session;

    bool first_ckpt;
    char ckpt_file[2048];
    uint32_t i;
    uint64_t key_range_start;

    uint64_t pages_reviewed, pages_rewritten, pages_skipped;

    first_ckpt = false;

    testutil_check(wiredtiger_open(home, &event_handler, conn_config, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    __wt_random_init_seed((WT_SESSION_IMPL *)session, &rnd);

    /* Create and populate table. Checkpoint the data after that. */
    testutil_check(session->create(session, uri1, table_config));
    testutil_check(session->create(session, uri2, table_config));

    populate(session, 0, NUM_RECORDS);

    /*
     * Although we are repeating the steps 40 times, we expect parent process will kill us way
     * before than that.
     */
    for (i = 0; i < 40; i++) {

        printf("Running Loop: %" PRIu32 "\n", i + 1);
        testutil_check(session->checkpoint(session, NULL));
        /*
         * Create the checkpoint file so that the parent process knows at least one checkpoint has
         * finished and can start its timer.
         */
        if (!first_ckpt) {
            testutil_check(__wt_snprintf(ckpt_file, sizeof(ckpt_file), ckpt_file_fmt, home));
            testutil_checksys((fp = fopen(ckpt_file, "w")) == NULL);
            testutil_checksys(fclose(fp) != 0);
            first_ckpt = true;
        }

        /*
         * Remove 1/3 of data from the middle of the key range to let compact relocate blocks from
         * the end of the file.
         */
        key_range_start = (uint64_t)__wt_random(&rnd) % (((NUM_RECORDS * 2) / 3) - 1);
        remove_records(session, uri1, key_range_start, key_range_start + NUM_RECORDS / 3);
        remove_records(session, uri2, key_range_start, key_range_start + NUM_RECORDS / 3);

        /* Only perform compaction on the first table. */
        testutil_check(session->compact(session, uri1, NULL));

        log_db_size(session, uri1);

        /* If we made progress with compact, verify that compact stats support that. */
        get_compact_progress(session, uri1, &pages_reviewed, &pages_rewritten, &pages_skipped);
        printf(" - Pages reviewed: %" PRIu64 "\n", pages_reviewed);
        printf(" - Pages selected for being rewritten: %" PRIu64 "\n", pages_rewritten);
        printf(" - Pages skipped: %" PRIu64 "\n", pages_skipped);

        /* Put the deleted records back. */
        populate(session, key_range_start, key_range_start + NUM_RECORDS / 3);

        sleep(1);
    }

    /* Clean-up. */
    testutil_check(conn->close(conn, NULL));
}

static void
populate(WT_SESSION *session, uint64_t start, uint64_t end)
{
    WT_CURSOR *cursor_1;
    WT_CURSOR *cursor_2;
    WT_RAND_STATE rnd;

    uint64_t i, str_len, val;

    __wt_random_init_seed((WT_SESSION_IMPL *)session, &rnd);

    str_len = sizeof(data_str) / sizeof(data_str[0]);
    for (i = 0; i < str_len - 1; i++)
        data_str[i] = 'a' + (uint32_t)__wt_random(&rnd) % 26;

    data_str[str_len - 1] = '\0';

    testutil_check(session->open_cursor(session, uri1, NULL, NULL, &cursor_1));
    testutil_check(session->open_cursor(session, uri2, NULL, NULL, &cursor_2));
    for (i = start; i < end; i++) {
        val = (uint64_t)__wt_random(&rnd);
        cursor_1->set_key(cursor_1, i + 1);
        cursor_1->set_value(cursor_1, val, data_str);
        testutil_check(cursor_1->insert(cursor_1));

        cursor_2->set_key(cursor_2, i + 1);
        cursor_2->set_value(cursor_2, val, data_str);
        testutil_check(cursor_2->insert(cursor_2));
    }

    testutil_check(cursor_1->close(cursor_1));
    testutil_check(cursor_2->close(cursor_2));
}

static void
remove_records(WT_SESSION *session, const char *uri, uint64_t start, uint64_t end)
{
    WT_CURSOR *cursor;
    uint64_t i;

    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));

    for (i = start; i < end; i++) {
        cursor->set_key(cursor, i + 1);
        testutil_check(cursor->remove(cursor));
    }

    testutil_check(cursor->close(cursor));
}

static int
verify_tables_helper(WT_SESSION *session, const char *table1, const char *table2)
{
    WT_CURSOR *cursor_1, *cursor_2;

    char *str_val_1, *str_val_2;
    int ret, total_keys;
    size_t val_1_size, val_2_size;
    uint64_t key, val_1, val_2;

    testutil_check(session->open_cursor(session, table1, NULL, NULL, &cursor_1));
    testutil_check(session->open_cursor(session, table2, NULL, NULL, &cursor_2));

    /* Run over all keys in first table and verify they are present in the second table. */
    total_keys = 0;
    while ((ret = cursor_1->next(cursor_1)) == 0) {
        cursor_1->get_key(cursor_1, &key);
        cursor_2->set_key(cursor_2, key);
        testutil_check(cursor_2->search(cursor_2));
        testutil_check(cursor_1->get_value(cursor_1, &val_1, &str_val_1));
        testutil_check(cursor_2->get_value(cursor_2, &val_2, &str_val_2));

        val_1_size = strlen(str_val_1);
        val_2_size = strlen(str_val_2);

        testutil_assert(val_1 == val_2);
        testutil_assert(val_1_size == val_2_size);
        testutil_assert(memcmp(str_val_1, str_val_2, val_1_size) == 0);

        ++total_keys;
    }
    testutil_assert(ret == WT_NOTFOUND);

    testutil_check(cursor_1->close(cursor_1));
    testutil_check(cursor_2->close(cursor_2));

    /* Return the number of keys verified. */
    return (total_keys);
}

static void
verify_tables(WT_SESSION *session)
{
    int total_keys_1, total_keys_2;

    /*
     * Run over all keys in first table and verify they are present in the second table. Repeat with
     * all keys from the second table and verify that they are present in the first table;
     */

    total_keys_1 = verify_tables_helper(session, uri1, uri2);
    total_keys_2 = verify_tables_helper(session, uri2, uri1);

    testutil_assert(total_keys_2 == total_keys_1);
    printf("%i Keys verified from the two tables. \n", total_keys_1);
}

static void
get_file_stats(WT_SESSION *session, const char *uri, uint64_t *file_sz, uint64_t *avail_bytes)
{
    WT_CURSOR *cur_stat;
    char *descr, stat_uri[128], *str_val;

    testutil_check(__wt_snprintf(stat_uri, sizeof(stat_uri), "statistics:%s", uri));
    testutil_check(session->open_cursor(session, stat_uri, NULL, "statistics=(all)", &cur_stat));

    /* Get file size. */
    cur_stat->set_key(cur_stat, WT_STAT_DSRC_BLOCK_SIZE);
    testutil_check(cur_stat->search(cur_stat));
    testutil_check(cur_stat->get_value(cur_stat, &descr, &str_val, file_sz));

    /* Get bytes available for reuse. */
    cur_stat->set_key(cur_stat, WT_STAT_DSRC_BLOCK_REUSE_BYTES);
    testutil_check(cur_stat->search(cur_stat));
    testutil_check(cur_stat->get_value(cur_stat, &descr, &str_val, avail_bytes));

    testutil_check(cur_stat->close(cur_stat));
    cur_stat = NULL;
}

static void
log_db_size(WT_SESSION *session, const char *uri)
{
    uint64_t avail_bytes, available_pct, file_sz;

    get_file_stats(session, uri, &file_sz, &avail_bytes);

    /* Check if there's maximum of 10% space available after compaction. */
    available_pct = (avail_bytes * 100) / file_sz;
    printf(" - Compacted file size: %" PRIu64 "MB (%" PRIu64 "B)\n - Available for reuse: %" PRIu64
           "MB (%" PRIu64 "B)\n - %" PRIu64 "%% space available in the file.\n",
      file_sz / WT_MEGABYTE, file_sz, avail_bytes / WT_MEGABYTE, avail_bytes, available_pct);
}

static void
get_compact_progress(WT_SESSION *session, const char *uri, uint64_t *pages_reviewed,
  uint64_t *pages_skipped, uint64_t *pages_rewritten)
{

    WT_CURSOR *cur_stat;
    char *descr, *str_val, stat_uri[128];

    testutil_check(__wt_snprintf(stat_uri, sizeof(stat_uri), "statistics:%s", uri));
    testutil_check(session->open_cursor(session, stat_uri, NULL, "statistics=(all)", &cur_stat));

    cur_stat->set_key(cur_stat, WT_STAT_DSRC_BTREE_COMPACT_PAGES_REVIEWED);
    testutil_check(cur_stat->search(cur_stat));
    testutil_check(cur_stat->get_value(cur_stat, &descr, &str_val, pages_reviewed));
    cur_stat->set_key(cur_stat, WT_STAT_DSRC_BTREE_COMPACT_PAGES_SKIPPED);
    testutil_check(cur_stat->search(cur_stat));
    testutil_check(cur_stat->get_value(cur_stat, &descr, &str_val, pages_skipped));
    cur_stat->set_key(cur_stat, WT_STAT_DSRC_BTREE_COMPACT_PAGES_REWRITTEN);
    testutil_check(cur_stat->search(cur_stat));
    testutil_check(cur_stat->get_value(cur_stat, &descr, &str_val, pages_rewritten));

    testutil_check(cur_stat->close(cur_stat));
}
