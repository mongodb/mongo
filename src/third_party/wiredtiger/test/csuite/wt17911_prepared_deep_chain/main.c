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

#include <pthread.h>

#define WT_DEEP_CHAIN_UPDATES 20000
#define WT_DEEP_CHAIN_STACK_BYTES (512UL * 1024UL)

static const char *const uri = "table:wt17911_prepared_deep_chain";
static const char *const conn_config = "create,cache_size=256MB";
static const char *const table_config = "key_format=S,value_format=S";

static WT_CONNECTION *g_conn;

/*
 * worker_thread --
 *     Run a prepared transaction that writes many updates to a single key, then commit or roll it
 *     back.
 */
static void *
worker_thread(void *arg)
{
    WT_CURSOR *cursor;
    WT_SESSION *session;
    uint64_t i;
    char value[64];
    bool commit;

    commit = *(bool *)arg;

    testutil_check(g_conn->open_session(g_conn, NULL, NULL, &session));
    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));

    testutil_check(session->begin_transaction(session, "isolation=snapshot"));

    for (i = 0; i < WT_DEEP_CHAIN_UPDATES; i++) {
        cursor->set_key(cursor, commit ? "commit-key" : "rollback-key");
        testutil_snprintf(value, sizeof(value), "value-%" PRIu64, i);
        cursor->set_value(cursor, value);
        testutil_check(cursor->insert(cursor));
    }

    testutil_check(session->prepare_transaction(session, "prepare_timestamp=10"));

    if (commit)
        testutil_check(
          session->commit_transaction(session, "commit_timestamp=20,durable_timestamp=20"));
    else
        testutil_check(session->rollback_transaction(session, NULL));

    testutil_check(cursor->close(cursor));
    testutil_check(session->close(session, NULL));
    return (NULL);
}

/*
 * run_worker_in_restricted_thread --
 *     Spawn a thread with restricted stack space.
 */
static void
run_worker_in_restricted_thread(bool commit)
{
    pthread_attr_t attribute;
    pthread_t thread;

    testutil_check(pthread_attr_init(&attribute));
    testutil_check(pthread_attr_setstacksize(&attribute, WT_DEEP_CHAIN_STACK_BYTES));
    testutil_check(pthread_create(&thread, &attribute, worker_thread, &commit));
    testutil_check(pthread_attr_destroy(&attribute));
    testutil_check(pthread_join(thread, NULL));
}

/*
 * run_test --
 *     Prepare a clean home directory and run the test.
 */
static void
run_test(const char *home)
{
    WT_SESSION *session;

    testutil_recreate_dir(home);

    testutil_check(wiredtiger_open(home, NULL, conn_config, &g_conn));

    testutil_check(g_conn->open_session(g_conn, NULL, NULL, &session));
    testutil_check(session->create(session, uri, table_config));
    testutil_check(session->close(session, NULL));

    testutil_check(g_conn->set_timestamp(g_conn, "oldest_timestamp=1,stable_timestamp=1"));

    run_worker_in_restricted_thread(true);
    run_worker_in_restricted_thread(false);

    testutil_check(g_conn->close(g_conn, NULL));
    g_conn = NULL;
}

/*
 * main --
 *     Entry point.
 */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));

    run_test(opts->home);

    if (!opts->preserve)
        testutil_remove(opts->home);

    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}
