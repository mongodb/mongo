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
 * ex_verbose.c
 *	Demonstrate how to configure verbose messaging in WiredTiger.
 */
#include <test_util.h>

static const char *home;

int handle_wiredtiger_message(WT_EVENT_HANDLER *, WT_SESSION *, const char *);

/*
 * handle_wiredtiger_message --
 *     Function to handle message callbacks from WiredTiger.
 */
int
handle_wiredtiger_message(WT_EVENT_HANDLER *handler, WT_SESSION *session, const char *message)
{
    /* Unused parameters */
    (void)handler;
    printf("WiredTiger Message - Session: %p, Message: %s\n", (void *)session, message);

    return (0);
}

static void
config_verbose(void)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    WT_CURSOR *cursor;

    WT_EVENT_HANDLER event_handler;

    event_handler.handle_message = handle_wiredtiger_message;
    event_handler.handle_error = NULL;
    event_handler.handle_progress = NULL;
    event_handler.handle_close = NULL;
    event_handler.handle_general = NULL;

    /*! [Configure verbose_messaging] */
    error_check(wiredtiger_open(
      home, (WT_EVENT_HANDLER *)&event_handler, "create,verbose=[api:1,version,write:0]", &conn));
    /*! [Configure verbose_messaging] */

    /* Make a series of API calls, to ensure verbose messages are produced. */
    printf("ex_verbose: expect verbose messages to follow:\n");
    error_check(conn->open_session(conn, NULL, NULL, &session));
    error_check(session->create(session, "table:verbose", "key_format=S,value_format=S"));
    error_check(session->open_cursor(session, "table:verbose", NULL, NULL, &cursor));
    cursor->set_key(cursor, "foo");
    cursor->set_value(cursor, "bar");
    error_check(cursor->insert(cursor));
    error_check(cursor->close(cursor));
    printf("ex_verbose: end of verbose messages\n");

    error_check(conn->close(conn, NULL));
}

int
main(int argc, char *argv[])
{
    home = example_setup(argc, argv);

    config_verbose();

    return (EXIT_SUCCESS);
}
