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

/*
 * This test executes two test cases:
 * - One with WT_TIMING_STRESS_CHECKPOINT_SLOW flag. It adds 10 seconds sleep before each
 * checkpoint.
 * - Another test case synchronizes compact and checkpoint threads using a condition variable.
 * The reason we have two tests here is that they give different output when configured
 * with "verbose=[compact,compact_progress]". There's a chance these two cases are different.
 */

#define NUM_RECORDS 1000000
#define CHECKPOINT_NUM 3

/* Constants and variables declaration. */
/*
 * You may want to add "verbose=[compact,compact_progress]" to the connection config string to get
 * better view on what is happening.
 */
static const char conn_config[] = "create,cache_size=2GB,statistics=(all)";
static const char table_config_row[] =
  "allocation_size=4KB,leaf_page_max=4KB,key_format=Q,value_format=QQQS";
static const char table_config_col[] =
  "allocation_size=4KB,leaf_page_max=4KB,key_format=r,value_format=QQQS";
static char data_str[1024] = "";
static pthread_t thread_compact;

/* Structures definition. */
struct thread_data {
    WT_CONNECTION *conn;
    const char *uri;
    WT_CONDVAR *cond;
};

/* Forward declarations. */
static void run_test_clean(bool, bool, bool, const char *, const char *, const char *ri);
static void run_test(bool, bool, const char *, const char *);
static void *thread_func_compact(void *);
static void *thread_func_checkpoint(void *);
static void populate(WT_SESSION *, const char *);
static void remove_records(WT_SESSION *, const char *);
static void get_file_stats(WT_SESSION *, const char *, uint64_t *, uint64_t *);
static void set_timing_stress_checkpoint(WT_CONNECTION *);
static bool check_db_size(WT_SESSION *, const char *);
static void get_compact_progress(
  WT_SESSION *session, const char *, uint64_t *, uint64_t *, uint64_t *);

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

    /*
     * First, run test with WT_TIMING_STRESS_CHECKPOINT_SLOW. Row store case.
     */
    run_test_clean(true, false, opts->preserve, opts->home, "SR", opts->uri);

    /*
     * Now, run test where compact and checkpoint threads are synchronized using condition variable.
     * Row store case.
     */
    run_test_clean(false, false, opts->preserve, opts->home, "NR", opts->uri);

    /*
     * Next, run test with WT_TIMING_STRESS_CHECKPOINT_SLOW. Column store case.
     */
    run_test_clean(true, true, opts->preserve, opts->home, "SC", opts->uri);

    /*
     * Finally, run test where compact and checkpoint threads are synchronized using condition
     * variable. Column store case.
     */
    run_test_clean(false, true, opts->preserve, opts->home, "NC", opts->uri);

    testutil_cleanup(opts);

    return (EXIT_SUCCESS);
}

/*
 * run_test_clean --
 *     TODO: Add a comment describing this function.
 */
static void
run_test_clean(bool stress_test, bool column_store, bool preserve, const char *home,
  const char *suffix, const char *uri)
{
    char home_full[512];

    printf("\n");
    printf("Running %s test with %s store...\n", stress_test ? "stress" : "normal",
      column_store ? "column" : "row");
    testutil_assert(sizeof(home_full) > strlen(home) + strlen(suffix) + 2);
    sprintf(home_full, "%s.%s", home, suffix);
    run_test(stress_test, column_store, home_full, uri);

    /* Cleanup */
    if (!preserve)
        testutil_clean_work_dir(home_full);
}

/*
 * run_test --
 *     TODO: Add a comment describing this function.
 */
