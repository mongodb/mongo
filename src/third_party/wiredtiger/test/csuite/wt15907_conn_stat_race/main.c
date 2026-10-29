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
 * A connection statistic that is set rather than accumulated is read back as the sum of its
 * buckets. Gathering such a statistic must therefore leave the buckets summing to the value at
 * every instant, because opening a statistics cursor gathers into the shared array and then
 * aggregates it without holding anything against a concurrent gatherer.
 *
 * Two threads open statistics cursors here. A transaction holds dirty data in the cache the whole
 * time, so a zero for the dirty-bytes statistic can only come from a reader catching the buckets
 * mid-update. The statistics-log server is the gatherer that exposes this in practice - the Python
 * test framework enables it for every test configured with statistics=(all) - but it samples once a
 * second, far too rarely to make the window observable, so a thread stands in for it.
 */

#define NUM_RECORDS 400
#define READS_DEFAULT (500 * WT_THOUSAND)
#define VALUE_SIZE 2000

static const char conn_config[] = "create,cache_size=1GB,statistics=(all)";
static const char table_config[] = "key_format=i,value_format=S";
static const char *const uri = "table:wt15907-conn-stat-race";

static WT_CONNECTION *conn;

/*
 * Nothing is published through this flag, so the gather thread only has to observe it eventually
 * and a relaxed access is enough. It still has to be atomic: the store races the load.
 */
static bool done;

/* Forward declarations. */
static int64_t read_cache_bytes_dirty(WT_SESSION *);
static void run_test(const char *, uint64_t);
static void *thread_func_gather(void *);

/*
 * read_cache_bytes_dirty --
 *     Return the connection's dirty-bytes statistic, gathering and aggregating it as any statistics
 *     cursor open does.
 */
static int64_t
read_cache_bytes_dirty(WT_SESSION *session)
{
    WT_CURSOR *cursor;
    int64_t value;
    const char *desc, *pvalue;

    testutil_check(session->open_cursor(session, "statistics:", NULL, NULL, &cursor));
    cursor->set_key(cursor, WT_STAT_CONN_CACHE_BYTES_DIRTY);
    testutil_check(cursor->search(cursor));
    testutil_check(cursor->get_value(cursor, &desc, &pvalue, &value));
    testutil_check(cursor->close(cursor));

    return (value);
}

/*
 * thread_func_gather --
 *     Stand in for the statistics-log server, gathering connection statistics in a loop.
 */
static void *
thread_func_gather(void *arg)
{
    WT_SESSION *session;

    (void)arg;

    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    while (!__wt_atomic_load_bool_relaxed(&done))
        (void)read_cache_bytes_dirty(session);
    testutil_check(session->close(session, NULL));

    return (NULL);
}

/*
 * run_test --
 *     Hold dirty data in the cache and check that concurrent gathering never makes the statistic
 *     read as zero.
 */
static void
run_test(const char *home, uint64_t reads)
{
    WT_CURSOR *cursor;
    WT_SESSION *session;
    pthread_t thread_gather;
    uint64_t i, zero_reads;
    int64_t value;
    char buf[VALUE_SIZE + 1];

    __wt_atomic_store_bool_relaxed(&done, false);
    zero_reads = 0;

    memset(buf, 'a', sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    testutil_recreate_dir(home);
    testutil_check(wiredtiger_open(home, NULL, conn_config, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->create(session, uri, table_config));
    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));

    /* Leave the transaction open so the data stays dirty and uncommitted for every read. */
    testutil_check(session->begin_transaction(session, NULL));
    for (i = 1; i <= NUM_RECORDS; i++) {
        cursor->set_key(cursor, (int)i);
        cursor->set_value(cursor, buf);
        testutil_check(cursor->insert(cursor));
    }

    value = read_cache_bytes_dirty(session);
    testutil_assert(value > 0);

    testutil_check(pthread_create(&thread_gather, NULL, thread_func_gather, NULL));

    for (i = 0; i < reads; i++)
        if (read_cache_bytes_dirty(session) == 0)
            ++zero_reads;

    __wt_atomic_store_bool_relaxed(&done, true);
    (void)pthread_join(thread_gather, NULL);

    if (zero_reads != 0)
        testutil_die(EINVAL,
          "cache_bytes_dirty read as zero on %" PRIu64 " of %" PRIu64
          " reads while a transaction held dirty data",
          zero_reads, reads);

    testutil_check(session->rollback_transaction(session, NULL));
    testutil_check(session->close(session, NULL));
    testutil_check(conn->close(conn, NULL));
    conn = NULL;
}

/*
 * main --
 *     Methods implementation.
 */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;
    uint64_t reads;

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));

    reads = opts->nrecords == 0 ? READS_DEFAULT : opts->nrecords;

    printf("Running test with %" PRIu64 " statistics reads ...\n", reads);
    run_test(opts->home, reads);

    if (!opts->preserve)
        testutil_remove(opts->home);

    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}
