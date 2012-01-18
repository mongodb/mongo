/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 *
 * ex_config.c
 *	This is an example demonstrating how to configure various database and
 *	table properties.
 */

#include <stdio.h>
#include <string.h>

#include <wiredtiger.h>

const char *home = "WT_TEST";

int main(void)
{
	int ret;
	WT_CONNECTION *conn;
	WT_SESSION *session;
	WT_CURSOR *cursor;
	const char *key, *value;

	/*! [configure cache size] */
	if ((ret = wiredtiger_open(home, NULL,
	    "create,cache_size=10M", &conn)) != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));
	/*! [configure cache size] */

	/*! [create a table] */
	ret = conn->open_session(conn, NULL, NULL, &session);

	ret = session->create(session,
	    "table:access", "key_format=S,value_format=S");
	/*! [create a table] */

	/*! [transaction] */
	ret = session->begin_transaction(session, "priority=100,name=mytxn");

	ret = session->open_cursor(session, "config:", NULL, NULL, &cursor);

	while ((ret = cursor->next(cursor)) == 0) {
		cursor->get_key(cursor, &key);
		cursor->get_value(cursor, &value);
		printf("configuration value: %s = %s\n", key, value);
	}

	ret = session->commit_transaction(session, NULL);
	/*! [transaction] */

	ret = conn->close(conn, NULL);

	return (ret);
}

