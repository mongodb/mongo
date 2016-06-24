/*-
 * Public Domain 2014-2016 MongoDB, Inc.
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
 * ex_event_handler.c
 *	Demonstrate how to use the WiredTiger event handler mechanism.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wiredtiger.h>

static const char *home;

int handle_wiredtiger_error(
    WT_EVENT_HANDLER *, WT_SESSION *, int, const char *);
int handle_wiredtiger_message(WT_EVENT_HANDLER *, WT_SESSION *, const char *);

/*! [Function event_handler] */
/*
 * Create our own event handler structure to allow us to pass context through
 * to event handler callbacks. For this to work the WiredTiger event handler
 * must appear first in our custom event handler structure.
 */
typedef struct {
	WT_EVENT_HANDLER h;
	const char *app_id;
} CUSTOM_EVENT_HANDLER;

/*
 * handle_wiredtiger_error --
 *	Function to handle error callbacks from WiredTiger.
 */
int
handle_wiredtiger_error(WT_EVENT_HANDLER *handler,
    WT_SESSION *session, int error, const char *message)
{
	CUSTOM_EVENT_HANDLER *custom_handler;

	/* Cast the handler back to our custom handler. */
	custom_handler = (CUSTOM_EVENT_HANDLER *)handler;

	/* Report the error on the console. */
	fprintf(stderr,
	    "app_id %s, thread context %p, error %d, message %s\n",
	    custom_handler->app_id, (void *)session, error, message);

	return (0);
}

/*
 * handle_wiredtiger_message --
 *	Function to handle message callbacks from WiredTiger.
 */
int
handle_wiredtiger_message(
    WT_EVENT_HANDLER *handler, WT_SESSION *session, const char *message)
{
	/* Cast the handler back to our custom handler. */
	printf("app id %s, thread context %p, message %s\n",
	    ((CUSTOM_EVENT_HANDLER *)handler)->app_id,
	    (void *)session, message);

	return (0);
}
/*! [Function event_handler] */

static int
config_event_handler(void)
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	int ret;

	/*! [Configure event_handler] */
	CUSTOM_EVENT_HANDLER event_handler;

	event_handler.h.handle_error = handle_wiredtiger_error;
	event_handler.h.handle_message = handle_wiredtiger_message;
	/* Set handlers to NULL to use the default handler. */
	event_handler.h.handle_progress = NULL;
	event_handler.h.handle_close = NULL;
	event_handler.app_id = "example_event_handler";

	ret = wiredtiger_open(home,
	    (WT_EVENT_HANDLER *)&event_handler, "create", &conn);
	/*! [Configure event_handler] */

	/* Make an invalid API call, to ensure the event handler works. */
	printf("ex_event_handler: expect an error message to follow\n");
	ret = conn->open_session(conn, NULL, "isolation=invalid", &session);

	ret = conn->close(conn, NULL);

	return (ret);
}

int
main(void)
{
	int ret;

	/*
	 * Create a clean test directory for this run of the test program if the
	 * environment variable isn't already set (as is done by make check).
	 */
	if (getenv("WIREDTIGER_HOME") == NULL) {
		home = "WT_HOME";
		(void)system("rm -rf WT_HOME && mkdir WT_HOME");
	} else
		home = NULL;

	ret = config_event_handler();

	return (ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
