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
 * JIRA ticket reference: WT-2447
 *
 * Test case description: This test case is adapted from the submitted test
 * program in the JIRA ticket. We create a database of 10,000 entries, with
 * every key i having pair of values (i, i).  Create indices on both values,
 * and establish a join: table.v1 >= 5000 AND table.v2 < 5001.  There's a
 * Bloom filter on v2.  We expect that although we iterate from 5000 to
 * 10000, we'll only have accesses to the main table for key 5000, as
 * 5001-10000 will generally not be in the Bloom filter.  For key 5000,
 * we technically have two accesses to the main table - one occurs when we
 * see key 5000 is in the Bloom filter, and we need to do a full test, we
 * make an access to the projection table:tablename(v2), that's just to get
 * the value of v2, which we'll check by comparison to the cursor at 5001.
 * That counts as a main table access, and when we see it is satisfied and
 * return the complete set of values, we'll access the main table with the
 * full projection (that's the second main table access).
 *
 * Failure mode: Before fixes of WT-2447, we saw lots of accesses to the main
 * table.
 */

#define N_RECORDS 10000

/*
 * get_stat_total --
 *     TODO: Add a comment describing this function.
 */
static void
get_stat_total(WT_SESSION *session, WT_CURSOR *jcursor, const char *descmatch, uint64_t *pval)
{
    WT_CURSOR *statcursor;
    WT_DECL_RET;
    uint64_t val;
    char *desc, *valstr;
    bool match;

    match = false;
    *pval = 0;
    testutil_check(session->open_cursor(session, "statistics:join", jcursor, NULL, &statcursor));

    while ((ret = statcursor->next(statcursor)) == 0) {
        testutil_assert(statcursor->get_value(statcursor, &desc, &valstr, &val) == 0);

        printf("statistics: %s: %s: %" PRIu64 "\n", desc, valstr, val);

        if (strstr(desc, descmatch) != NULL) {
            *pval += val;
            match = true;
        }
    }
    testutil_assert(ret == WT_NOTFOUND);
    testutil_check(statcursor->close(statcursor));
    testutil_assert(match);
}

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;
    WT_CURSOR *cursor1, *cursor2, *jcursor;
    WT_ITEM d;
    WT_SESSION *session;
    uint64_t maincount, i64;
    int half, i, j;
    char bloom_cfg[128], index1uri[256], index2uri[256], joinuri[256], table_cfg[128];
    const char *tablename;

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    /* 0 isn't a valid table_type; use rows by default */
    opts->table_type = TABLE_ROW;
    testutil_check(testutil_parse_opts(argc, argv, opts));
    testutil_make_work_dir(opts->home);

    switch (opts->table_type) {
    case TABLE_COL:
        printf("Table type: columns\n");
        break;
    case TABLE_FIX:
        testutil_die(ENOTSUP, "Fixed-length column store not supported");
    case TABLE_ROW:
        printf("Table type: rows\n");
        break;
    }

    tablename = strchr(opts->uri, ':');
    testutil_assert(tablename != NULL);
    tablename++;
    testutil_check(__wt_snprintf(index1uri, sizeof(index1uri), "index:%s:index1", tablename));
    testutil_check(__wt_snprintf(index2uri, sizeof(index2uri), "index:%s:index2", tablename));
    testutil_check(__wt_snprintf(joinuri, sizeof(joinuri), "join:%s", opts->uri));

    testutil_check(wiredtiger_open(opts->home, NULL, "statistics=(all),create", &opts->conn));
    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session));

    testutil_check(__wt_snprintf(table_cfg, sizeof(table_cfg),
      "key_format=%s,value_format=iiu,columns=(k,v1,v2,d)",
      opts->table_type == TABLE_ROW ? "i" : "r"));
    testutil_check(session->create(session, opts->uri, table_cfg));
    testutil_check(session->create(session, index1uri, "columns=(v1)"));
    testutil_check(session->create(session, index2uri, "columns=(v2)"));

    testutil_check(session->open_cursor(session, opts->uri, NULL, NULL, &cursor1));

    d.size = 4100;
    d.data = dmalloc(d.size);
    memset((char *)d.data, 7, d.size);

    for (i = 1; i < N_RECORDS + 1; ++i) {
        cursor1->set_key(cursor1, i);
        cursor1->set_value(cursor1, i, i, &d);
        testutil_check(cursor1->insert(cursor1));
    }

    free((void *)d.data);

    testutil_check(opts->conn->close(opts->conn, NULL));
    testutil_check(
      wiredtiger_open(opts->home, NULL, "statistics=(all),create,cache_size=1GB", &opts->conn));
    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session));

    testutil_check(session->open_cursor(session, index1uri, NULL, NULL, &cursor1));
    testutil_check(session->open_cursor(session, index2uri, NULL, NULL, &cursor2));

    half = N_RECORDS / 2;
    cursor1->set_key(cursor1, half);
    testutil_check(cursor1->search(cursor1));

    cursor2->set_key(cursor2, half + 1);
    testutil_check(cursor2->search(cursor2));

    testutil_check(
      __wt_snprintf(bloom_cfg, sizeof(bloom_cfg), "compare=lt,strategy=bloom,count=%d", half));

    testutil_check(session->open_cursor(session, joinuri, NULL, NULL, &jcursor));
    testutil_check(session->join(session, jcursor, cursor1, "compare=ge"));
    testutil_check(session->join(session, jcursor, cursor2, bloom_cfg));

    /* Expect one value returned */
    testutil_assert(jcursor->next(jcursor) == 0);
    i = 0;
    if (opts->table_type == TABLE_ROW)
        testutil_assert(jcursor->get_key(jcursor, &i) == 0);
    else {
        testutil_assert(jcursor->get_key(jcursor, &i64) == 0);
        i = (int)i64;
    }
    testutil_assert(i == half);
    i = j = 0;
    memset(&d, 0, sizeof(d));
    testutil_assert(jcursor->get_value(jcursor, &i, &j, &d) == 0);
    testutil_assert(i == half);
    testutil_assert(j == half);
    testutil_assert(d.size == 4100);
    for (i = 0; i < 4100; i++)
        testutil_assert(((char *)d.data)[i] == 7);

    testutil_assert(jcursor->next(jcursor) == WT_NOTFOUND);

    /*
     * Make sure there have been 2 accesses to the main table, explained in the discussion above.
     */
    get_stat_total(session, jcursor, "accesses to the main table", &maincount);
    testutil_assert(maincount == 2);

    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}
