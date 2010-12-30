/*
 * ex_stat.c Copyright (c) 2010 WiredTiger, Inc.  All rights reserved.
 *
 * This is an example demonstrating how to query a database's statistics.
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
	const char *key;
	uint64_t value;

	if ((ret = wiredtiger_open(home, NULL, "create", &conn)) != 0 ||
	    (ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));
	/* Note: further error checking omitted for clarity. */

	/* Open a cursor on the (virtual) statistics table. */
	ret = session->open_cursor(session, "statistics:", NULL, &cursor);

	while ((ret = cursor->next(cursor)) == 0) {
		cursor->get_key(cursor, &key);
		cursor->get_value(cursor, &value);

		printf("Got statistic: %s = %d\n", key, (int)value);
	}

	ret = conn->close(conn, NULL);

	return (ret);
}


