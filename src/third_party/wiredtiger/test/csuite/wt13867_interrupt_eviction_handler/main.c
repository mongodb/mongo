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
 * This test verifies that eviction can be interrupted.
 */
#define NUM_RECORDS (10000)
#define WRITE_CYCLES (40000)
#define MIN_CACHE_OPS (100)

/* The table URI. */
#define TABLE_URI "table:evict"

/* Constants and variables declaration. */
static const char conn_config[] = "create,cache_size=1MB,statistics=(all)";
static const char table_config[] =
  "allocation_size=4KB,leaf_page_max=4KB,key_format=Q,value_format=" WT_UNCHECKED_STRING(QS);

static char data_str[WT_KILOBYTE] = "";

static bool do_interrupt_eviction = false;
static bool handle_general_called = false;

static WT_SESSION *my_session;

/*
 * handle_general --
 *     General event handler.
 */
static int
handle_general(WT_EVENT_HANDLER *handler, WT_CONNECTION *conn, WT_SESSION *session,
  WT_EVENT_TYPE type, void *arg)
{
    (void)(handler);
    (void)(conn);
    (void)(session);
    (void)(arg);

    if (type != WT_EVENT_EVICTION)
        return (0);

    /* Make sure we only get called for our session - not any internal one. */
    testutil_assert(session == my_session);

    handle_general_called = true;

    return (do_interrupt_eviction ? -1 : 0);
}

static WT_EVENT_HANDLER event_handler = {
  NULL, NULL,    /* Message handlers */
  NULL,          /* Progress handler */
  NULL,          /* Close handler */
  handle_general /* General handler */
};

#define GET_STAT(KEY, VARIABLE)                                           \
    do {                                                                  \
        const char *desc, *pvalue;                                        \
        stat->set_key(stat, KEY);                                         \
        testutil_check(stat->search(stat));                               \
        testutil_check(stat->get_value(stat, &desc, &pvalue, &VARIABLE)); \
        printf("%s = %" PRId64 "\n", #KEY, VARIABLE);                     \
    } while (0)

#define GET_STATS(                                                                        \
  CACHE_OPS_VAR, CACHE_BUSY_OPS_VAR, CACHE_IDLE_OPS_VAR, BYTES_MAX_VAR, BYTES_INUSE_VAR)  \
    do {                                                                                  \
        WT_CURSOR *stat;                                                                  \
        testutil_check(session->open_cursor(session, "statistics:", NULL, NULL, &stat));  \
        GET_STAT(WT_STAT_CONN_APPLICATION_CACHE_OPS, CACHE_OPS_VAR);                      \
        GET_STAT(WT_STAT_CONN_APPLICATION_CACHE_UNINTERRUPTIBLE_OPS, CACHE_BUSY_OPS_VAR); \
        GET_STAT(WT_STAT_CONN_APPLICATION_CACHE_INTERRUPTIBLE_OPS, CACHE_IDLE_OPS_VAR);   \
        GET_STAT(WT_STAT_CONN_CACHE_BYTES_MAX, BYTES_MAX_VAR);                            \
        GET_STAT(WT_STAT_CONN_CACHE_BYTES_INUSE, BYTES_INUSE_VAR);                        \
        testutil_check(stat->close(stat));                                                \
    } while (0)

