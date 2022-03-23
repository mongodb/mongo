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
 * The motivation for this test is to try and reproduce BF-24385 by stressing insert functionality.
 * The test creates a lot of threads that concurrently insert a lot of records with random keys.
 * Having a large memory page ensures that we have big insert lists. Big cache size allows having
 * more dirty content in the memory before eviction kicks in. The test is in C suite because CPP
 * suite does not allow overriding validation at the moment.
 */

#define THREAD_NUM_ITERATIONS 200000
#define NUM_THREADS 110
#define KEY_MAX UINT32_MAX
#define TABLE_CONFIG_FMT "key_format=%s,value_format=%s,memory_page_image_max=50MB"

static const char *const conn_config = "create,cache_size=4G";

static uint64_t ready_counter;

void *thread_insert_race(void *);

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
 * main --
 *     Test's entry point.
 */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;
    WT_CURSOR *cursor;
    WT_SESSION *session;
    clock_t ce, cs;
    pthread_t id[NUM_THREADS];
    int i, ret;
    char tableconf[128];

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    opts->nthreads = NUM_THREADS;
    opts->table_type = TABLE_ROW;
    testutil_check(testutil_parse_opts(argc, argv, opts));
    testutil_make_work_dir(opts->home);

    testutil_check(wiredtiger_open(opts->home, NULL, conn_config, &opts->conn));
    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session));
    testutil_check(__wt_snprintf(tableconf, sizeof(tableconf), TABLE_CONFIG_FMT,
      opts->table_type == TABLE_ROW ? "Q" : "r", opts->table_type == TABLE_FIX ? "8t" : "Q"));
    testutil_check(session->create(session, opts->uri, tableconf));

    cs = clock();

    /* Multithreaded insert */
    for (i = 0; i < (int)opts->nthreads; ++i)
        testutil_check(pthread_create(&id[i], NULL, thread_insert_race, opts));

    while (--i >= 0)
        testutil_check(pthread_join(id[i], NULL));

    /* Reopen connection for WT_SESSION::verify. It requires exclusive access to the file. */
    testutil_check(opts->conn->close(opts->conn, NULL));
    opts->conn = NULL;
    testutil_check(wiredtiger_open(opts->home, NULL, conn_config, &opts->conn));

    /* Validate */
    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session));
    testutil_check(session->verify(session, opts->uri, NULL));

    testutil_check(session->open_cursor(session, opts->uri, NULL, NULL, &cursor));

    for (i = 0; (ret = cursor->next(cursor)) == 0; i++)
        ;

    testutil_assert(ret == WT_NOTFOUND);

    ce = clock();
    printf(" Number of records: %" PRIu64 "\n Duration: %.2lf\n", (uint64_t)i,
      (ce - cs) / (double)CLOCKS_PER_SEC);

    testutil_cleanup(opts);

    return (EXIT_SUCCESS);
}

/*
 * thread_insert_race --
 *     Insert items with random keys.
 */
WT_THREAD_RET
thread_insert_race(void *arg)
{
    TEST_OPTS *opts;
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_RAND_STATE rnd;
    WT_SESSION *session;
    uint64_t i, ready_counter_local, key;

    opts = (TEST_OPTS *)arg;
    conn = opts->conn;

    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->open_cursor(session, opts->uri, NULL, NULL, &cursor));

    __wt_random_init_seed((WT_SESSION_IMPL *)session, &rnd);

    /* Wait until all the threads are ready to go. */
    (void)__wt_atomic_add64(&ready_counter, 1);
    for (;; __wt_yield()) {
        WT_ORDERED_READ(ready_counter_local, ready_counter);
        if (ready_counter_local >= opts->nthreads)
            break;
    }

    for (i = 0; i < THREAD_NUM_ITERATIONS; ++i) {
        /* Generate random values from [1, KEY_MAX] */
        key = ((uint64_t)__wt_random(&rnd) % KEY_MAX) + 1;
        set_key(cursor, key);
        set_value(opts, cursor, key);
        testutil_check(cursor->insert(cursor));
    }

    return (NULL);
}
