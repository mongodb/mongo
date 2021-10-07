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
static const char table_config[] =
  "allocation_size=4KB,leaf_page_max=4KB,key_format=i,value_format=QQQS";
static char data_str[1024] = "";

/* Structures definition. */
struct thread_data {
    WT_CONNECTION *conn;
    const char *uri;
    WT_CONDVAR *cond;
};

/* Forward declarations. */
static void run_test(bool stress_test, const char *home, const char *uri);
static void *thread_func_compact(void *arg);
static void *thread_func_checkpoint(void *arg);
static void populate(WT_SESSION *session, const char *uri);
static void remove_records(WT_SESSION *session, const char *uri);
static uint64_t get_file_size(WT_SESSION *session, const char *uri);
static void set_timing_stress_checkpoint(WT_CONNECTION *conn);

/* Methods implementation. */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;
    char home_cv[512];

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));

    /*
     * First run test with WT_TIMING_STRESS_CHECKPOINT_SLOW.
     */
    printf("Running stress test...\n");
    run_test(true, opts->home, opts->uri);

    /*
     * Now run test where compact and checkpoint threads are synchronized using condition variable.
     */
    printf("Running normal test...\n");
    testutil_assert(sizeof(home_cv) > strlen(opts->home) + 3);
    sprintf(home_cv, "%s.CV", opts->home);
    run_test(false, home_cv, opts->uri);

    /* Cleanup */
    if (!opts->preserve)
        testutil_clean_work_dir(home_cv);

    testutil_cleanup(opts);

    return (EXIT_SUCCESS);
}

static void
run_test(bool stress_test, const char *home, const char *uri)
{
    struct thread_data td;
    WT_CONNECTION *conn;
    WT_SESSION *session;
    pthread_t thread_checkpoint, thread_compact;
    uint64_t file_sz_after, file_sz_before;

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
    testutil_check(session->create(session, uri, table_config));

    populate(session, uri);
    testutil_check(session->checkpoint(session, NULL));

    /*
     * Remove 1/3 of data from the middle of the key range to let compact relocate blocks from the
     * end of the file.
     */
    remove_records(session, uri);

    file_sz_before = get_file_size(session, uri);

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

        testutil_check(pthread_create(&thread_checkpoint, NULL, thread_func_checkpoint, &td));
        testutil_check(pthread_create(&thread_compact, NULL, thread_func_compact, &td));
    }

    /* Wait for the threads to finish the work. */
    (void)pthread_join(thread_checkpoint, NULL);
    (void)pthread_join(thread_compact, NULL);

    file_sz_after = get_file_size(session, uri);

    /* Cleanup */
    if (!stress_test) {
        __wt_cond_destroy((WT_SESSION_IMPL *)session, &td.cond);
        td.cond = NULL;
    }

    testutil_check(session->close(session, NULL));
    session = NULL;

    testutil_check(conn->close(conn, NULL));
    conn = NULL;

    /* Check if there's at least 10% compaction. */
    printf(" - Compressed file size MB: %f\n - Original file size MB: %f\n",
      file_sz_after / (1024.0 * 1024), file_sz_before / (1024.0 * 1024));

    /* Make sure the compact operation has reduced the file size by at least 20%. */
    testutil_assert((file_sz_before / 100) * 80 > file_sz_after);
}

static void *
thread_func_compact(void *arg)
{
    struct thread_data *td;
    WT_SESSION *session;

    td = (struct thread_data *)arg;

    testutil_check(td->conn->open_session(td->conn, NULL, NULL, &session));

    if (td->cond != NULL) {
        /*
         * Make sure checkpoint thread is initialized and waiting for the signal. Sleep for one
         * second.
         */
        __wt_sleep(1, 0);

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

static void
populate(WT_SESSION *session, const char *uri)
{
    WT_CURSOR *cursor;
    WT_RAND_STATE rnd;
    uint64_t val;
    int i, str_len;

    __wt_random_init_seed((WT_SESSION_IMPL *)session, &rnd);

    str_len = sizeof(data_str) / sizeof(data_str[0]);
    for (i = 0; i < str_len - 1; i++)
        data_str[i] = 'a' + __wt_random(&rnd) % 26;

    data_str[str_len - 1] = '\0';

    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));
    for (i = 0; i < NUM_RECORDS; i++) {
        cursor->set_key(cursor, i);
        val = (uint64_t)__wt_random(&rnd);
        cursor->set_value(cursor, val, val, val, data_str);
        testutil_check(cursor->insert(cursor));
    }

    testutil_check(cursor->close(cursor));
    cursor = NULL;
}

static void
remove_records(WT_SESSION *session, const char *uri)
{
    WT_CURSOR *cursor;
    int i;

    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));

    /* Remove 1/3 of the records from the middle of the key range. */
    for (i = NUM_RECORDS / 3; i < (NUM_RECORDS * 2) / 3; i++) {
        cursor->set_key(cursor, i);
        testutil_check(cursor->remove(cursor));
    }

    testutil_check(cursor->close(cursor));
    cursor = NULL;
}

static uint64_t
get_file_size(WT_SESSION *session, const char *uri)
{
    WT_CURSOR *cur_stat;
    uint64_t val;
    char *descr, *str_val, stat_uri[128];

    sprintf(stat_uri, "statistics:%s", uri);
    testutil_check(session->open_cursor(session, stat_uri, NULL, "statistics=(all)", &cur_stat));
    cur_stat->set_key(cur_stat, WT_STAT_DSRC_BLOCK_SIZE);
    testutil_check(cur_stat->search(cur_stat));
    testutil_check(cur_stat->get_value(cur_stat, &descr, &str_val, &val));
    testutil_check(cur_stat->close(cur_stat));
    cur_stat = NULL;

    return (val);
}

static void
set_timing_stress_checkpoint(WT_CONNECTION *conn)
{
    WT_CONNECTION_IMPL *conn_impl;

    conn_impl = (WT_CONNECTION_IMPL *)conn;
    conn_impl->timing_stress_flags |= WT_TIMING_STRESS_CHECKPOINT_SLOW;
}
