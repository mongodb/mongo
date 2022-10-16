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
 * JIRA ticket reference: WT-2592 Test case description: This is an adaptation of the join parts of
 * ex_schema.c, but written as a test. Though we have join tests in the Python test suite, the
 * Python API uses raw mode for cursors, so errors that are specific to non-raw mode are undetected
 * in Python. Failure mode: The failure seen in WT-2592 was that no items were returned by a join.
 */

/* The C struct for the data we are storing in a WiredTiger table. */
typedef struct {
    char country[5];
    uint16_t year;
    uint64_t population;
} POP_RECORD;

static POP_RECORD pop_data[] = {{"AU", 1900, 4000000}, {"AU", 1950, 8267337},
  {"AU", 2000, 19053186}, {"CAN", 1900, 5500000}, {"CAN", 1950, 14011422}, {"CAN", 2000, 31099561},
  {"UK", 1900, 369000000}, {"UK", 1950, 50127000}, {"UK", 2000, 59522468}, {"USA", 1900, 76212168},
  {"USA", 1950, 150697361}, {"USA", 2000, 301279593}, {"", 0, 0}};

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(int argc, char *argv[])
{
    POP_RECORD *p;
    TEST_OPTS *opts, _opts;
    WT_CURSOR *country_cursor, *country_cursor2, *cursor, *join_cursor, *subjoin_cursor,
      *year_cursor;
    WT_SESSION *session;
    const char *country, *tablename;
    char countryuri[256], joinuri[256], yearuri[256];
    uint64_t population, recno;
    uint16_t year;
    int count, ret;

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));
    testutil_make_work_dir(opts->home);

    tablename = strchr(opts->uri, ':');
    testutil_assert(tablename != NULL);
    tablename++;
    testutil_check(__wt_snprintf(countryuri, sizeof(countryuri), "index:%s:country", tablename));
    testutil_check(__wt_snprintf(yearuri, sizeof(yearuri), "index:%s:year", tablename));
    testutil_check(__wt_snprintf(joinuri, sizeof(joinuri), "join:%s", opts->uri));

    testutil_check(wiredtiger_open(opts->home, NULL,
      "create,cache_size=200M,statistics=(all),statistics_log=(json,on_close,wait=1)",
      &opts->conn));
    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session));
    testutil_check(session->create(
      session, opts->uri, "key_format=r,value_format=5sHQ,columns=(id,country,year,population)"));

    /* Create an index with a simple key. */
    testutil_check(session->create(session, countryuri, "columns=(country)"));

    /* Create an immutable index. */
    testutil_check(session->create(session, yearuri, "columns=(year),immutable"));

    /* Insert the records into the table. */
    testutil_check(session->open_cursor(session, opts->uri, NULL, "append", &cursor));
    count = 1;
    for (p = pop_data; p->year != 0; p++) {
        cursor->set_key(cursor, count);
        cursor->set_value(cursor, p->country, p->year, p->population);
        testutil_check(cursor->insert(cursor));
        count++;
    }
    testutil_check(cursor->close(cursor));

    /* Open cursors needed by the join. */
    testutil_check(session->open_cursor(session, joinuri, NULL, NULL, &join_cursor));
    testutil_check(session->open_cursor(session, countryuri, NULL, NULL, &country_cursor));
    testutil_check(session->open_cursor(session, yearuri, NULL, NULL, &year_cursor));

    /* select values WHERE country == "AU" AND year > 1900 */
    country_cursor->set_key(country_cursor, "AU\0\0\0");
    testutil_check(country_cursor->search(country_cursor));
    testutil_check(session->join(session, join_cursor, country_cursor, "compare=eq,count=10"));
    year_cursor->set_key(year_cursor, (uint16_t)1900);
    testutil_check(year_cursor->search(year_cursor));
    testutil_check(
      session->join(session, join_cursor, year_cursor, "compare=gt,count=10,strategy=bloom"));

    count = 0;
    /* List the values that are joined */
    while ((ret = join_cursor->next(join_cursor)) == 0) {
        testutil_check(join_cursor->get_key(join_cursor, &recno));
        testutil_check(join_cursor->get_value(join_cursor, &country, &year, &population));
        printf("ID %" PRIu64, recno);
        printf(
          ": country %s, year %" PRIu16 ", population %" PRIu64 "\n", country, year, population);
        count++;
    }
    testutil_assert(ret == WT_NOTFOUND);
    testutil_assert(count == 2);

    testutil_check(join_cursor->close(join_cursor));
    testutil_check(year_cursor->close(year_cursor));
    testutil_check(country_cursor->close(country_cursor));

    /* Open cursors needed by the join. */
    testutil_check(session->open_cursor(session, joinuri, NULL, NULL, &join_cursor));
    testutil_check(session->open_cursor(session, joinuri, NULL, NULL, &subjoin_cursor));
    testutil_check(session->open_cursor(session, countryuri, NULL, NULL, &country_cursor));
    testutil_check(session->open_cursor(session, countryuri, NULL, NULL, &country_cursor2));
    testutil_check(session->open_cursor(session, yearuri, NULL, NULL, &year_cursor));

    /*
     * select values WHERE (country == "AU" OR country == "UK")
     *                     AND year > 1900
     *
     * First, set up the join representing the country clause.
     */
    country_cursor->set_key(country_cursor, "AU\0\0\0");
    testutil_check(country_cursor->search(country_cursor));
    testutil_check(
      session->join(session, subjoin_cursor, country_cursor, "operation=or,compare=eq,count=10"));
    country_cursor2->set_key(country_cursor2, "UK\0\0\0");
    testutil_check(country_cursor2->search(country_cursor2));
    testutil_check(
      session->join(session, subjoin_cursor, country_cursor2, "operation=or,compare=eq,count=10"));

    /* Join that to the top join, and add the year clause */
    testutil_check(session->join(session, join_cursor, subjoin_cursor, NULL));
    year_cursor->set_key(year_cursor, (uint16_t)1900);
    testutil_check(year_cursor->search(year_cursor));
    testutil_check(
      session->join(session, join_cursor, year_cursor, "compare=gt,count=10,strategy=bloom"));

    count = 0;
    /* List the values that are joined */
    while ((ret = join_cursor->next(join_cursor)) == 0) {
        testutil_check(join_cursor->get_key(join_cursor, &recno));
        testutil_check(join_cursor->get_value(join_cursor, &country, &year, &population));
        printf("ID %" PRIu64, recno);
        printf(
          ": country %s, year %" PRIu16 ", population %" PRIu64 "\n", country, year, population);
        count++;
    }
    testutil_assert(ret == WT_NOTFOUND);
    testutil_assert(count == 4);

    testutil_check(join_cursor->close(join_cursor));
    testutil_check(subjoin_cursor->close(subjoin_cursor));
    testutil_check(country_cursor->close(country_cursor));
    testutil_check(country_cursor2->close(country_cursor2));
    testutil_check(year_cursor->close(year_cursor));
    testutil_check(session->close(session, NULL));

    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}
