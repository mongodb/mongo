/*
 * ex_access.c Copyright (c) 2010 WiredTiger, Inc.  All rights reserved.
 *
 * This is an example demostrating how to create and access a simple table.
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

	if ((ret = wiredtiger_open(home, NULL, "create", &conn)) != 0 ||
	    (ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));
	/* Note: further error checking omitted for clarity. */

	if (conn->is_new(conn))
		ret = session->create_table(session, "access",
		    "keyfmt=S,valuefmt=S");

	ret = session->open_cursor(session, "table:access", NULL, &cursor);

	if (conn->is_new(conn)) {
		/* Insert a record. */
		ret = cursor->set_key(cursor, "key1");
		ret = cursor->set_value(cursor, "value1");
		ret = cursor->insert(cursor);
		ret = cursor->first(cursor);
	}

	/* Show all records. */
	while ((ret = cursor->next(cursor)) == 0) {
		ret = cursor->get_key(cursor, &key);
		ret = cursor->get_value(cursor, &value);

		printf("Got record: %s : %s\n", key, value);
	}

	ret = conn->close(conn, NULL);

	return (ret);
}
