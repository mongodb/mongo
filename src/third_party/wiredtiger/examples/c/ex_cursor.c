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
 * ex_cursor.c
 *	This is an example demonstrating some cursor types and operations.
 */
#include <test_util.h>

int cursor_reset(WT_CURSOR *cursor);
int cursor_forward_scan(WT_CURSOR *cursor);
int cursor_reverse_scan(WT_CURSOR *cursor);
int cursor_search(WT_CURSOR *cursor);
int cursor_search_near(WT_CURSOR *cursor);
int cursor_insert(WT_CURSOR *cursor);
int cursor_update(WT_CURSOR *cursor);
int cursor_remove(WT_CURSOR *cursor);
int cursor_largest_key(WT_CURSOR *cursor);
int version_cursor_dump(WT_CURSOR *cursor);

static const char *home;

/*! [cursor next] */
int
cursor_forward_scan(WT_CURSOR *cursor)
{
    const char *key, *value;
    int ret;

    while ((ret = cursor->next(cursor)) == 0) {
        error_check(cursor->get_key(cursor, &key));
        error_check(cursor->get_value(cursor, &value));
    }
    scan_end_check(ret == WT_NOTFOUND);

    return (0);
}
/*! [cursor next] */

/*! [cursor prev] */
int
cursor_reverse_scan(WT_CURSOR *cursor)
{
    const char *key, *value;
    int ret;

    while ((ret = cursor->prev(cursor)) == 0) {
        error_check(cursor->get_key(cursor, &key));
        error_check(cursor->get_value(cursor, &value));
    }
    scan_end_check(ret == WT_NOTFOUND);

    return (0);
}
/*! [cursor prev] */

/*! [cursor reset] */
int
cursor_reset(WT_CURSOR *cursor)
{
    return (cursor->reset(cursor));
}
/*! [cursor reset] */

/*! [cursor search] */
int
cursor_search(WT_CURSOR *cursor)
{
    const char *value;

    cursor->set_key(cursor, "foo");

    error_check(cursor->search(cursor));
    error_check(cursor->get_value(cursor, &value));

    return (0);
}
/*! [cursor search] */

/*! [cursor search near] */
int
cursor_search_near(WT_CURSOR *cursor)
{
    const char *key, *value;
    int exact;

    cursor->set_key(cursor, "foo");

    error_check(cursor->search_near(cursor, &exact));
    switch (exact) {
    case -1: /* Returned key smaller than search key */
        error_check(cursor->get_key(cursor, &key));
        break;
    case 0: /* Exact match found */
        break;
    case 1: /* Returned key larger than search key */
        error_check(cursor->get_key(cursor, &key));
        break;
    }
    error_check(cursor->get_value(cursor, &value));

    return (0);
}
/*! [cursor search near] */

/*! [cursor insert] */
int
cursor_insert(WT_CURSOR *cursor)
{
    cursor->set_key(cursor, "foo");
    cursor->set_value(cursor, "bar");

    return (cursor->insert(cursor));
}
/*! [cursor insert] */

/*! [cursor update] */
int
cursor_update(WT_CURSOR *cursor)
{
    cursor->set_key(cursor, "foo");
    cursor->set_value(cursor, "newbar");

    return (cursor->update(cursor));
}
/*! [cursor update] */

/*! [cursor remove] */
int
cursor_remove(WT_CURSOR *cursor)
{
    cursor->set_key(cursor, "foo");
    return (cursor->remove(cursor));
}
/*! [cursor remove] */

/*! [cursor largest key] */
int
cursor_largest_key(WT_CURSOR *cursor)
{
    return (cursor->largest_key(cursor));
}
/*! [cursor largest key] */

/*! [version cursor dump] */
int
version_cursor_dump(WT_CURSOR *cursor)
{
    wt_timestamp_t start_ts, start_durable_ts, stop_ts, stop_durable_ts;
    uint64_t start_txnid, stop_txnid;
    uint8_t flags, location, prepare, type;
    const char *value;
    cursor->set_key(cursor, "foo");
    error_check(cursor->search(cursor));
    error_check(cursor->get_value(cursor, &start_txnid, &start_ts, &start_durable_ts, &stop_txnid,
      &stop_ts, &stop_durable_ts, &type, &prepare, &flags, &location, &value));

    return (0);
}
/*! [version cursor dump] */

int
main(int argc, char *argv[])
{
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_SESSION *session;

    home = example_setup(argc, argv);

    /* Open a connection to the database, creating it if necessary. */
    error_check(wiredtiger_open(home, NULL, "create,statistics=(fast)", &conn));

    /* Open a session for the current thread's work. */
    error_check(conn->open_session(conn, NULL, NULL, &session));

    error_check(session->create(session, "table:world",
      "key_format=r,value_format=5sii,columns=(id,country,population,area)"));

    /*! [open cursor #1] */
    error_check(session->open_cursor(session, "table:world", NULL, NULL, &cursor));
    /*! [open cursor #1] */

    /*! [open cursor #2] */
    error_check(
      session->open_cursor(session, "table:world(country,population)", NULL, NULL, &cursor));
    /*! [open cursor #2] */

    /*! [open cursor #3] */
    error_check(session->open_cursor(session, "statistics:", NULL, NULL, &cursor));
    /*! [open cursor #3] */

    /* Create a simple string table to illustrate basic operations. */
    error_check(session->create(session, "table:map", "key_format=S,value_format=S"));
    error_check(session->open_cursor(session, "table:map", NULL, NULL, &cursor));
    error_check(cursor_insert(cursor));
    error_check(cursor_reset(cursor));
    error_check(cursor_forward_scan(cursor));
    error_check(cursor_reset(cursor));
    error_check(cursor_reverse_scan(cursor));
    error_check(cursor_search_near(cursor));
    error_check(cursor_update(cursor));
    error_check(cursor_remove(cursor));
    error_check(cursor_insert(cursor));
    error_check(cursor_largest_key(cursor));
    error_check(cursor->close(cursor));

    /* Create a version cursor. */
    error_check(session->begin_transaction(session, "read_timestamp=1"));
    error_check(
      session->open_cursor(session, "file:map.wt", NULL, "debug=(dump_version=true)", &cursor));
    error_check(version_cursor_dump(cursor));
    error_check(cursor->close(cursor));

    /* Note: closing the connection implicitly closes open session(s). */
    error_check(conn->close(conn, NULL));

    return (EXIT_SUCCESS);
}
