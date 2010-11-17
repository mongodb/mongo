/*
 * ex_column_schema.c Copyright (c) 2010 WiredTiger
 *
 * This is an example demostrating how to create and access column-oriented data.
 * This file can be used as a loadable module.
 */

#include <stdio.h>
#include <string.h>

#include "ex_column.h"

static WIREDTIGER_COLUMN_INFO pop_columns[] = {
	{ "country", NULL, NULL },
	{ "year", NULL, NULL },
	{ "population", NULL, NULL }
};

static WIREDTIGER_SCHEMA pop_schema = {
	"r",		/* Format string for keys (recno). */
	"5sHQ",		/*
			 * Format string for data items:
			 * (5-byte string, short, quad.
			 */
	sizeof(pop_columns) / sizeof(pop_columns[0]), /* Number of columns. */
	pop_columns,	/* Column descriptions. */
	0,		/* Session cookie size. */
	NULL,		/* Key comparator. */
	NULL,		/* Duplicate comparator. */
};

#if LOADABLE_MODULE
int wiredtiger_extension_init(WIREDTIGER_CONNECTION *conn, const char *config)
#else
int add_pop_schema(WIREDTIGER_CONNECTION *conn)
#endif
{
	/*
	 * Tell WT about the schema.  If the schema was moved into an
	 * extension, this would not be required.  Similarly for creating the
	 * table: once created, cursors do not need to know about the schema.
	 */
	return conn->add_schema(conn, "POP_RECORD", &pop_schema, NULL);
}
