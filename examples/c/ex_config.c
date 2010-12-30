/*
 * ex_config.c Copyright (c) 2010 WiredTiger, Inc.  All rights reserved.
 *
 * This is an example demonstrating how to configure various database and table
 * properties.
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
	    "create,cache_size=10000000", &conn)) != 0 ||
	    (ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));
	/* Note: further error checking omitted for clarity. */

	if (conn->is_new(conn))
		session->create_table(session, "access", "keyfmt=S,valuefmt=S");

	/* Open a cursor on the (virtual) configuration table. */
	ret = session->open_cursor(session, "config:", NULL, &cursor);

	while ((ret = cursor->next(cursor)) == 0) {
		cursor->get_key(cursor, &key);
		cursor->get_value(cursor, &value);

		printf("Got configuration value: %s = %s\n", key, value);
	}

	ret = conn->close(conn, NULL);

	return (ret);
}

