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
 * JIRA ticket reference: WT-2999
 *
 * Test case description: Create a table that stores ~4K size blobs; two indices are defined using a
 * pair of custom extractors that pull the first and second 32-bit integers from the blob. A simple
 * join is created using the two indices, and iterated.
 *
 * Failure mode: When a custom extractor is used with cursor joins, there are memory leaks at the
 * point where the extractor sets the key.
 */

/*
 * custom_extract1 --
 *     TODO: Add a comment describing this function.
 */
static int
custom_extract1(WT_EXTRACTOR *extractor, WT_SESSION *session, const WT_ITEM *key,
  const WT_ITEM *value, WT_CURSOR *result_cursor)
{
    WT_ITEM item;
    int64_t v1;

    (void)extractor;
    (void)key;
    testutil_check(wiredtiger_struct_unpack(session, value->data, value->size, "u", &item));

    v1 = ((int64_t *)item.data)[0];
    item.data = &v1;
    item.size = sizeof(v1);

    result_cursor->set_key(result_cursor, &item);
    return (result_cursor->insert(result_cursor));
}

/*
 * custom_extract2 --
 *     TODO: Add a comment describing this function.
 */
static int
custom_extract2(WT_EXTRACTOR *extractor, WT_SESSION *session, const WT_ITEM *key,
  const WT_ITEM *value, WT_CURSOR *result_cursor)
{
    WT_ITEM item;
    int64_t v2;

    (void)extractor;
    (void)key;
    testutil_check(wiredtiger_struct_unpack(session, value->data, value->size, "u", &item));

    v2 = ((int64_t *)item.data)[1];
    item.data = &v2;
    item.size = sizeof(v2);

    result_cursor->set_key(result_cursor, &item);
    return (result_cursor->insert(result_cursor));
}

static WT_EXTRACTOR custom_extractor1 = {custom_extract1, NULL, NULL};
static WT_EXTRACTOR custom_extractor2 = {custom_extract2, NULL, NULL};

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;
    WT_CONNECTION *conn;
    WT_CURSOR *cursor1, *cursor2, *jcursor;
    WT_ITEM k, v;
    WT_SESSION *session;
    int64_t key, val[2];
    int i, ret;

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));
    testutil_make_work_dir(opts->home);

    testutil_check(wiredtiger_open(opts->home, NULL, "create,statistics=(all)", &conn));
    opts->conn = conn;
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    testutil_check(conn->add_extractor(conn, "custom_extractor1", &custom_extractor1, NULL));
    testutil_check(conn->add_extractor(conn, "custom_extractor2", &custom_extractor2, NULL));

    testutil_check(
      session->create(session, "table:main", "key_format=u,value_format=u,columns=(k,v)"));
    testutil_check(
      session->create(session, "index:main:index1", "key_format=u,extractor=custom_extractor1"));
    testutil_check(
      session->create(session, "index:main:index2", "key_format=u,extractor=custom_extractor2"));

    testutil_check(session->open_cursor(session, "table:main", NULL, NULL, &cursor1));

    v.data = val;
    v.size = sizeof(val);
    k.data = &key;
    k.size = sizeof(key);

    key = 10;
    val[0] = 20;
    val[1] = 30;
    for (i = 0; i < 100 * WT_THOUSAND; ++i) {
        key += i;
        val[0] += i;
        val[1] += i;
        cursor1->set_key(cursor1, &k);
        cursor1->set_value(cursor1, &v);
        testutil_check(cursor1->insert(cursor1));
    }

    testutil_check(cursor1->close(cursor1));

    testutil_check(session->open_cursor(session, "index:main:index1", NULL, NULL, &cursor1));
    key = 20;
    cursor1->set_key(cursor1, &k);
    testutil_check(cursor1->search(cursor1));

    testutil_check(session->open_cursor(session, "index:main:index2", NULL, NULL, &cursor2));
    key = 30;
    cursor2->set_key(cursor2, &k);
    testutil_check(cursor2->search(cursor2));

    testutil_check(session->open_cursor(session, "join:table:main", NULL, NULL, &jcursor));
    testutil_check(session->join(session, jcursor, cursor1, "compare=gt"));
    testutil_check(session->join(session, jcursor, cursor2, "compare=gt"));

    while ((ret = jcursor->next(jcursor)) == 0) /* leak */
        ;
    testutil_assert(ret == WT_NOTFOUND);

    testutil_check(jcursor->close(jcursor));
    testutil_check(cursor1->close(cursor1));
    testutil_check(cursor2->close(cursor2));

    testutil_check(opts->conn->close(opts->conn, NULL));
    opts->conn = NULL;
    testutil_cleanup(opts);

    return (EXIT_SUCCESS);
}
