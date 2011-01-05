/*
 * ex_cursor.c Copyright (c) 2010 WiredTiger, Inc.  All rights reserved.
 *
 * This is an example demonstrating some cursor types and operations.
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

	/* Open a connection to the database, creating it if necessary. */
	if ((ret = wiredtiger_open(home, NULL, "create", &conn)) != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));

	/* Open a session for the current thread's work. */
	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		fprintf(stderr, "Error opening a session on %s: %s\n",
		    home, wiredtiger_strerror(ret));

	ret = session->create_table(session, "map",
	    "key_format=r,value_format=5sii,"
	    "columns=(id,country,population,area)");

	ret = session->open_cursor(session, "table:map", NULL, &cursor);
	cursor->close(cursor, NULL);

	ret = session->open_cursor(session, "table:map(country,population)", NULL, &cursor);
	cursor->close(cursor, NULL);

	ret = session->open_cursor(session, "table:", NULL, &cursor);
	cursor->close(cursor, NULL);

	ret = session->open_cursor(session, "statistics:", NULL, &cursor);
	cursor->close(cursor, NULL);

	/* Note: closing the connection implicitly closes open session(s). */
	if ((ret = conn->close(conn, NULL)) != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));

	return (ret);
}
