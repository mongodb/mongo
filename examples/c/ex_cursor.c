/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 *
 * ex_cursor.c
 *	This is an example demonstrating some cursor types and operations.
 */

#include <stdio.h>
#include <string.h>

#include <wiredtiger.h>

int cursor_first(WT_CURSOR *cursor);
int cursor_last(WT_CURSOR *cursor);
int cursor_forward_scan(WT_CURSOR *cursor);
int cursor_reverse_scan(WT_CURSOR *cursor);
int cursor_search(WT_CURSOR *cursor);
int cursor_search_near(WT_CURSOR *cursor);
int cursor_insert(WT_CURSOR *cursor);
int cursor_update(WT_CURSOR *cursor);
int cursor_remove(WT_CURSOR *cursor);

const char *home = "WT_TEST";

int
cursor_first(WT_CURSOR *cursor)
{
	const char *key, *value;
	int ret;

	if ((ret = cursor->first(cursor)) == 0) {
		ret = cursor->get_key(cursor, &key);
		ret = cursor->get_value(cursor, &value);
	}
	return (ret);
}

int
cursor_last(WT_CURSOR *cursor)
{
	const char *key, *value;
	int ret;

	if ((ret = cursor->last(cursor)) == 0) {
		ret = cursor->get_key(cursor, &key);
		ret = cursor->get_value(cursor, &value);
	}
	return (ret);
}

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

int
cursor_search(WT_CURSOR *cursor)
{
	const char *value;
	int ret;

	cursor->set_key(cursor, "foo");

	if ((ret = cursor->search(cursor)) != 0)
		ret = cursor->get_value(cursor, &value);

	return (ret);
}

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

int
cursor_insert(WT_CURSOR *cursor)
{
	cursor->set_key(cursor, "foo");
	cursor->set_value(cursor, "bar");

	return (cursor->insert(cursor));
}

int
cursor_update(WT_CURSOR *cursor)
{
	cursor->set_key(cursor, "foo");
	cursor->set_value(cursor, "newbar");

	return (cursor->update(cursor));
}

int
cursor_remove(WT_CURSOR *cursor)
{
	cursor->set_key(cursor, "foo");
	return (cursor->remove(cursor));
}

int main(void)
{
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	int ret;

	/* Open a connection to the database, creating it if necessary. */
	if ((ret = wiredtiger_open(home, NULL, "create", &conn)) != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));

	/* Open a session for the current thread's work. */
	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		fprintf(stderr, "Error opening a session on %s: %s\n",
		    home, wiredtiger_strerror(ret));

	ret = session->create(session, "table:world",
	    "key_format=r,value_format=5sii,"
	    "columns=(id,country,population,area)");

	ret = session->open_cursor(session, "table:world", NULL, NULL, &cursor);
	ret = cursor->close(cursor, NULL);

	ret = session->open_cursor(session, "table:world(country,population)",
	    NULL, NULL, &cursor);
	ret = cursor->close(cursor, NULL);

	ret = session->open_cursor(session, "table:", NULL, NULL, &cursor);
	ret = cursor->close(cursor, NULL);

	ret = session->open_cursor(session, "statistics:", NULL, NULL, &cursor);
	ret = cursor->close(cursor, NULL);

	/* Create a simple string table to illustrate basic operations. */
	ret = session->create(session, "table:map",
	    "key_format=S,value_format=S");
	ret = session->open_cursor(session, "table:map", NULL, NULL, &cursor);
	ret = cursor_insert(cursor);
	ret = cursor_first(cursor);
	ret = cursor_forward_scan(cursor);
	ret = cursor_last(cursor);
	ret = cursor_reverse_scan(cursor);
	ret = cursor_update(cursor);
	ret = cursor_remove(cursor);
	ret = cursor->close(cursor, NULL);

	/* Note: closing the connection implicitly closes open session(s). */
	if ((ret = conn->close(conn, NULL)) != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));

	return (ret);
}
