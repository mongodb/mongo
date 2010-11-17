/*
 * ex_access.c Copyright (c) 2010 WiredTiger
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
	WIREDTIGER_CONNECTION *conn;
	WIREDTIGER_SESSION *session;
	WIREDTIGER_CURSOR *cursor;
	const char *key, *value;

	if ((ret = wiredtiger_open(home, "create", &conn)) != 0 ||
	    (ret = conn->open_session(conn, NULL, &session)) != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));
	/* Note: further error checking omitted for clarity. */

	if (conn->is_new)
		session->create_table(session, "access",
		    "keystruct=s,valuestruct=s");

	ret = session->open_cursor(session, "table:access", NULL, &cursor);

	cursor->set_key(cursor, "1");
	cursor->set_value(cursor, "one");
	ret = cursor->insert(cursor);

	while ((ret = cursor->next(cursor)) == 0) {
		cursor->get_key(cursor, &key);
		cursor->get_value(cursor, &value);

		printf("Got record: %s : %s\n", key, value);
	}

	ret = conn->close(conn, NULL);

	return (ret);
}
