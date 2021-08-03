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
 * ex_stat.c
 *	This is an example demonstrating how to query database statistics.
 */
#include <test_util.h>

void get_stat(WT_CURSOR *cursor, int stat_field, int64_t *valuep);
void print_cursor(WT_CURSOR *);
void print_database_stats(WT_SESSION *);
void print_derived_stats(WT_SESSION *);
void print_file_stats(WT_SESSION *);
void print_join_cursor_stats(WT_SESSION *);
void print_overflow_pages(WT_SESSION *);
void print_session_stats(WT_SESSION *);

static const char *home;

/*! [statistics display function] */
void
print_cursor(WT_CURSOR *cursor)
{
    const char *desc, *pvalue;
    int64_t value;
    int ret;

    while ((ret = cursor->next(cursor)) == 0) {
        error_check(cursor->get_value(cursor, &desc, &pvalue, &value));
        if (value != 0)
            printf("%s=%s\n", desc, pvalue);
    }
    scan_end_check(ret == WT_NOTFOUND);
}
/*! [statistics display function] */

void
print_database_stats(WT_SESSION *session)
{
    WT_CURSOR *cursor;

    /*! [statistics database function] */
    error_check(session->open_cursor(session, "statistics:", NULL, NULL, &cursor));

    print_cursor(cursor);
    error_check(cursor->close(cursor));
    /*! [statistics database function] */
}

void
print_file_stats(WT_SESSION *session)
{
    WT_CURSOR *cursor;

    /*! [statistics table function] */
    error_check(session->open_cursor(session, "statistics:table:access", NULL, NULL, &cursor));

    print_cursor(cursor);
    error_check(cursor->close(cursor));
    /*! [statistics table function] */
}

void
print_join_cursor_stats(WT_SESSION *session)
{
    WT_CURSOR *idx_cursor, *join_cursor, *stat_cursor;

    error_check(session->create(session, "index:access:idx", "columns=(v)"));
    error_check(session->open_cursor(session, "index:access:idx", NULL, NULL, &idx_cursor));
    error_check(idx_cursor->next(idx_cursor));
    error_check(session->open_cursor(session, "join:table:access", NULL, NULL, &join_cursor));
    error_check(session->join(session, join_cursor, idx_cursor, "compare=gt"));
    print_cursor(join_cursor);

    /*! [statistics join cursor function] */
    error_check(session->open_cursor(session, "statistics:join", join_cursor, NULL, &stat_cursor));

    print_cursor(stat_cursor);
    error_check(stat_cursor->close(stat_cursor));
    /*! [statistics join cursor function] */

    error_check(join_cursor->close(join_cursor));
    error_check(idx_cursor->close(idx_cursor));
}

void
print_session_stats(WT_SESSION *session)
{
    WT_CURSOR *stat_cursor;

    /*! [statistics session function] */
    error_check(session->open_cursor(session, "statistics:session", NULL, NULL, &stat_cursor));

    print_cursor(stat_cursor);
    error_check(stat_cursor->close(stat_cursor));
    /*! [statistics session function] */
}

void
print_overflow_pages(WT_SESSION *session)
{
    /*! [statistics retrieve by key] */
    WT_CURSOR *cursor;
    const char *desc, *pvalue;
    int64_t value;

    error_check(session->open_cursor(session, "statistics:table:access", NULL, NULL, &cursor));

    cursor->set_key(cursor, WT_STAT_DSRC_BTREE_OVERFLOW);
    error_check(cursor->search(cursor));
    error_check(cursor->get_value(cursor, &desc, &pvalue, &value));
    printf("%s=%s\n", desc, pvalue);

    error_check(cursor->close(cursor));
    /*! [statistics retrieve by key] */
}

/*! [statistics calculation helper function] */
void
get_stat(WT_CURSOR *cursor, int stat_field, int64_t *valuep)
{
    const char *desc, *pvalue;

    cursor->set_key(cursor, stat_field);
    error_check(cursor->search(cursor));
    error_check(cursor->get_value(cursor, &desc, &pvalue, valuep));
}
/*! [statistics calculation helper function] */

void
print_derived_stats(WT_SESSION *session)
{
    WT_CURSOR *cursor;

    /*! [statistics calculate open table stats] */
    error_check(session->open_cursor(session, "statistics:table:access", NULL, NULL, &cursor));
    /*! [statistics calculate open table stats] */

    {
        /*! [statistics calculate table fragmentation] */
        int64_t ckpt_size, file_size, percent;
        get_stat(cursor, WT_STAT_DSRC_BLOCK_CHECKPOINT_SIZE, &ckpt_size);
        get_stat(cursor, WT_STAT_DSRC_BLOCK_SIZE, &file_size);

        percent = 0;
        if (file_size != 0)
            percent = 100 * ((file_size - ckpt_size) / file_size);
        printf("Table is %" PRId64 "%% fragmented\n", percent);
        /*! [statistics calculate table fragmentation] */
    }

    {
        /*! [statistics calculate write amplification] */
        int64_t app_insert, app_remove, app_update, fs_writes;

        get_stat(cursor, WT_STAT_DSRC_CURSOR_INSERT_BYTES, &app_insert);
        get_stat(cursor, WT_STAT_DSRC_CURSOR_REMOVE_BYTES, &app_remove);
        get_stat(cursor, WT_STAT_DSRC_CURSOR_UPDATE_BYTES, &app_update);

        get_stat(cursor, WT_STAT_DSRC_CACHE_BYTES_WRITE, &fs_writes);

        if (app_insert + app_remove + app_update != 0)
            printf("Write amplification is %.2lf\n",
              (double)fs_writes / (app_insert + app_remove + app_update));
        /*! [statistics calculate write amplification] */
    }

    error_check(cursor->close(cursor));
}

int
main(int argc, char *argv[])
{
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_SESSION *session;

    home = example_setup(argc, argv);

    error_check(wiredtiger_open(home, NULL, "create,statistics=(all)", &conn));
    error_check(conn->open_session(conn, NULL, NULL, &session));
    error_check(
      session->create(session, "table:access", "key_format=S,value_format=S,columns=(k,v)"));

    error_check(session->open_cursor(session, "table:access", NULL, NULL, &cursor));
    cursor->set_key(cursor, "key");
    cursor->set_value(cursor, "value");
    error_check(cursor->insert(cursor));
    error_check(cursor->close(cursor));

    error_check(session->checkpoint(session, NULL));

    print_database_stats(session);

    print_file_stats(session);

    print_join_cursor_stats(session);

    print_session_stats(session);

    print_overflow_pages(session);

    print_derived_stats(session);

    error_check(conn->close(conn, NULL));

    return (EXIT_SUCCESS);
}
