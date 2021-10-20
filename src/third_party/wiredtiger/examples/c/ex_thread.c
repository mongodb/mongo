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
 * ex_thread.c
 *	This is an example demonstrating how to create and access a simple
 *	table from multiple threads.
 */

#include "test_util.h"

static const char *home;

#define NUM_THREADS 10

/*! [thread scan] */
static WT_THREAD_RET
scan_thread(void *conn_arg)
{
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_SESSION *session;
    int ret;
    const char *key, *value;

    conn = conn_arg;
    error_check(conn->open_session(conn, NULL, NULL, &session));
    error_check(session->open_cursor(session, "table:access", NULL, NULL, &cursor));

    /* Show all records. */
    while ((ret = cursor->next(cursor)) == 0) {
        error_check(cursor->get_key(cursor, &key));
        error_check(cursor->get_value(cursor, &value));

        printf("Got record: %s : %s\n", key, value);
    }
    if (ret != WT_NOTFOUND)
        fprintf(stderr, "WT_CURSOR.next: %s\n", session->strerror(session, ret));

    return (WT_THREAD_RET_VALUE);
}
/*! [thread scan] */

/*! [thread main] */
int
main(int argc, char *argv[])
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    WT_CURSOR *cursor;
    wt_thread_t threads[NUM_THREADS];
    int i;

    home = example_setup(argc, argv);

    error_check(wiredtiger_open(home, NULL, "create", &conn));

    error_check(conn->open_session(conn, NULL, NULL, &session));
    error_check(session->create(session, "table:access", "key_format=S,value_format=S"));
    error_check(session->open_cursor(session, "table:access", NULL, "overwrite", &cursor));
    cursor->set_key(cursor, "key1");
    cursor->set_value(cursor, "value1");
    error_check(cursor->insert(cursor));
    error_check(session->close(session, NULL));

    for (i = 0; i < NUM_THREADS; i++)
        error_check(__wt_thread_create(NULL, &threads[i], scan_thread, conn));

    for (i = 0; i < NUM_THREADS; i++)
        error_check(__wt_thread_join(NULL, &threads[i]));

    error_check(conn->close(conn, NULL));

    return (EXIT_SUCCESS);
}
/*! [thread main] */
