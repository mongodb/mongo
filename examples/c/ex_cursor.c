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

const char *home = "WT_TEST";

static void
initialize_key(WT_ITEM *key)
{
	key->data = "foo";
	key->size = 3;
}

static void
initialize_value(WT_ITEM *val)
{
	val->data = "bar";
	val->size = 3;
}

static int
first(WT_CURSOR *cursor)
{
	WT_ITEM key, value;
	int ret;

	while ((ret = cursor->first(cursor)) == 0) {
		if ((ret = cursor->get_key(cursor, &key)) != 0)
			return (ret);
		if ((ret = cursor->get_value(cursor, &value)) != 0)
			return (ret);
	}
	return (ret);
}

static int
last(WT_CURSOR *cursor)
{
	WT_ITEM key, value;
	int ret;

	while ((ret = cursor->first(cursor)) == 0) {
		if ((ret = cursor->get_key(cursor, &key)) != 0)
			return (ret);
		if ((ret = cursor->get_value(cursor, &value)) != 0)
			return (ret);
	}
	return (ret);
}

static int
forward_traversal(WT_CURSOR *cursor)
{
	WT_ITEM key, value;
	int ret;

	while ((ret = cursor->next(cursor)) == 0) {
		if ((ret = cursor->get_key(cursor, &key)) != 0)
			return (ret);
		if ((ret = cursor->get_value(cursor, &value)) != 0)
			return (ret);
	}
	return (ret);
}

static int
backward_traversal(WT_CURSOR *cursor)
{
	WT_ITEM key, value;
	int ret;

	while ((ret = cursor->prev(cursor)) == 0) {
		if ((ret = cursor->get_key(cursor, &key)) != 0)
			return (ret);
		if ((ret = cursor->get_value(cursor, &value)) != 0)
			return (ret);
	}
	return (ret);
}

static int
search(WT_CURSOR *cursor)
{
	WT_ITEM key, value;
	int ret;

	initialize_key(&key);
	cursor->set_key(cursor, &key);

	if ((ret = cursor->search(cursor)) != 0)
		return (ret);

	if ((ret = cursor->get_value(cursor, &value)) != 0)
		return (ret);

	return (0);
}

static int
search_near(WT_CURSOR *cursor)
{
	WT_ITEM key, value;
	int exact, ret;

	initialize_key(&key);
	cursor->set_key(cursor, &key);

	if ((ret = cursor->search_near(cursor, &exact)) != 0)
		return (ret);

	switch (exact) {
	case -1:		/* Returned key smaller than search key */
		if ((ret = cursor->get_key(cursor, &key)) != 0)
			return (ret);
		break;
	case 0:			/* Exact match found */
		break;
	case 1:			/* Returned key larger than search key */
		if ((ret = cursor->get_key(cursor, &key)) != 0)
			return (ret);
		break;
	}

	if ((ret = cursor->get_value(cursor, &value)) != 0)
		return (ret);

	return (0);
}

static int
cursor_insert(WT_CURSOR *cursor)
{
	WT_ITEM key, value;
	int ret;

	initialize_key(&key);
	cursor->set_key(cursor, &key);
	initialize_value(&value);
	cursor->set_value(cursor, &value);

	return (cursor->update(cursor));
}

static int
cursor_update(WT_CURSOR *cursor)
{
	WT_ITEM key, value;
	int ret;

	initialize_key(&key);
	cursor->set_key(cursor, &key);
	initialize_value(&value);
	cursor->set_value(cursor, &value);

	return (cursor->update(cursor));
}

static int
cursor_remove(WT_CURSOR *cursor)
{
	WT_ITEM key, value;
	int ret;

	initialize_key(&key);
	cursor->set_key(cursor, &key);

	return (cursor->remove(cursor));
}

int main(void)
{
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_ITEM key, value;
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

	ret = session->create(session, "table:map",
	    "key_format=r,value_format=5sii,"
	    "columns=(id,country,population,area)");

	ret = session->open_cursor(session, "table:map", NULL, NULL, &cursor);
	cursor->close(cursor, NULL);

	ret = session->open_cursor(
	    session, "table:map(country,population)", NULL, NULL, &cursor);
	cursor->close(cursor, NULL);

	ret = session->open_cursor(session, "table:", NULL, NULL, &cursor);
	cursor->close(cursor, NULL);

	ret = session->open_cursor(session, "statistics:", NULL, NULL, &cursor);
	cursor->close(cursor, NULL);

	/* Note: closing the connection implicitly closes open session(s). */
	if ((ret = conn->close(conn, NULL)) != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));

	return (ret);
}
