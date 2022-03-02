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
 * JIRA ticket reference: WT-3135 Test case description: Each set of data is ordered and contains
 * five elements (0-4). We insert elements 1 and 3, and then do search_near and search for each
 * element. For each set of data, we perform these tests first using a custom collator, and second
 * using a custom collator and extractor. In each case there are index keys having variable length.
 * Failure mode: In the reported test case, the custom compare routine is given a truncated key to
 * compare, and the unpack functions return errors because the truncation appeared in the middle of
 * a key.
 */

#define TEST_ENTRY_COUNT 5
typedef const char *TEST_SET[TEST_ENTRY_COUNT];
static TEST_SET test_sets[] = {{"0", "01", "012", "0123", "01234"}, {"A", "B", "C", "D", "E"},
  {"5", "54", "543", "5432", "54321"}, {"54321", "5433", "544", "55", "6"}};
#define TEST_SET_COUNT (sizeof(test_sets) / sizeof(test_sets[0]))

/*
 * item_str_equal --
 *     TODO: Add a comment describing this function.
 */
static bool
item_str_equal(WT_ITEM *item, const char *str)
{
    return (item->size == strlen(str) + 1 && strncmp((char *)item->data, str, item->size) == 0);
}

/*
 * compare_int --
 *     TODO: Add a comment describing this function.
 */
static int
compare_int(int64_t a, int64_t b)
{
    return (a < b ? -1 : (a > b ? 1 : 0));
}

/*
 * index_compare_primary --
 *     TODO: Add a comment describing this function.
 */
static int
index_compare_primary(WT_PACK_STREAM *s1, WT_PACK_STREAM *s2, int *cmp)
{
    int64_t pkey1, pkey2;
    int rc1, rc2;

    rc1 = wiredtiger_unpack_int(s1, &pkey1);
    rc2 = wiredtiger_unpack_int(s2, &pkey2);

    if (rc1 == 0 && rc2 == 0)
        *cmp = compare_int(pkey1, pkey2);
    else if (rc1 != 0 && rc2 != 0)
        *cmp = 0;
    else if (rc1 != 0)
        *cmp = -1;
    else
        *cmp = 1;
    return (0);
}

/*
 * index_compare_S --
 *     TODO: Add a comment describing this function.
 */
static int
index_compare_S(
  WT_COLLATOR *collator, WT_SESSION *session, const WT_ITEM *key1, const WT_ITEM *key2, int *cmp)
{
    WT_PACK_STREAM *s1, *s2;
    const char *skey1, *skey2;

    (void)collator;

    testutil_check(wiredtiger_unpack_start(session, "Si", key1->data, key1->size, &s1));
    testutil_check(wiredtiger_unpack_start(session, "Si", key2->data, key2->size, &s2));

    testutil_check(wiredtiger_unpack_str(s1, &skey1));
    testutil_check(wiredtiger_unpack_str(s2, &skey2));

    if ((*cmp = strcmp(skey1, skey2)) == 0)
        testutil_check(index_compare_primary(s1, s2, cmp));

    testutil_check(wiredtiger_pack_close(s1, NULL));
    testutil_check(wiredtiger_pack_close(s2, NULL));

    return (0);
}

/*
 * index_compare_u --
 *     TODO: Add a comment describing this function.
 */
static int
index_compare_u(
  WT_COLLATOR *collator, WT_SESSION *session, const WT_ITEM *key1, const WT_ITEM *key2, int *cmp)
{
    WT_ITEM skey1, skey2;
    WT_PACK_STREAM *s1, *s2;

    (void)collator;

    testutil_check(wiredtiger_unpack_start(session, "ui", key1->data, key1->size, &s1));
    testutil_check(wiredtiger_unpack_start(session, "ui", key2->data, key2->size, &s2));

    testutil_check(wiredtiger_unpack_item(s1, &skey1));
    testutil_check(wiredtiger_unpack_item(s2, &skey2));

    if ((*cmp = strcmp(skey1.data, skey2.data)) == 0)
        testutil_check(index_compare_primary(s1, s2, cmp));

    testutil_check(wiredtiger_pack_close(s1, NULL));
    testutil_check(wiredtiger_pack_close(s2, NULL));

    return (0);
}

