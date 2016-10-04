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
 * ex_thread.c
 *	This is an example demonstrating how to create and access a simple
 *	table from multiple threads.
 */

#ifndef _WIN32
#include <pthread.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "windows_shim.h"
#endif

#include <wiredtiger.h>

static const char *home;

void *scan_thread(void *arg);

#define	NUM_THREADS	10

/*! [thread scan] */
void *
scan_thread(void *conn_arg)
{
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	const char *key, *value;
	int ret;

	conn = conn_arg;
	ret = conn->open_session(conn, NULL, NULL, &session);
	ret = session->open_cursor(
	    session, "table:access", NULL, NULL, &cursor);

	/* Show all records. */
	while ((ret = cursor->next(cursor)) == 0) {
		ret = cursor->get_key(cursor, &key);
		ret = cursor->get_value(cursor, &value);

		printf("Got record: %s : %s\n", key, value);
	}
	if (ret != WT_NOTFOUND)
		fprintf(stderr,
		    "WT_CURSOR.next: %s\n", session->strerror(session, ret));

	return (NULL);
}
/*! [thread scan] */

/*! [thread main] */
int
main(void)
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	WT_CURSOR *cursor;
	pthread_t threads[NUM_THREADS];
	int i, ret;

	/*
	 * Create a clean test directory for this run of the test program if the
	 * environment variable isn't already set (as is done by make check).
	 */
	if (getenv("WIREDTIGER_HOME") == NULL) {
		home = "WT_HOME";
		ret = system("rm -rf WT_HOME && mkdir WT_HOME");
	} else
		home = NULL;

	if ((ret = wiredtiger_open(home, NULL, "create", &conn)) != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home == NULL ? "." : home, wiredtiger_strerror(ret));
	/* Note: further error checking omitted for clarity. */

	ret = conn->open_session(conn, NULL, NULL, &session);
	ret = session->create(session, "table:access",
	    "key_format=S,value_format=S");
	ret = session->open_cursor(session, "table:access", NULL,
	    "overwrite", &cursor);
	cursor->set_key(cursor, "key1");
	cursor->set_value(cursor, "value1");
	ret = cursor->insert(cursor);
	ret = session->close(session, NULL);

	for (i = 0; i < NUM_THREADS; i++)
		ret = pthread_create(&threads[i], NULL, scan_thread, conn);

	for (i = 0; i < NUM_THREADS; i++)
		ret = pthread_join(threads[i], NULL);

	ret = conn->close(conn, NULL);

	return (ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
/*! [thread main] */
