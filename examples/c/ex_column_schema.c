/*
 * ex_column_schema.c Copyright (c) 2010 WiredTiger
 *
 * This is an example demostrating how to create and access column-oriented data.
 * This file can be used as a loadable module.
 */

#include <stdio.h>
#include <string.h>

#include "ex_column.h"

/* Extractors for the fields. */
static int __pop_get_country(WT_SESSION *session, WT_SCHEMA *schema,
    const WT_ITEM *key, const WT_ITEM *value, WT_ITEM *column_key, int *more)
{
	POP_RECORD *record = (POP_RECORD *)value->data;
	column_key->data = record->country;
	column_key->size = sizeof (record->country);
	return 0;
}

static int __pop_get_year(WT_SESSION *session, WT_SCHEMA *schema,
    const WT_ITEM *key, const WT_ITEM *value, WT_ITEM *column_key, int *more)
{
	POP_RECORD *record = (POP_RECORD *)value->data;
	column_key->data = &record->year;
	column_key->size = sizeof (record->year);
	return 0;
}

static int __pop_get_population(WT_SESSION *session, WT_SCHEMA *schema,
    const WT_ITEM *key, const WT_ITEM *value, WT_ITEM *column_key, int *more)
{
	POP_RECORD *record = (POP_RECORD *)value->data;
	column_key->data = &record->population;
	column_key->size = sizeof (record->population);
	return 0;
}

/*
 * Country will compare lexicographically, but we care that the years are in
 * numerical order.
 */
static int __pop_cmp_year(WT_SESSION *session, WT_SCHEMA *schema,
    const WT_ITEM *key1, const WT_ITEM *key2)
{
	int16_t year1, year2;	
	memcpy(&year1, key1->data, sizeof (int16_t));
	memcpy(&year2, key2->data, sizeof (int16_t));
	return (year1 < year2) ? -1 : (year1 == year2) ? 0 : 1;
}

static WT_COLUMN_INFO pop_columns[] = {
	{ "country", NULL, __pop_get_country },
	{ "year", __pop_cmp_year, __pop_get_year },
	{ "population", NULL, __pop_get_population }
};

static WT_SCHEMA pop_schema = { NULL, NULL, 3, pop_columns };

#if LOADABLE_MODULE
int wt_extension_init(WT_CONNECTION *conn)
{
	int ret;

	/* Note: error checking omitted for clarity. */

	/*
	 * Tell WT about the schema.  If the schema was moved into an
	 * extension, this would not be required.  Similarly for creating the
	 * table: once created, cursors do not need to know about the schema.
	 */
	ret = conn->add_schema(conn, "POP_RECORD", &pop_schema, NULL);

	return (ret);
}
#else
int add_pop_schema(WT_CONNECTION *conn)
{
	return conn->add_schema(conn, "POP_RECORD", &pop_schema, NULL);
}
#endif
