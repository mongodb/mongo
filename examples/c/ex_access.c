/*
 * ex_access.c Copyright (c) 2010 WiredTiger
 *
 * This is an example demostrating how to create and access a simple table.
 */

#include <stdio.h>
#include <string.h>

#include <wt/wtds.h>

const char *home = "WT_TEST";

int main()
{
	int ret;
	WT_CONNECTION *conn;
	WT_SESSION *session;
	WT_CURSOR *cursor;
	WT_ITEM key, value;

	if ((ret = wt_open(home, "create", &conn)) != 0 ||
	    (ret = conn->open_session(conn, NULL, &session)) != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wt_strerror(ret));
	/* Note: further error checking omitted for clarity. */

	/* The config string is only needed if the table might not exist. */
	ret = session->open_cursor(session, "table:access",
	    "create,keytype=string,valuetype=string", &cursor);

	key.data = (void *)"1";
	value.data = (void *)"one";
	ret = cursor->insert(cursor, &key, &value);

	for (ret = cursor->get(cursor, &key, &value, WT_FIRST);
	    ret == 0;
	    ret = cursor->get(cursor, &key, &value, WT_NEXT)) {
		printf("Got record: %s : %s\n",
		    (const char *)key.data, (const char *)value.data);
	}

	ret = conn->close(conn, NULL);

	return (ret);
}
