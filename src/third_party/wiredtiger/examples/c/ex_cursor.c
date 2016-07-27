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
 * ex_cursor.c
 *	This is an example demonstrating some cursor types and operations.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wiredtiger.h>

int cursor_reset(WT_CURSOR *cursor);
int cursor_forward_scan(WT_CURSOR *cursor);
int cursor_reverse_scan(WT_CURSOR *cursor);
int cursor_search(WT_CURSOR *cursor);
int cursor_search_near(WT_CURSOR *cursor);
int cursor_insert(WT_CURSOR *cursor);
int cursor_update(WT_CURSOR *cursor);
int cursor_remove(WT_CURSOR *cursor);

static const char *home;

/*! [cursor next] */
int
cursor_forward_scan(WT_CURSOR *cursor)
{
	const char *key, *value;
	int ret;

	while ((ret = cursor->next(cursor)) == 0) {
		ret = cursor->get_key(cursor, &key);
		ret = cursor->get_value(cursor, &value);
	}
	return (ret);
}
/*! [cursor next] */

/*! [cursor prev] */
int
cursor_reverse_scan(WT_CURSOR *cursor)
{
	const char *key, *value;
	int ret;

	while ((ret = cursor->prev(cursor)) == 0) {
		ret = cursor->get_key(cursor, &key);
		ret = cursor->get_value(cursor, &value);
	}
	return (ret);
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
	int ret;

	cursor->set_key(cursor, "foo");

	if ((ret = cursor->search(cursor)) == 0)
		ret = cursor->get_value(cursor, &value);

	return (ret);
}
/*! [cursor search] */

/*! [cursor search near] */
int
cursor_search_near(WT_CURSOR *cursor)
{
	const char *key, *value;
	int exact, ret;

	cursor->set_key(cursor, "foo");

	if ((ret = cursor->search_near(cursor, &exact)) == 0) {
		switch (exact) {
		case -1:	/* Returned key smaller than search key */
			ret = cursor->get_key(cursor, &key);
			break;
		case 0:		/* Exact match found */
			break;
		case 1:		/* Returned key larger than search key */
			ret = cursor->get_key(cursor, &key);
			break;
		}

		ret = cursor->get_value(cursor, &value);
	}

	return (ret);
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

int
main(void)
{
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
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
	if ((ret = wiredtiger_open(
	    home, NULL, "create,statistics=(fast)", &conn)) != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home == NULL ? "." : home, wiredtiger_strerror(ret));

	/* Open a session for the current thread's work. */
	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		fprintf(stderr, "Error opening a session on %s: %s\n",
		    home == NULL ? "." : home, wiredtiger_strerror(ret));

	ret = session->create(session, "table:world",
	    "key_format=r,value_format=5sii,"
	    "columns=(id,country,population,area)");

	/*! [open cursor #1] */
	ret = session->open_cursor(session, "table:world", NULL, NULL, &cursor);
	/*! [open cursor #1] */

	/*! [open cursor #2] */
	ret = session->open_cursor(session,
	    "table:world(country,population)", NULL, NULL, &cursor);
	/*! [open cursor #2] */

	/*! [open cursor #3] */
	ret = session->open_cursor(session, "statistics:", NULL, NULL, &cursor);
	/*! [open cursor #3] */

	/* Create a simple string table to illustrate basic operations. */
	ret = session->create(session, "table:map",
	    "key_format=S,value_format=S");
	ret = session->open_cursor(session, "table:map", NULL, NULL, &cursor);
	ret = cursor_insert(cursor);
	ret = cursor_reset(cursor);
	ret = cursor_forward_scan(cursor);
	ret = cursor_reset(cursor);
	ret = cursor_reverse_scan(cursor);
	ret = cursor_search_near(cursor);
	ret = cursor_update(cursor);
	ret = cursor_remove(cursor);
	ret = cursor->close(cursor);

	/* Note: closing the connection implicitly closes open session(s). */
	if ((ret = conn->close(conn, NULL)) != 0) {
		fprintf(stderr, "Error closing %s: %s\n",
		    home == NULL ? "." : home, wiredtiger_strerror(ret));
		return (EXIT_FAILURE);
	}

	return (EXIT_SUCCESS);
}
