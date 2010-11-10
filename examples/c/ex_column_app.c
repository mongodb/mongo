/*
 * ex_column.c Copyright (c) 2010 WiredTiger
 *
 * This is an example application demonstrating how to create and access
 * column-oriented data.
 */

#include <stdio.h>
#include <string.h>

#include "ex_column.h"

const char *home = "WT_TEST";

POP_RECORD pop_data[] = {
	{ "USA", 1980, 226542250 },
	{ "USA", 2009, 307006550 },
	{ "UK", 2008, 61414062 },
	{ "CAN", 2008, 33311400 },
	{ "AU", 2008, 21431800 }
};

int main()
{
	int ret;
	WT_CONNECTION *conn;
	WT_SESSION *session;
	WT_CURSOR *cursor;
	WT_ITEM key, value;
	POP_RECORD *p, *endp;

	/*
	 * The config string below is only needed if it is possible that the
	 * database has not already been created.  The database could be
	 * created and configured with a command line tool instead.  Subsequent
	 * opens would not require any configuration: the previous settings
	 * would persist.
	 */
#if LOADABLE_MODULE
	ret = wt_open(home, "create,extension=ex_column_ext.so", &conn);
#else
	ret = wt_open(home, "create", &conn);
	if (ret == 0) {
		extern int add_pop_schema(WT_CONNECTION *);
		ret = add_pop_schema(conn);
	}
#endif
	if (ret != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wt_strerror(ret));
	/* Note: error checking omitted for clarity. */

	ret = conn->open_session(conn, NULL, &session);
	ret = session->open_cursor(session, "ctable:population",
	    "create,schema=POP_RECORD", &cursor);

	endp = pop_data + (sizeof pop_data / sizeof pop_data[0]);
	for (p = pop_data; p < endp; p++) {
		value.data = p;
		ret = cursor->insert(cursor, NULL, &value);
	}
	ret = cursor->close(cursor, NULL);

	/* Now just read through the countries we know about */
	ret = session->open_cursor(session, "column:population.country",
	    NULL, &cursor);

	for (ret = cursor->get(cursor, &key, &value, WT_FIRST);
	    ret == 0;
	    ret = cursor->get(cursor, &key, &value, WT_NEXT)) {
		printf("Got record for country %s : row ID %d\n",
		    (const char *)key.data, (int)*(uint64_t *)value.data);
	}

	ret = conn->close(conn, NULL);

	return (ret);
}