/*
 * index_extractor_u --
 *     TODO: Add a comment describing this function.
 */
static int
index_extractor_u(WT_EXTRACTOR *extractor, WT_SESSION *session, const WT_ITEM *key,
  const WT_ITEM *value, WT_CURSOR *result_cursor)
{
    (void)extractor;
    (void)session;
    (void)key;

    result_cursor->set_key(result_cursor, value);
    return result_cursor->insert(result_cursor);
}

static WT_COLLATOR collator_S = {index_compare_S, NULL, NULL};
static WT_COLLATOR collator_u = {index_compare_u, NULL, NULL};
static WT_EXTRACTOR extractor_u = {index_extractor_u, NULL, NULL};

/*
 * search_using_str --
 *     Check search() and search_near() using the test string indicated by test_index.
 */
static void
search_using_str(WT_CURSOR *cursor, TEST_SET test_set, int test_index)
{
    int exact, ret;
    const char *result;
    const char *str_01, *str_0123, *test_str;

    testutil_assert(test_index >= 0 && test_index <= 4);
    str_01 = test_set[1];
    str_0123 = test_set[3];
    test_str = test_set[test_index];

    cursor->set_key(cursor, test_str);
    testutil_check(cursor->search_near(cursor, &exact));
    testutil_check(cursor->get_key(cursor, &result));

    if (test_index == 0)
        testutil_assert(strcmp(result, str_01) == 0 && exact > 0);
    else if (test_index == 1)
        testutil_assert(strcmp(result, str_01) == 0 && exact == 0);
    else if (test_index == 2)
        testutil_assert((strcmp(result, str_0123) == 0 && exact > 0) ||
          (strcmp(result, str_01) == 0 && exact < 0));
    else if (test_index == 3)
        testutil_assert(strcmp(result, str_0123) == 0 && exact == 0);
    else if (test_index == 4)
        testutil_assert(strcmp(result, str_0123) == 0 && exact < 0);

    cursor->set_key(cursor, test_str);
    ret = cursor->search(cursor);

    if (test_index == 0 || test_index == 2 || test_index == 4)
        testutil_assert(ret == WT_NOTFOUND);
    else if (test_index == 1 || test_index == 3)
        testutil_assert(ret == 0);
}

/*
 * search_using_item --
 *     Check search() and search_near() using the test string indicated by test_index against a
 *     table containing a variable sized item.
 */
static void
search_using_item(WT_CURSOR *cursor, TEST_SET test_set, int test_index)
{
    WT_ITEM item;
    size_t testlen;
    int exact, ret;
    const char *str_01, *str_0123, *test_str;

    testutil_assert(test_index >= 0 && test_index <= 4);
    str_01 = test_set[1];
    str_0123 = test_set[3];
    test_str = test_set[test_index];

    testlen = strlen(test_str) + 1;
    item.data = test_str;
    item.size = testlen;
    cursor->set_key(cursor, &item);
    testutil_check(cursor->search_near(cursor, &exact));
    testutil_check(cursor->get_key(cursor, &item));

    if (test_index == 0)
        testutil_assert(item_str_equal(&item, str_01) && exact > 0);
    else if (test_index == 1)
        testutil_assert(item_str_equal(&item, str_01) && exact == 0);
    else if (test_index == 2)
        testutil_assert((item_str_equal(&item, str_0123) && exact > 0) ||
          (item_str_equal(&item, str_01) && exact < 0));
    else if (test_index == 3)
        testutil_assert(item_str_equal(&item, str_0123) && exact == 0);
    else if (test_index == 4)
        testutil_assert(item_str_equal(&item, str_0123) && exact < 0);

    item.data = test_str;
    item.size = testlen;
    cursor->set_key(cursor, &item);
    ret = cursor->search(cursor);

    if (test_index == 0 || test_index == 2 || test_index == 4)
        testutil_assert(ret == WT_NOTFOUND);
    else if (test_index == 1 || test_index == 3)
        testutil_assert(ret == 0);
}

