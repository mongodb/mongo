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
 *
 * ex_schema.c
 *	This is an example application demonstrating how to create and access
 *	tables using a schema.
 */
#include <test_util.h>

static const char *home;

/*! [schema declaration] */
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
/*! [schema declaration] */

int
main(int argc, char *argv[])
{
    POP_RECORD *p;
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_SESSION *session;
    const char *country;
    uint64_t recno, population;
    uint16_t year;
    int ret;

    home = example_setup(argc, argv);

    error_check(wiredtiger_open(home, NULL, "create,statistics=(fast)", &conn));

    error_check(conn->open_session(conn, NULL, NULL, &session));

    /*! [Create a table with column groups] */
    /*
     * Create the population table. Keys are record numbers, the format for values is (5-byte
     * string, uint16_t, uint64_t). See ::wiredtiger_struct_pack for details of the format strings.
     */
    error_check(session->create(session, "table:poptable",
      "key_format=r,value_format=5sHQ,columns=(id,country,year,population),colgroups=(main,"
      "population)"));

    /*
     * Create two column groups: a primary column group with the country code, year and population
     * (named "main"), and a population column group with the population by itself (named
     * "population").
     */
    error_check(
      session->create(session, "colgroup:poptable:main", "columns=(country,year,population)"));
    error_check(session->create(session, "colgroup:poptable:population", "columns=(population)"));
    /*! [Create a table with column groups] */

    /*! [Create an index] */
    /* Create an index with a simple key. */
    error_check(session->create(session, "index:poptable:country", "columns=(country)"));
    /*! [Create an index] */

    /*! [Create an index with a composite key] */
    /* Create an index with a composite key (country,year). */
    error_check(
      session->create(session, "index:poptable:country_plus_year", "columns=(country,year)"));
    /*! [Create an index with a composite key] */

    /*! [Create an immutable index] */
    /* Create an immutable index. */
    error_check(
      session->create(session, "index:poptable:immutable_year", "columns=(year),immutable"));
    /*! [Create an immutable index] */

    /* Insert the records into the table. */
    error_check(session->open_cursor(session, "table:poptable", NULL, "append", &cursor));
    for (p = pop_data; p->year != 0; p++) {
        cursor->set_value(cursor, p->country, p->year, p->population);
        error_check(cursor->insert(cursor));
    }
    error_check(cursor->close(cursor));

    /* Update records in the table. */
    error_check(session->open_cursor(session, "table:poptable", NULL, NULL, &cursor));
    while ((ret = cursor->next(cursor)) == 0) {
        error_check(cursor->get_key(cursor, &recno));
        error_check(cursor->get_value(cursor, &country, &year, &population));
        cursor->set_value(cursor, country, year, population + 1);
        error_check(cursor->update(cursor));
    }
    scan_end_check(ret == WT_NOTFOUND);
    error_check(cursor->close(cursor));

    /* List the records in the table. */
    error_check(session->open_cursor(session, "table:poptable", NULL, NULL, &cursor));
    while ((ret = cursor->next(cursor)) == 0) {
        error_check(cursor->get_key(cursor, &recno));
        error_check(cursor->get_value(cursor, &country, &year, &population));
        printf("ID %" PRIu64, recno);
        printf(
          ": country %s, year %" PRIu16 ", population %" PRIu64 "\n", country, year, population);
    }
    scan_end_check(ret == WT_NOTFOUND);
    error_check(cursor->close(cursor));

    /*! [List the records in the table using raw mode.] */
    /* List the records in the table using raw mode. */
    error_check(session->open_cursor(session, "table:poptable", NULL, "raw", &cursor));
    while ((ret = cursor->next(cursor)) == 0) {
        WT_ITEM key, value;

        error_check(cursor->get_key(cursor, &key));
        error_check(wiredtiger_struct_unpack(session, key.data, key.size, "r", &recno));
        printf("ID %" PRIu64, recno);

        error_check(cursor->get_value(cursor, &value));
        error_check(wiredtiger_struct_unpack(
          session, value.data, value.size, "5sHQ", &country, &year, &population));
        printf(
          ": country %s, year %" PRIu16 ", population %" PRIu64 "\n", country, year, population);
    }
    scan_end_check(ret == WT_NOTFOUND);
    /*! [List the records in the table using raw mode.] */
    error_check(cursor->close(cursor));

    /*! [Read population from the primary column group] */
    /*
     * Open a cursor on the main column group, and return the information for a particular country.
     */
    error_check(session->open_cursor(session, "colgroup:poptable:main", NULL, NULL, &cursor));
    cursor->set_key(cursor, 2);
    error_check(cursor->search(cursor));
    error_check(cursor->get_value(cursor, &country, &year, &population));
    printf(
      "ID 2: country %s, year %" PRIu16 ", population %" PRIu64 "\n", country, year, population);
    /*! [Read population from the primary column group] */
    error_check(cursor->close(cursor));

    /*! [Read population from the standalone column group] */
    /*
     * Open a cursor on the population column group, and return the population of a particular
     * country.
     */
    error_check(session->open_cursor(session, "colgroup:poptable:population", NULL, NULL, &cursor));
    cursor->set_key(cursor, 2);
    error_check(cursor->search(cursor));
    error_check(cursor->get_value(cursor, &population));
    printf("ID 2: population %" PRIu64 "\n", population);
    /*! [Read population from the standalone column group] */
    error_check(cursor->close(cursor));

    /*! [Search in a simple index] */
    /* Search in a simple index. */
    error_check(session->open_cursor(session, "index:poptable:country", NULL, NULL, &cursor));
    cursor->set_key(cursor, "AU\0\0\0");
    error_check(cursor->search(cursor));
    error_check(cursor->get_value(cursor, &country, &year, &population));
    printf("AU: country %s, year %" PRIu16 ", population %" PRIu64 "\n", country, year, population);
    /*! [Search in a simple index] */
    error_check(cursor->close(cursor));

    /*! [Search in a composite index] */
    /* Search in a composite index. */
    error_check(
      session->open_cursor(session, "index:poptable:country_plus_year", NULL, NULL, &cursor));
    cursor->set_key(cursor, "USA\0\0", (uint16_t)1900);
    error_check(cursor->search(cursor));
    error_check(cursor->get_value(cursor, &country, &year, &population));
    printf(
      "US 1900: country %s, year %" PRIu16 ", population %" PRIu64 "\n", country, year, population);
    /*! [Search in a composite index] */
    error_check(cursor->close(cursor));

    error_check(conn->close(conn, NULL));

    return (EXIT_SUCCESS);
}
