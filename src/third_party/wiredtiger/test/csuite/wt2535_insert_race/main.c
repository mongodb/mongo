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
 * JIRA ticket reference: WT-2535 Test case description: This is a test case that looks for lost
 * updates to a single record. That is multiple threads each do the same number of read modify write
 * operations on a single record. At the end verify that the data contains the expected value.
 * Failure mode: Check that the data is correct at the end of the run.
 */

void *thread_insert_race(void *);

static uint64_t ready_counter;

/*
 * set_key --
 *     Wrapper providing the correct typing for the WT_CURSOR::set_key variadic argument.
 */
static void
set_key(WT_CURSOR *c, uint64_t value)
{
    c->set_key(c, value);
}

/*
 * set_value --
 *     Wrapper providing the correct typing for the WT_CURSOR::set_value variadic argument.
 */
static void
set_value(TEST_OPTS *opts, WT_CURSOR *c, uint64_t value)
{
    if (opts->table_type == TABLE_FIX)
        c->set_value(c, (uint8_t)value);
    else
        c->set_value(c, value);
}

/*
 * get_value --
 *     Wrapper providing the correct typing for the WT_CURSOR::get_value variadic argument.
 */
static uint64_t
get_value(TEST_OPTS *opts, WT_CURSOR *c)
{
    uint64_t value64;
    uint8_t value8;

    if (opts->table_type == TABLE_FIX) {
        testutil_check(c->get_value(c, &value8));
        return (value8);
    } else {
        testutil_check(c->get_value(c, &value64));
        return (value64);
    }
}

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;
    WT_CURSOR *c;
    WT_SESSION *session;
    clock_t ce, cs;
    pthread_t id[100];
    uint64_t current_value, expected_value;
    int i;
    char tableconf[128];

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    opts->nthreads = 20;
    opts->nrecords = 100 * WT_THOUSAND;
    opts->table_type = TABLE_ROW;
    testutil_check(testutil_parse_opts(argc, argv, opts));
    testutil_make_work_dir(opts->home);

    testutil_check(wiredtiger_open(opts->home, NULL,
      "create,cache_size=2G,eviction=(threads_max=5),statistics=(all),statistics_log=(json,on_"
      "close,wait=1)",
      &opts->conn));
    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session));
    testutil_check(__wt_snprintf(tableconf, sizeof(tableconf),
      "key_format=%s,value_format=%s,leaf_page_max=32k,", opts->table_type == TABLE_ROW ? "Q" : "r",
      opts->table_type == TABLE_FIX ? "8t" : "Q"));
    testutil_check(session->create(session, opts->uri, tableconf));

    /* Create the single record. */
    testutil_check(session->open_cursor(session, opts->uri, NULL, NULL, &c));
    set_key(c, 1);
    set_value(opts, c, 0);
    testutil_check(c->insert(c));
    testutil_check(c->close(c));
    cs = clock();
    for (i = 0; i < (int)opts->nthreads; ++i) {
        testutil_check(pthread_create(&id[i], NULL, thread_insert_race, opts));
    }
    while (--i >= 0)
        testutil_check(pthread_join(id[i], NULL));
    testutil_check(session->open_cursor(session, opts->uri, NULL, NULL, &c));
    set_key(c, 1);
    testutil_check(c->search(c));
    current_value = get_value(opts, c);
    expected_value = opts->nthreads * opts->nrecords;
    if (opts->table_type == TABLE_FIX)
        expected_value %= 256;
    if (current_value != expected_value) {
        fprintf(stderr, "ERROR: didn't get expected number of changes\n");
        fprintf(stderr, "got: %" PRIu64 ", expected: %" PRIu64 "\n", current_value,
          opts->nthreads * opts->nrecords);
        return (EXIT_FAILURE);
    }
    testutil_check(session->close(session, NULL));
    ce = clock();
    printf("%" PRIu64 ": %.2lf\n", opts->nrecords, (ce - cs) / (double)CLOCKS_PER_SEC);

    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}

/*
 * thread_insert_race --
 *     Append to a table in a "racy" fashion - that is attempt to insert the same record another
 *     thread is likely to also be inserting.
 */
void *
thread_insert_race(void *arg)
{
    TEST_OPTS *opts;
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_SESSION *session;
    uint64_t i, value, ready_counter_local;

    opts = (TEST_OPTS *)arg;
    conn = opts->conn;

    printf("Running insert thread\n");

    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->open_cursor(session, opts->uri, NULL, NULL, &cursor));

    /* Wait until all the threads are ready to go. */
    (void)__wt_atomic_add64(&ready_counter, 1);
    for (;; __wt_yield()) {
        WT_ORDERED_READ(ready_counter_local, ready_counter);
        if (ready_counter_local >= opts->nthreads)
            break;
    }

    for (i = 0; i < opts->nrecords; ++i) {
        testutil_check(session->begin_transaction(session, "isolation=snapshot"));
        set_key(cursor, 1);
        testutil_check(cursor->search(cursor));
        value = get_value(opts, cursor);
        set_key(cursor, 1);
        set_value(opts, cursor, value + 1);
        if ((ret = cursor->update(cursor)) != 0) {
            if (ret == WT_ROLLBACK) {
                testutil_check(session->rollback_transaction(session, NULL));
                i--;
                continue;
            }
            printf("Error in update: %d\n", ret);
        }
        testutil_check(session->commit_transaction(session, NULL));
        if (i % (10 * WT_THOUSAND) == 0) {
            printf("insert: %" PRIu64 "\r", i);
            fflush(stdout);
        }
    }
    if (i > 10 * WT_THOUSAND)
        printf("\n");

    opts->running = false;

    return (NULL);
}
