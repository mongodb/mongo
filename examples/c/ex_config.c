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

int main()
{
	int ret;
	WT_CONNECTION *conn;
	WT_SESSION *session;
	WT_CURSOR *cursor;
	const char *key, *value;

	if ((ret = wiredtiger_open(home, NULL,
	    "create,cache_size=10M", &conn)) != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));
	/* Note: further error checking omitted for clarity. */

	ret = conn->open_session(conn, NULL, NULL, &session);

	ret = session->create_table(session, "access",
	    "key_format=S,value_format=S");

	ret = session->begin_transaction(session, "priority=100,name=mytxn");

	ret = session->open_cursor(session, "config:", NULL, NULL, &cursor);

	while ((ret = cursor->next(cursor)) == 0) {
		cursor->get_key(cursor, &key);
		cursor->get_value(cursor, &value);

		printf("Got configuration value: %s = %s\n", key, value);
	}

	ret = session->commit_transaction(session, NULL);

	ret = conn->close(conn, NULL);

	return (ret);
}