#define GET_ALL_STATS(IDX)                                                                    \
    printf("  Stats for cycle %d:\n", IDX);                                                   \
    int64_t cache_ops##IDX;                                                                   \
    int64_t cache_busy_ops##IDX;                                                              \
    int64_t cache_idle_ops##IDX;                                                              \
    int64_t cache_bytes_max##IDX, cache_bytes_inuse##IDX;                                     \
    GET_STATS(cache_ops##IDX, cache_busy_ops##IDX, cache_idle_ops##IDX, cache_bytes_max##IDX, \
      cache_bytes_inuse##IDX)

/*
 * populate --
 *     Populate the table.
 */
static void
populate(WT_SESSION *session, uint64_t count)
{
    WT_CURSOR *cursor;
    WT_RAND_STATE rnd;

    /* Use a static seed for better reproducible results. */
    __wt_random_init_seed(&rnd, 0);

    uint64_t str_len = sizeof(data_str) / sizeof(data_str[0]);
    for (uint64_t i = 0; i < str_len - 1; i++)
        data_str[i] = 'a' + (uint32_t)__wt_random(&rnd) % 26;
    data_str[str_len - 1] = '\0';

    testutil_check(session->open_cursor(session, TABLE_URI, NULL, NULL, &cursor));

    for (uint64_t i = 0; i < count; ++i) {
        uint64_t val = (uint64_t)__wt_random(&rnd);
        /* Populate random keys so that we don't have a single hot page. */
        cursor->set_key(cursor, (uint64_t)__wt_random(&rnd) % NUM_RECORDS);
        cursor->set_value(cursor, val, data_str);
        testutil_check(cursor->insert(cursor));
    }

    testutil_check(cursor->close(cursor));
}

/*
 * main --
 *     The main method.
 */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;
    WT_CONNECTION *conn;
    WT_SESSION *session;
    char home[1024];

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));

    /* Initialize database. */

    testutil_work_dir_from_path(home, sizeof(home), "WT_TEST.evict-abort");
    testutil_recreate_dir(home);

    testutil_check(wiredtiger_open(home, &event_handler, conn_config, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    my_session = session;
    testutil_check(session->create(session, TABLE_URI, table_config));

    /* Do operations without blocking eviction. */

    uint64_t cycle = 1;
redo1:
    handle_general_called = false;
    populate(session, WRITE_CYCLES * cycle);

    GET_ALL_STATS(1);
    if (cache_ops1 < MIN_CACHE_OPS) {
        /* If we didn't do enough cache operations, do more. */
        cycle *= 2;
        testutil_assert(cycle <= 1024);
        goto redo1;
    }

    /* Sanity checks. */
    testutil_assert(handle_general_called);
    testutil_assert(cache_ops1 > 0);
    testutil_assert(cache_bytes_max1 > 0);
    testutil_assert(cache_bytes_inuse1 > 0);

    /* Both idle and busy counters should increase. */
    testutil_assert(cache_busy_ops1 + cache_idle_ops1 == cache_ops1);

    printf("Cache fill ratio = %" PRId64 "%%\n", cache_bytes_inuse1 * 100 / cache_bytes_max1);

    /* Do operations blocking eviction. */

    cycle = 1;
redo2:
    do_interrupt_eviction = true;
    handle_general_called = false;
    populate(session, WRITE_CYCLES * cycle);

    GET_ALL_STATS(2);
    if (cache_ops2 - cache_ops1 < MIN_CACHE_OPS) {
        /* If we didn't do enough cache operations, do more. */
        cycle *= 2;
        testutil_assert(cycle <= 1024);
        goto redo2;
    }

    /* Sanity checks. */
    testutil_assert(handle_general_called);
    testutil_assert(cache_ops2 - cache_ops1 > 0);
    testutil_assert(cache_bytes_max2 > 0);
    testutil_assert(cache_bytes_inuse2 > 0);

    /* Busy counters should increase. */
    testutil_assert(cache_busy_ops2 - cache_busy_ops1 > 0);
    /* Idle counters should remain the same. */
    testutil_assert(cache_idle_ops2 - cache_idle_ops1 == 0);

    printf("Cache fill ratio = %" PRId64 "%%\n", cache_bytes_inuse2 * 100 / cache_bytes_max2);

    /* Finish the test and clean up. */

    do_interrupt_eviction = false;
    testutil_check(session->close(session, NULL));
    testutil_check(conn->close(conn, NULL));

    if (!opts->preserve)
        testutil_remove(home);
    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}
