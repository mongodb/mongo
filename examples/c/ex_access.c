/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 *
 * ex_access.c
 * 	demonstrates how to create and access a simple table.
 */
#include <stdio.h>
#include <string.h>

#include <wiredtiger.h>

const char *home = "WT_TEST";

int main(void)
{
	/*! [access example connection] */
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	const char *key, *value;
	int ret;

	if ((ret = wiredtiger_open(home, NULL, "create", &conn)) != 0 ||
	    (ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));
	/*! [access example connection] */

	/*! [access example table create] */
	ret = session->create(session,
	    "table:access", "key_format=S,value_format=S");
	/*! [access example table create] */

	/*! [access example cursor open] */
	ret = session->open_cursor(session,
	    "table:access", NULL, NULL, &cursor);
	/*! [access example cursor open] */

	/*! [access example cursor insert] */
	cursor->set_key(cursor, "key1");	/* Insert a record. */
	cursor->set_value(cursor, "value1");
	ret = cursor->insert(cursor);
	/*! [access example cursor insert] */

	/*! [access example cursor list] */
	for (ret = cursor->first(cursor);	/* Show all records. */
	    ret == 0;
	    ret = cursor->next(cursor)) {
		ret = cursor->get_key(cursor, &key);
		ret = cursor->get_value(cursor, &value);

		printf("Got record: %s : %s\n", key, value);
	}
	/*! [access example cursor list] */

	/*! [access example close] */
	ret = conn->close(conn, NULL);
	/*! [access example close] */

	return (ret);
}
