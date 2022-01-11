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
 * JIRA ticket reference: WT-3874 Test case description: Set up a collator that only uses the first
 * byte of a record for comparison; all other bytes are considered padding. With that collator for a
 * table, insert an item, then remove that item (with different padding). Failure mode: An assertion
 * is fired when we get back the key as stored in the record, if we compare it to the given key
 * without taking into account the collator.
 */

#define KEY_SIZE 20

/*
 * my_compare --
 *     TODO: Add a comment describing this function.
 */
static int
my_compare(
  WT_COLLATOR *collator, WT_SESSION *session, const WT_ITEM *v1, const WT_ITEM *v2, int *cmp)
{
    (void)collator;
    (void)session;

    if (v1->size < 1 || v2->size < 1)
        return (EINVAL);
    *cmp = strncmp((const char *)v1->data, (const char *)v2->data, 1);
    return (0);
}

static WT_COLLATOR my_coll = {my_compare, NULL, NULL};

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_ITEM key;
    WT_SESSION *session;
    char buf[KEY_SIZE];

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    srand(123);

    testutil_check(testutil_parse_opts(argc, argv, opts));
    testutil_make_work_dir(opts->home);

    testutil_check(wiredtiger_open(opts->home, NULL, "create,log=(enabled)", &opts->conn));
    conn = opts->conn;
    testutil_check(conn->add_collator(conn, "my_coll", &my_coll, NULL));
    testutil_check(conn->open_session(opts->conn, NULL, NULL, &session));

    testutil_check(
      session->create(session, "table:main", "key_format=u,value_format=u,collator=my_coll"));

    testutil_check(session->open_cursor(session, "table:main", NULL, NULL, &cursor));

    memset(buf, 'X', sizeof(buf));
    buf[0] = 'a';

    key.data = buf;
    key.size = sizeof(buf);
    cursor->set_key(cursor, &key);
    cursor->set_value(cursor, &key);
    testutil_check(cursor->insert(cursor));

    testutil_check(session->checkpoint(session, NULL));

    /* Use a different padding. */
    memset(buf, 'Y', sizeof(buf));
    buf[0] = 'a';

    cursor->set_key(cursor, &key);
    testutil_check(cursor->remove(cursor));

    testutil_check(session->close(session, NULL));
    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}
