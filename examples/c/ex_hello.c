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
 * ex_hello.c
 *	This is an example demonstrating how to create and connect to a
 *	database.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wiredtiger.h>

static const char *home;

int
main(void)
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	int ret;

	/*
	 * Create a clean test directory for this run of the test program if the
	 * environment variable isn't already set (as is done by make check).
	 */
	if (getenv("WIREDTIGER_HOME") == NULL) {
		home = "WT_HOME";
		ret = system("rm -rf WT_HOME && mkdir WT_HOME");
	} else
		home = NULL;

	/* Open a connection to the database, creating it if necessary. */
	if ((ret = wiredtiger_open(home, NULL, "create", &conn)) != 0) {
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home == NULL ? "." : home, wiredtiger_strerror(ret));
		return (EXIT_FAILURE);
	}

	/* Open a session for the current thread's work. */
	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0) {
		fprintf(stderr, "Error opening a session on %s: %s\n",
		    home == NULL ? "." : home, wiredtiger_strerror(ret));
		return (EXIT_FAILURE);
	}

	/* Do some work... */

	/* Note: closing the connection implicitly closes open session(s). */
	if ((ret = conn->close(conn, NULL)) != 0) {
		fprintf(stderr, "Error closing %s: %s\n",
		    home == NULL ? "." : home, wiredtiger_strerror(ret));
		return (EXIT_FAILURE);
	}

	return (EXIT_SUCCESS);
}
