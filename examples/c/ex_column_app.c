/*
 * ex_column.c Copyright (c) 2010 WiredTiger
 *
 * This is an example application demonstrating how to create and access
 * column-oriented data.
 */

#include <stdio.h>
#include <string.h>

#include "ex_column.h"

const char *home = "WIREDTIGER_TEST";

POP_RECORD pop_data[] = {
	{ "USA", 1980, 226542250 },
	{ "USA", 2009, 307006550 },
	{ "UK", 2008, 61414062 },
	{ "CAN", 2008, 33311400 },
	{ "AU", 2008, 21431800 }
};

int main()
{
	int is_new, ret;
	WIREDTIGER_CONNECTION *conn;
	WIREDTIGER_SESSION *session;
	WIREDTIGER_CURSOR *cursor;
	POP_RECORD *p, *endp;
	const char *country;
	wiredtiger_recno_t recno;

	ret = wiredtiger_open(home, "create", &conn);
	if (ret != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));
	/* Note: error checking omitted for clarity. */

	if (conn->is_new) {
#if LOADABLE_MODULE
		ret = conn->add_extension(conn, NULL, "ex_column_app.so", NULL);
#else
		extern int add_pop_schema(WIREDTIGER_CONNECTION *);
		ret = add_pop_schema(conn);
#endif
		ret = session->create_table(session,
		    "population", "schema=POP_RECORD");
	}

	ret = conn->open_session(conn, NULL, &session);
	ret = session->open_cursor(session, "table:population", NULL, &cursor);

	endp = pop_data + (sizeof(pop_data) / sizeof(pop_data[0]));
	for (p = pop_data; p < endp; p++) {
		cursor->set_value(cursor, p->country, p->year, p->population);
		ret = cursor->insert(cursor);
	}
	ret = cursor->close(cursor, NULL);

	/* Now just read through the countries we know about */
	ret = session->open_cursor(session, "column:population.country",
	    NULL, &cursor);

	while ((ret = cursor->next(cursor)) == 0) {
		cursor->get_key(cursor, &country);
		cursor->get_value(cursor, &recno);

		printf("Got country %s : row ID %d\n", country, (int)recno);
	}

	ret = conn->close(conn, NULL);

	return (ret);
}

