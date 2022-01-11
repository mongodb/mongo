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
 * JIRA ticket reference: WT-3184 Test case description: Each set of data is ordered and contains
 * five elements (0-4). We insert elements 1 and 3, and then do search_near and search for each
 * element. For each set of data, we perform these tests first using a custom collator, and second
 * using a custom collator and extractor. In each case there are index keys having variable length.
 * Failure mode: In the reported test case, the custom compare routine is given a truncated key to
 * compare, and the unpack functions return errors because the truncation appeared in the middle of
 * a key.
 */

/*
 * compare_int --
 *     TODO: Add a comment describing this function.
 */
static int
compare_int(int32_t a, int32_t b)
{
    return (a < b ? -1 : (a > b ? 1 : 0));
}

/*
 * item_to_int --
 *     TODO: Add a comment describing this function.
 */
static int32_t
item_to_int(const WT_ITEM *item)
{
    int32_t ret;

    testutil_assert(item->size == sizeof(int32_t));

    /*
     * Using memcpy instead of direct type cast to avoid undefined behavior sanitizer complaining
     * about misaligned address.
     */
    memcpy(&ret, item->data, sizeof(int32_t));
    return ret;
}

/*
 * compare_int_items --
 *     TODO: Add a comment describing this function.
 */
static int
compare_int_items(WT_ITEM *itema, WT_ITEM *itemb)
{
    testutil_assert(itema->size == sizeof(int32_t));
    testutil_assert(itemb->size == sizeof(int32_t));
    return (compare_int(item_to_int(itema), item_to_int(itemb)));
}

/*
 * print_int_item --
 *     TODO: Add a comment describing this function.
 */
static void
print_int_item(const char *str, const WT_ITEM *item)
{
    if (item->size > 0)
        printf("%s%" PRId32, str, item_to_int(item));
    else
        printf("%s<empty>", str);
}

/*
 * index_compare --
 *     TODO: Add a comment describing this function.
 */
static int
index_compare(
  WT_COLLATOR *collator, WT_SESSION *session, const WT_ITEM *key1, const WT_ITEM *key2, int *cmp)
{
    WT_ITEM ikey1, ikey2, pkey1, pkey2;

    (void)collator;
    testutil_check(wiredtiger_struct_unpack(session, key1->data, key1->size, "uu", &ikey1, &pkey1));
    testutil_check(wiredtiger_struct_unpack(session, key2->data, key2->size, "uu", &ikey2, &pkey2));

    print_int_item("index_compare: index key1 = ", &ikey1);
    print_int_item(", primary key1 = ", &pkey1);
    print_int_item(", index key2 = ", &ikey2);
    print_int_item(", primary key2 = ", &pkey2);
    printf("\n");

    if ((*cmp = compare_int_items(&ikey1, &ikey2)) != 0)
        return (0);

    if (pkey1.size != 0 && pkey2.size != 0)
        *cmp = compare_int_items(&pkey1, &pkey2);
    else if (pkey1.size != 0)
        *cmp = 1;
    else if (pkey2.size != 0)
        *cmp = -1;
    else
        *cmp = 0;

    return (0);
}

static WT_COLLATOR index_coll = {index_compare, NULL, NULL};

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;
    WT_CURSOR *cursor, *cursor1;
    WT_ITEM got, k, v;
    WT_SESSION *session;
    int32_t ki, vi;

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));
    testutil_make_work_dir(opts->home);

    testutil_check(wiredtiger_open(opts->home, NULL, "create", &opts->conn));
    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session));

    testutil_check(opts->conn->add_collator(opts->conn, "index_coll", &index_coll, NULL));

    testutil_check(
      session->create(session, "table:main", "key_format=u,value_format=u,columns=(k,v)"));
    testutil_check(session->create(session, "index:main:index", "columns=(v),collator=index_coll"));

    printf("adding new record\n");
    testutil_check(session->open_cursor(session, "table:main", NULL, NULL, &cursor));

    ki = 13;
    vi = 17;

    k.data = &ki;
    k.size = sizeof(ki);
    v.data = &vi;
    v.size = sizeof(vi);

    cursor->set_key(cursor, &k);
    cursor->set_value(cursor, &v);
    testutil_check(cursor->insert(cursor));
    testutil_check(cursor->close(cursor));

    printf("positioning index cursor\n");

    testutil_check(session->open_cursor(session, "index:main:index", NULL, NULL, &cursor));
    cursor->set_key(cursor, &v);
    testutil_check(cursor->search(cursor));

    printf("duplicating cursor\n");
    testutil_check(session->open_cursor(session, NULL, cursor, NULL, &cursor1));
    testutil_check(cursor->get_value(cursor, &got));
    testutil_assert(item_to_int(&got) == 17);
    testutil_check(cursor1->get_value(cursor1, &got));
    testutil_assert(item_to_int(&got) == 17);

    testutil_check(session->close(session, NULL));
    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}
