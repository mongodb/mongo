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
 * This test is to create a window where a checkpoint can fail to include a transaction's updates
 * with commit times before stable. Consider a situation like below.
 * 1. In the commit transaction the commit timestamp is checked and the transaction sleeps.
 * 2. Stable timestamp is then moved past the commit timestamp.
 * 3. Checkpoint selects the stable timestamp and runs. This checkpoint will not include changes
 *    for a commit timestamp that is before the checkpoint's stable timestamp.
 *
 * Added a WT_TIMING_STRESS_COMMIT_TRANSACTION_SLOW timing stress to add a delay
 * while committing a transaction.
 */

#define NUM_RECORDS WT_THOUSAND

/* Constants and variables declaration. */

static const char conn_config[] =
  "create,cache_size=2GB,statistics=(all),statistics_log=(json,on_close,wait=1),timing_stress_for_"
  "test=[commit_transaction_slow, prepare_checkpoint_delay]";
static const char table_config_row[] =
  "allocation_size=4KB,leaf_page_max=4KB,key_format=Q,value_format=Q";
static const char *const uri = "table:wt9199-checkpoint-txn-commit-race";
static uint64_t global_stable_ts;

static volatile bool inserted;

/* Structures definition. */
struct thread_data {
    WT_CONNECTION *conn;
    const char *uri;
};

/* Forward declarations. */
static void run_test(const char *);
static void *thread_func_checkpoint(void *);
static void *thread_func_insert_txn(void *);

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

    printf("\n");
    printf("Running test ...\n");
    run_test(opts->home);

    /* Cleanup */
    if (!opts->preserve)
        testutil_remove(opts->home);

    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}

/*
 * run_test --
 *     Run test.
 */
static void
run_test(const char *home)
{
    struct thread_data td;
    WT_CONNECTION *conn;
    WT_SESSION *session;
    pthread_t thread_checkpoint, thread_insert_txn;

    inserted = false;
    testutil_recreate_dir(home);
    testutil_check(wiredtiger_open(home, NULL, conn_config, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Create the table. */
    testutil_check(session->create(session, uri, table_config_row));

    td.conn = conn;
    td.uri = uri;

    /* Spawn checkpoint and insert threads. */
    testutil_check(pthread_create(&thread_insert_txn, NULL, thread_func_insert_txn, &td));
    testutil_check(pthread_create(&thread_checkpoint, NULL, thread_func_checkpoint, &td));

    /* Wait for the threads to finish the work. */
    (void)pthread_join(thread_insert_txn, NULL);
    (void)pthread_join(thread_checkpoint, NULL);

    testutil_check(session->close(session, NULL));
    session = NULL;

    testutil_check(conn->close(conn, NULL));
    conn = NULL;
}

/*
 * thread_func_checkpoint --
 *     Function to checkpoint the database.
 */
static void *
thread_func_checkpoint(void *arg)
{
    struct thread_data *td;
    WT_SESSION *session;
    uint64_t stable_ts;
    char ts_string[WT_TS_HEX_STRING_SIZE];
    char tscfg[64];

    td = (struct thread_data *)arg;

    testutil_check(td->conn->open_session(td->conn, NULL, NULL, &session));

    while (!inserted) {
        __wt_sleep(0, 1000);
    }

    /*
     * Increment and set the stable timestamp so that checkpoint picks this timestamp as the
     * checkpoint timestamp.
     */
    global_stable_ts += 20;
    testutil_snprintf(tscfg, sizeof(tscfg), "stable_timestamp=%" PRIu64, global_stable_ts);
    testutil_check(td->conn->set_timestamp(td->conn, tscfg));

    /* Wait for sometime to let the commit transaction checks the timestamp validity. */
    __wt_sleep(2, 0);

    testutil_check(session->checkpoint(session, NULL));
    testutil_check(td->conn->query_timestamp(td->conn, ts_string, "get=last_checkpoint"));
    testutil_assert(sscanf(ts_string, "%" SCNx64, &stable_ts) == 1);

    testutil_check(session->close(session, NULL));
    return (NULL);
}

/*
 * thread_func_insert_txn --
 *     Function to insert the data with a transaction.
 */
static void *
thread_func_insert_txn(void *arg)
{
    struct thread_data *td;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_SESSION *session;
    uint64_t i;
    uint64_t val;
    char tscfg[64], tscfg_1[64];

    td = (struct thread_data *)arg;

    testutil_check(td->conn->open_session(td->conn, NULL, NULL, &session));

    /* Open a cursor on the table. */
    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));

    testutil_check(session->begin_transaction(session, NULL));
    for (i = 0; i < NUM_RECORDS; i++) {
        cursor->set_key(cursor, i + 1);
        val = i * 10;
        cursor->set_value(cursor, val);
        testutil_check(cursor->insert(cursor));
    }

    /* Initially set the stable timestamp to 50. */
    global_stable_ts = 50;
    testutil_snprintf(tscfg, sizeof(tscfg), "stable_timestamp=%" PRIu64, global_stable_ts);
    testutil_check(td->conn->set_timestamp(td->conn, tscfg));

    inserted = true;

    /*
     * Increment the stable timestamp and commit the transaction with the incremented timestamp.
     */
    global_stable_ts += 20;

    testutil_snprintf(tscfg_1, sizeof(tscfg_1), "commit_timestamp=%" PRIu64, global_stable_ts);
    ret = session->commit_transaction(session, tscfg_1);

    testutil_assert(ret == EINVAL);

    testutil_check(cursor->close(cursor));
    cursor = NULL;

    testutil_check(session->close(session, NULL));
    return (NULL);
}