static void
run_test(bool stress_test, bool column_store, const char *home, const char *uri)
{
    struct thread_data td;
    WT_CONNECTION *conn;
    WT_SESSION *session;
    pthread_t thread_checkpoint;
    uint64_t pages_reviewed, pages_rewritten, pages_skipped;
    bool size_check_res;

    testutil_make_work_dir(home);
    testutil_check(wiredtiger_open(home, NULL, conn_config, &conn));

    if (stress_test) {
        /*
         * Set WT_TIMING_STRESS_CHECKPOINT_SLOW flag for stress test. It adds 10 seconds sleep
         * before each checkpoint.
         */
        set_timing_stress_checkpoint(conn);
    }

    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Create and populate table. Checkpoint the data after that. */
    testutil_check(
      session->create(session, uri, column_store ? table_config_col : table_config_row));

    populate(session, uri);
    testutil_check(session->checkpoint(session, NULL));

    /*
     * Remove 1/3 of data from the middle of the key range to let compact relocate blocks from the
     * end of the file.
     */
    remove_records(session, uri);

    td.conn = conn;
    td.uri = uri;
    td.cond = NULL;

    /* Spawn checkpoint and compact threads. Order is important! */
    if (stress_test) {
        testutil_check(pthread_create(&thread_compact, NULL, thread_func_compact, &td));
        testutil_check(pthread_create(&thread_checkpoint, NULL, thread_func_checkpoint, &td));
    } else {
        /* Create and initialize conditional variable. */
        testutil_check(__wt_cond_alloc((WT_SESSION_IMPL *)session, "compact operation", &td.cond));

        /* The checkpoint thread will spawn the compact thread when it's ready. */
        testutil_check(pthread_create(&thread_checkpoint, NULL, thread_func_checkpoint, &td));
    }

    /* Wait for the threads to finish the work. */
    (void)pthread_join(thread_checkpoint, NULL);
    (void)pthread_join(thread_compact, NULL);

    /* Collect compact progress stats. */
    get_compact_progress(session, uri, &pages_reviewed, &pages_skipped, &pages_rewritten);
    size_check_res = check_db_size(session, uri);

    /* Cleanup */
    if (!stress_test) {
        __wt_cond_destroy((WT_SESSION_IMPL *)session, &td.cond);
        td.cond = NULL;
    }

    testutil_check(session->close(session, NULL));
    session = NULL;

    testutil_check(conn->close(conn, NULL));
    conn = NULL;

    printf(" - Pages reviewed: %" PRIu64 "\n", pages_reviewed);
    printf(" - Pages selected for being rewritten: %" PRIu64 "\n", pages_rewritten);
    printf(" - Pages skipped: %" PRIu64 "\n", pages_skipped);
    testutil_assert(pages_reviewed > 0);
    testutil_assert(pages_rewritten > 0);
    /*
     * Check if there's more than 10% available space in the file. Checking result here to allow
     * connection to close properly.
     */
    testutil_assert(size_check_res);
}

/*
 * thread_func_compact --
 *     TODO: Add a comment describing this function.
 */
static void *
thread_func_compact(void *arg)
{
    struct thread_data *td;
    WT_SESSION *session;

    td = (struct thread_data *)arg;

    testutil_check(td->conn->open_session(td->conn, NULL, NULL, &session));

    if (td->cond != NULL) {
        /* Wake up the checkpoint thread. */
        printf("Sending the signal!\n");
        __wt_cond_signal((WT_SESSION_IMPL *)session, td->cond);
    }

    /* Perform compact operation. */
    testutil_check(session->compact(session, td->uri, NULL));

    testutil_check(session->close(session, NULL));
    session = NULL;

    return (NULL);
}

/*
 * wait_run_check --
 *     TODO: Add a comment describing this function.
 */
static bool
wait_run_check(WT_SESSION_IMPL *session)
{
    (void)session; /* Unused */

    /*
     * Always return true to make sure __wt_cond_wait_signal does wait. This callback is required
     * with waits longer that one second.
     */
    return (true);
}

/*
 * thread_func_checkpoint --
 *     TODO: Add a comment describing this function.
 */
