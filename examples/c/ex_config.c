/*
 * ex_config.c Copyright (c) 2010 WiredTiger
 *
 * This is an example demostrating how to configure various database and table
 * properties.
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

	if ((ret = wt_open(home, "create,cache_size=10000000", &conn)) != 0 ||
	    (ret = conn->open_session(conn, NULL, &session)) != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wt_strerror(ret));
	/* Note: further error checking omitted for clarity. */

	/* Open a cursor on the (virtual) configuration table. */
	ret = session->open_cursor(session, "config:", NULL, &cursor);

	for (ret = cursor->get(cursor, &key, &value, WT_FIRST);
	    ret == 0;
	    ret = cursor->get(cursor, &key, &value, WT_NEXT)) {
		printf("Got configuration value: %s = %s\n",
		    (const char *)key.data, (const char *)value.data);
	}

	ret = conn->close(conn, NULL);

	return (ret);
}