/*
 * test_one_set --
 *     For each set of data, perform tests.
 */
static void
test_one_set(WT_SESSION *session, TEST_SET set)
{
    WT_CURSOR *cursor;
    WT_ITEM item;
    int32_t i;

    /*
     * Part 1: Using a custom collator, insert some elements and verify results from search_near.
     */

    testutil_check(
      session->create(session, "table:main", "key_format=i,value_format=S,columns=(k,v)"));
    testutil_check(session->create(session, "index:main:def_collator", "columns=(v)"));
    testutil_check(
      session->create(session, "index:main:custom_collator", "columns=(v),collator=collator_S"));

    /* Insert only elements #1 and #3. */
    testutil_check(session->open_cursor(session, "table:main", NULL, NULL, &cursor));
    cursor->set_key(cursor, 0);
    cursor->set_value(cursor, set[1]);
    testutil_check(cursor->insert(cursor));
    cursor->set_key(cursor, 1);
    cursor->set_value(cursor, set[3]);
    testutil_check(cursor->insert(cursor));
    testutil_check(cursor->close(cursor));

    /* Check all elements in def_collator index. */
    testutil_check(session->open_cursor(session, "index:main:def_collator", NULL, NULL, &cursor));
    for (i = 0; i < (int32_t)TEST_ENTRY_COUNT; i++)
        search_using_str(cursor, set, i);
    testutil_check(cursor->close(cursor));

    /* Check all elements in custom_collator index */
    testutil_check(
      session->open_cursor(session, "index:main:custom_collator", NULL, NULL, &cursor));
    for (i = 0; i < (int32_t)TEST_ENTRY_COUNT; i++)
        search_using_str(cursor, set, i);
    testutil_check(cursor->close(cursor));

    /*
     * Part 2: perform the same checks using a custom collator and extractor.
     */
    testutil_check(
      session->create(session, "table:main2", "key_format=i,value_format=u,columns=(k,v)"));

    testutil_check(session->create(
      session, "index:main2:idx_w_coll", "key_format=u,collator=collator_u,extractor=extractor_u"));

    testutil_check(session->open_cursor(session, "table:main2", NULL, NULL, &cursor));

    memset(&item, 0, sizeof(item));
    item.size = strlen(set[1]) + 1;
    item.data = set[1];
    cursor->set_key(cursor, 1);
    cursor->set_value(cursor, &item);
    testutil_check(cursor->insert(cursor));

    item.size = strlen(set[3]) + 1;
    item.data = set[3];
    cursor->set_key(cursor, 3);
    cursor->set_value(cursor, &item);
    testutil_check(cursor->insert(cursor));

    testutil_check(cursor->close(cursor));

    testutil_check(session->open_cursor(session, "index:main2:idx_w_coll", NULL, NULL, &cursor));
    for (i = 0; i < (int32_t)TEST_ENTRY_COUNT; i++)
        search_using_item(cursor, set, i);
    testutil_check(cursor->close(cursor));

    testutil_drop(session, "table:main", NULL);
    testutil_drop(session, "table:main2", NULL);
}

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;
    WT_SESSION *session;
    size_t i;

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));
    testutil_make_work_dir(opts->home);

    testutil_check(wiredtiger_open(opts->home, NULL, "create", &opts->conn));
    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session));

    /* Add any collators and extractors used by tests */
    testutil_check(opts->conn->add_collator(opts->conn, "collator_S", &collator_S, NULL));
    testutil_check(opts->conn->add_collator(opts->conn, "collator_u", &collator_u, NULL));
    testutil_check(opts->conn->add_extractor(opts->conn, "extractor_u", &extractor_u, NULL));

    for (i = 0; i < TEST_SET_COUNT; i++) {
        printf("test set %" WT_SIZET_FMT "\n", i);
        test_one_set(session, test_sets[i]);
    }

    testutil_check(session->close(session, NULL));
    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}
