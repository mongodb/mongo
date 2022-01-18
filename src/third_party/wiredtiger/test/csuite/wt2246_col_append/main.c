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
 * JIRA ticket reference: WT-2246 Test case description: The column-store search routine used to
 * search the target leaf page even when the cursor is configured with append and we're allocating a
 * record number. That was inefficient, this test case demonstrates the inefficiency. Failure mode:
 * It isn't simple to make this test case failure explicit since it is demonstrating an inefficiency
 * rather than a correctness bug.
 */

/* Don't move into shared function there is a cross platform solution */
#include <signal.h>

#define MILLION 1000000

/* Needs to be global for signal handling. */
static TEST_OPTS *opts, _opts;

/*
 * page_init --
 *     TODO: Add a comment describing this function.
 */
static void
page_init(uint64_t n)
{
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_SESSION *session;
    uint64_t recno, vrecno;
    char buf[64];

    conn = opts->conn;

    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->open_cursor(session, opts->uri, NULL, "append", &cursor));

    vrecno = 0;
    buf[0] = '\2';
    for (recno = 1;; ++recno) {
        if (opts->table_type == TABLE_FIX)
            cursor->set_value(cursor, buf[0]);
        else {
            if (recno % 3 == 0)
                ++vrecno;
            testutil_check(__wt_snprintf(buf, sizeof(buf), "%" PRIu64 " VALUE ------", vrecno));
            cursor->set_value(cursor, buf);
        }
        testutil_check(cursor->insert(cursor));
        testutil_check(cursor->get_key(cursor, &opts->max_inserted_id));
        if (opts->max_inserted_id >= n)
            break;
    }
}

/*
 * onsig --
 *     TODO: Add a comment describing this function.
 */
static void
onsig(int signo)
{
    WT_UNUSED(signo);
    opts->running = false;
}

#define N_APPEND_THREADS 6
#define N_RECORDS (20 * WT_MILLION)

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(int argc, char *argv[])
{
    WT_SESSION *session;
    wt_thread_t idlist[100];
    clock_t ce, cs;
    uint64_t i, id;
    char buf[100];

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    opts->table_type = TABLE_ROW;
    opts->n_append_threads = N_APPEND_THREADS;
    opts->nrecords = N_RECORDS;
    testutil_check(testutil_parse_opts(argc, argv, opts));
    testutil_make_work_dir(opts->home);

    testutil_check(__wt_snprintf(buf, sizeof(buf),
      "create,cache_size=%s,eviction=(threads_max=5),statistics=(fast)",
      opts->table_type == TABLE_FIX ? "500MB" : "2GB"));
    testutil_check(wiredtiger_open(opts->home, NULL, buf, &opts->conn));
    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session));
    testutil_check(__wt_snprintf(buf, sizeof(buf),
      "key_format=r,value_format=%s,allocation_size=4K,leaf_page_max=64K",
      opts->table_type == TABLE_FIX ? "8t" : "S"));
    testutil_check(session->create(session, opts->uri, buf));
    testutil_check(session->close(session, NULL));

    page_init(5000);

    /* Force to disk and re-open. */
    testutil_check(opts->conn->close(opts->conn, NULL));
    testutil_check(wiredtiger_open(opts->home, NULL, NULL, &opts->conn));

    (void)signal(SIGINT, onsig);

    memset(idlist, 0, sizeof(idlist));
    cs = clock();
    id = 0;
    for (i = 0; i < opts->n_append_threads; ++i, ++id) {
        printf("append: %" PRIu64 "\n", id);
        testutil_check(__wt_thread_create(NULL, &idlist[id], thread_append, opts));
    }

    for (i = 0; i < id; ++i)
        testutil_check(__wt_thread_join(NULL, &idlist[i]));

    ce = clock();
    printf("%" PRIu64 "M records: %.2lf processor seconds\n", opts->max_inserted_id / MILLION,
      (ce - cs) / (double)CLOCKS_PER_SEC);

    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}