static void *
thread_func_checkpoint(void *arg)
{
    struct thread_data *td;
    WT_RAND_STATE rnd;
    WT_SESSION *session;
    uint64_t sleep_sec;
    int i;
    bool signalled;

    td = (struct thread_data *)arg;

    testutil_check(td->conn->open_session(td->conn, NULL, NULL, &session));

    __wt_random_init_seed((WT_SESSION_IMPL *)session, &rnd);

    if (td->cond != NULL) {
        /*
         * Spawn the compact thread here to make sure the both threads are ready for the synced
         * start.
         */
        testutil_check(pthread_create(&thread_compact, NULL, thread_func_compact, td));

        printf("Waiting for the signal...\n");
        /*
         * Wait for the signal and time out after 20 seconds. wait_run_check is required because the
         * time out is longer that one second.
         */
        __wt_cond_wait_signal(
          (WT_SESSION_IMPL *)session, td->cond, 20 * WT_MILLION, wait_run_check, &signalled);
        testutil_assert(signalled);
        printf("Signal received!\n");
    }

    /*
     * Run several checkpoints. First one without any delay. Others will have a random delay before
     * start.
     */
    for (i = 0; i < CHECKPOINT_NUM; i++) {
        testutil_check(session->checkpoint(session, NULL));

        if (i < CHECKPOINT_NUM - 1) {
            sleep_sec = (uint64_t)__wt_random(&rnd) % 15 + 1;
            printf("Sleep %" PRIu64 " sec before next checkpoint.\n", sleep_sec);
            __wt_sleep(sleep_sec, 0);
        }
    }

    testutil_check(session->close(session, NULL));
    session = NULL;

    return (NULL);
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
    uint64_t i, str_len, val;

    __wt_random_init_seed((WT_SESSION_IMPL *)session, &rnd);

    str_len = sizeof(data_str) / sizeof(data_str[0]);
    for (i = 0; i < str_len - 1; i++)
        data_str[i] = 'a' + (uint32_t)__wt_random(&rnd) % 26;

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
remove_records(WT_SESSION *session, const char *uri)
{
    WT_CURSOR *cursor;
    uint64_t i;

    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));

    /* Remove 1/3 of the records from the middle of the key range. */
    for (i = NUM_RECORDS / 3; i < (NUM_RECORDS * 2) / 3; i++) {
        cursor->set_key(cursor, i + 1);
        testutil_check(cursor->remove(cursor));
    }

    testutil_check(cursor->close(cursor));
    cursor = NULL;
}

/*
 * get_file_stats --
 *     TODO: Add a comment describing this function.
 */
static void
get_file_stats(WT_SESSION *session, const char *uri, uint64_t *file_sz, uint64_t *avail_bytes)
{
    WT_CURSOR *cur_stat;
    char *descr, *str_val, stat_uri[128];

    sprintf(stat_uri, "statistics:%s", uri);
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

/*
 * set_timing_stress_checkpoint --
 *     TODO: Add a comment describing this function.
 */
static void
set_timing_stress_checkpoint(WT_CONNECTION *conn)
{
    WT_CONNECTION_IMPL *conn_impl;

    conn_impl = (WT_CONNECTION_IMPL *)conn;
    conn_impl->timing_stress_flags |= WT_TIMING_STRESS_CHECKPOINT_SLOW;
}

/*
 * get_compact_progress --
 *     TODO: Add a comment describing this function.
 */
static void
get_compact_progress(WT_SESSION *session, const char *uri, uint64_t *pages_reviewed,
  uint64_t *pages_skipped, uint64_t *pages_rewritten)
{

    WT_CURSOR *cur_stat;
    char *descr, *str_val;
    char stat_uri[128];

    sprintf(stat_uri, "statistics:%s", uri);
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

/*
 * check_db_size --
 *     TODO: Add a comment describing this function.
 */
static bool
check_db_size(WT_SESSION *session, const char *uri)
{
    uint64_t file_sz, avail_bytes, available_pct;

    get_file_stats(session, uri, &file_sz, &avail_bytes);

    available_pct = (avail_bytes * 100) / file_sz;
    printf(" - Compacted file size: %" PRIu64 "MB (%" PRIu64 "B)\n - Available for reuse: %" PRIu64
           "MB (%" PRIu64 "B)\n - %" PRIu64 "%% space available in the file.\n",
      file_sz / WT_MEGABYTE, file_sz, avail_bytes / WT_MEGABYTE, avail_bytes, available_pct);

    /*
     * Compaction is a best-effort algorithm. It moves blocks from the end to the beginning of the
     * file but there is no guarantee that all empty space at the beginning will be filled. The
     * logic in the algorithm checks if at least 20% of the file is available in the first 80% of
     * the file, we'll try compaction on the last 20% of the file. Else if at least 10% of the total
     * file is available in the first 90% of the file, we'll try compaction on the last 10% of the
     * file. It may well happen that 9.9% of the space is available for reuse in the first 90% of
     * the file. And 9.9% available in the last 10% of the file. In this case, the algorithm would
     * give up. But total available space in the file would be 19.8%. So we need to check that there
     * is a maximum of 20% space available for reuse after compaction.
     */
    return (available_pct <= 20);
}
