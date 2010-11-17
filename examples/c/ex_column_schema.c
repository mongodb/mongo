/*
 * ex_column_schema.c Copyright (c) 2010 WiredTiger
 *
 * This is an example demostrating how to create and access column-oriented data.
 * This file can be used as a loadable module.
 */

#include <stdio.h>
#include <string.h>

#include <wiredtiger.h>

static WIREDTIGER_COLUMN_INFO pop_columns[] = {
	{ "country", 0, NULL, NULL },
	{ "year", 0, NULL, NULL },
	{ "population", 1, NULL, NULL }
};

static const char *country_year_cols[] = { "country", "year" };
static WIREDTIGER_INDEX_INFO pop_indices[] = {
	{ "country_year",  country_year_cols,
	    sizeof(country_year_cols) / sizeof(country_year_cols[0]) }
};

static WIREDTIGER_SCHEMA pop_schema = {
	"r",		/* Format string for keys (recno). */
	"5sHQ",		/*
			 * Format string for data items:
			 * (5-byte string, short, quad).
			 * See ::wiredtiger_struct_pack
			 */
	pop_columns,	/* Column descriptions. */
	sizeof(pop_columns) / sizeof(pop_columns[0]), /* Number of columns. */
	pop_indices,	/* Index descriptions. */
	sizeof(pop_indices) / sizeof(pop_indices[0]), /* Number of columns. */
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
