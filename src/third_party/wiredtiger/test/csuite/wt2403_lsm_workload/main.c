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

static const char name[] = "lsm:test";
#define NUM_DOCS 100000
#define NUM_QUERIES (NUM_DOCS / 100)

/*
 * rand_str --
 *     TODO: Add a comment describing this function.
 */
static void
rand_str(uint64_t i, char *str)
{
    uint64_t x, y;

    y = strlen(str);
    for (x = y; x > y - 8; x--) {
        str[x - 1] = (char)(i % 10) + 48;
        i = i / 10;
    }
}

/*
 * check_str --
 *     TODO: Add a comment describing this function.
 */
static void
check_str(uint64_t i, char *str, bool mod)
{
    char str2[] = "0000000000000000";

    rand_str(i, str2);
    if (mod)
        str2[0] = 'A';
    testutil_checkfmt(strcmp(str, str2), "strcmp failed, got %s, expected %s", str, str2);
}

/*
 * query_docs --
 *     TODO: Add a comment describing this function.
 */
static void
query_docs(WT_CURSOR *cursor, bool mod)
{
    WT_ITEM key, value;
    int i;

    for (i = 0; i < NUM_QUERIES; i++) {
        testutil_check(cursor->next(cursor));
        testutil_check(cursor->get_key(cursor, &key));
        testutil_check(cursor->get_value(cursor, &value));
        check_str((uint64_t)key.data, (char *)value.data, mod);
    }
    printf("%d documents read\n", NUM_QUERIES);
}

/*
 * compact_thread --
 *     TODO: Add a comment describing this function.
 */
static void *
compact_thread(void *args)
{
    WT_SESSION *session;

    session = (WT_SESSION *)args;
    testutil_check(session->compact(session, name, NULL));
    return (NULL);
}

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;
    WT_CURSOR *rcursor, *wcursor;
    WT_ITEM key, value;
    WT_SESSION *session, *session2;
    pthread_t thread;
    uint64_t i;

    char str[] = "0000000000000000";

    /*
     * Create a clean test directory for this run of the test program if the environment variable
     * isn't already set (as is done by make check).
     */
    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));
    testutil_make_work_dir(opts->home);
    testutil_check(wiredtiger_open(opts->home, NULL, "create,cache_size=200M", &opts->conn));

    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session));
    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session2));

    testutil_check(session->create(session, name, "key_format=Q,value_format=S"));

    /* Populate the table with some data. */
    testutil_check(session->open_cursor(session, name, NULL, "overwrite", &wcursor));
    for (i = 0; i < NUM_DOCS; i++) {
        wcursor->set_key(wcursor, i);
        rand_str(i, str);
        wcursor->set_value(wcursor, str);
        testutil_check(wcursor->insert(wcursor));
    }
    testutil_check(wcursor->close(wcursor));
    printf("%d documents inserted\n", NUM_DOCS);

    /* Perform some random reads */
    testutil_check(session->open_cursor(session, name, NULL, "next_random=true", &rcursor));
    query_docs(rcursor, false);
    testutil_check(rcursor->close(rcursor));

    /* Setup Transaction to pin the current values */
    testutil_check(session2->begin_transaction(session2, "isolation=snapshot"));
    testutil_check(session2->open_cursor(session2, name, NULL, "next_random=true", &rcursor));

    /* Perform updates in a txn to confirm that we see only the original. */
    testutil_check(session->open_cursor(session, name, NULL, "overwrite", &wcursor));
    for (i = 0; i < NUM_DOCS; i++) {
        rand_str(i, str);
        str[0] = 'A';
        wcursor->set_key(wcursor, i);
        wcursor->set_value(wcursor, str);
        testutil_check(wcursor->update(wcursor));
    }
    testutil_check(wcursor->close(wcursor));
    printf("%d documents set to update\n", NUM_DOCS);

    /* Random reads, which should see the original values */
    query_docs(rcursor, false);
    testutil_check(rcursor->close(rcursor));

    /* Finish the txn */
    testutil_check(session2->rollback_transaction(session2, NULL));

    /* Random reads, which should see the updated values */
    testutil_check(session2->open_cursor(session2, name, NULL, "next_random=true", &rcursor));
    query_docs(rcursor, true);
    testutil_check(rcursor->close(rcursor));

    /* Setup a pre-delete txn */
    testutil_check(session2->begin_transaction(session2, "isolation=snapshot"));
    testutil_check(session2->open_cursor(session2, name, NULL, "next_random=true", &rcursor));

    /* Delete all but one document */
    testutil_check(session->open_cursor(session, name, NULL, "overwrite", &wcursor));
    for (i = 0; i < NUM_DOCS - 1; i++) {
        wcursor->set_key(wcursor, i);
        testutil_check(wcursor->remove(wcursor));
    }
    testutil_check(wcursor->close(wcursor));
    printf("%d documents deleted\n", NUM_DOCS - 1);

    /* Random reads, which should not see the deletes */
    query_docs(rcursor, true);
    testutil_check(rcursor->close(rcursor));

    /* Rollback the txn so we can see the deletes */
    testutil_check(session2->rollback_transaction(session2, NULL));

    /* Find the one remaining document 3 times */
    testutil_check(session2->open_cursor(session2, name, NULL, "next_random=true", &rcursor));
    for (i = 0; i < 3; i++) {
        testutil_check(rcursor->next(rcursor));
        testutil_check(rcursor->get_key(rcursor, &key));
        testutil_check(rcursor->get_value(rcursor, &value));
        /* There should only be one value available to us */
        testutil_assertfmt((uint64_t)key.data == NUM_DOCS - 1, "expected %d and got %" PRIu64,
          NUM_DOCS - 1, (uint64_t)key.data);
        check_str((uint64_t)key.data, (char *)value.data, true);
    }
    printf("Found the deleted doc 3 times\n");
    testutil_check(rcursor->close(rcursor));

    /* Repopulate the table for compact. */
    testutil_check(session->open_cursor(session, name, NULL, "overwrite", &wcursor));
    for (i = 0; i < NUM_DOCS - 1; i++) {
        wcursor->set_key(wcursor, i);
        rand_str(i, str);
        str[0] = 'A';
        wcursor->set_value(wcursor, str);
        testutil_check(wcursor->insert(wcursor));
    }
    testutil_check(wcursor->close(wcursor));

    /* Run random cursor queries while compact is running */
    testutil_check(session2->open_cursor(session2, name, NULL, "next_random=true", &rcursor));
    testutil_check(pthread_create(&thread, NULL, compact_thread, session));
    query_docs(rcursor, true);
    testutil_check(rcursor->close(rcursor));
    testutil_check(pthread_join(thread, NULL));

    /* Delete everything. Check for infinite loops */
    testutil_check(session->open_cursor(session, name, NULL, "overwrite", &wcursor));
    for (i = 0; i < NUM_DOCS; i++) {
        wcursor->set_key(wcursor, i);
        testutil_check(wcursor->remove(wcursor));
    }
    testutil_check(wcursor->close(wcursor));

    testutil_check(session2->open_cursor(session2, name, NULL, "next_random=true", &rcursor));
    for (i = 0; i < 3; i++)
        testutil_assert(rcursor->next(rcursor) == WT_NOTFOUND);
    printf("Successfully got WT_NOTFOUND\n");

    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}
