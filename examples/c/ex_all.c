/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 *
 * ex_hello.c
 *	Containing a call to every method in the WiredTiger API.
 *
 *	It doesn't do anything very useful, just demonstrates how to call each
 *	method.  This file is used to populate the API reference with code
 *	fragments.
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <wiredtiger.h>

void add_collator(WT_CONNECTION *conn);
void add_compressor(WT_CONNECTION *conn);
void add_cursor_type(WT_CONNECTION *conn);
void add_extractor(WT_CONNECTION *conn);
void connection_ops(WT_CONNECTION *conn);
void cursor_ops(WT_SESSION *session);
void cursor_search_near(WT_CURSOR *cursor);
void session_ops(WT_SESSION *session);

void
cursor_ops(WT_SESSION *session)
{
	WT_CURSOR *cursor;
	int ret;

	/*! [Open a cursor] */
	ret = session->open_cursor(
	    session, "table:mytable", NULL, NULL, &cursor);
	/*! [Open a cursor] */

	{
	/*! [Get the cursor's string key] */
	const char *key;
	ret = cursor->get_key(cursor, &key);
	/*! [Get the cursor's string key] */
	}

	{
	/*! [Get the cursor's record number key] */
	uint64_t recno;
	ret = cursor->get_key(cursor, &recno);
	/*! [Get the cursor's record number key] */
	}

	{
	/*! [Get the cursor's string value] */
	const char *value;
	ret = cursor->get_value(cursor, &value);
	/*! [Get the cursor's string value] */
	}

	{
	/*! [Set the cursor's string key] */
	const char *key = "another key";
	cursor->set_key(cursor, key);
	/*! [Set the cursor's string key] */
	}

	{
	/*! [Set the cursor's record number key] */
	uint64_t recno = 37;
	cursor->set_key(cursor, recno);
	/*! [Set the cursor's record number key] */
	}

	{
	/*! [Set the cursor's string value] */
	const char *value = "another value";
	cursor->set_value(cursor, value);
	/*! [Set the cursor's string value] */
	}

	/*! [Return the first key/value pair] */
	ret = cursor->first(cursor);
	/*! [Return the first key/value pair] */

	/*! [Return the last key/value pair] */
	ret = cursor->last(cursor);
	/*! [Return the last key/value pair] */

	/*! [Return the next key/value pair] */
	ret = cursor->next(cursor);
	/*! [Return the next key/value pair] */

	/*! [Return the previous key/value pair] */
	ret = cursor->prev(cursor);
	/*! [Return the previous key/value pair] */

	{
	/*! [Search for an exact match] */
	const char *key = "some key";
	cursor->set_key(cursor, key);
	ret = cursor->search(cursor);
	/*! [Search for an exact match] */
	}

	cursor_search_near(cursor);

	{
	/*! [Insert a new record] */
	const char *key = "some key";
	const char *value = "some value";
	cursor->set_key(cursor, key);
	cursor->set_value(cursor, value);
	ret = cursor->insert(cursor);
	/*! [Insert a new record] */
	}

	{
	/*! [Insert a new record or overwrite an existing record] */
	const char *key = "some key";
	const char *value = "some value";
	ret = session->open_cursor(
	    session, "table:mytable", NULL, "overwrite", &cursor);
	cursor->set_key(cursor, key);
	cursor->set_value(cursor, value);
	ret = cursor->insert(cursor);
	/*! [Insert a new record or overwrite an existing record] */
	}

	{
	/*! [Insert a new record and assign a record number] */
	uint64_t recno;
	const char *value = "some value";
	ret = session->open_cursor(
	    session, "table:mytable", NULL, "append", &cursor);
	cursor->set_value(cursor, value);
	ret = cursor->insert(cursor);
	if (ret == 0)
		recno = cursor->get_key(cursor, &recno);
	/*! [Insert a new record and assign a record number] */
	}

	{
	/*! [Update an existing record] */
	const char *key = "some key";
	const char *value = "some value";
	cursor->set_key(cursor, key);
	cursor->set_value(cursor, value);
	ret = cursor->update(cursor);
	/*! [Update an existing record] */
	}

	{
	/*! [Remove a record] */
	const char *key = "some key";
	cursor->set_key(cursor, key);
	ret = cursor->remove(cursor);
	/*! [Remove a record] */
	}

	/*! [Close the cursor] */
	ret = cursor->close(cursor, NULL);
	/*! [Close the cursor] */
}

void
cursor_search_near(WT_CURSOR *cursor)
{
	int exact, ret;
	const char *key;

	/*! [Search for an exact or adjacent match] */
	cursor->set_key(cursor, key);
	ret = cursor->search_near(cursor, &exact);
	if (ret == 0) {
		if (exact == 0) {
			/* an exact match */
		} else if (exact < 0) {
			/* returned smaller key */
		} else if (exact > 0) {
			/* returned larger key */
		}
	}
	/*! [Search for an exact or adjacent match] */

	/*! [Forward scan greater than or equal] */
	/*
	 * An example of a forward scan through the table, where all keys
	 * greater than or equal to a specified prefix are included in the
	 * scan.
	 */
	cursor->set_key(cursor, key);
	ret = cursor->search_near(cursor, &exact);
	if (ret == 0 && exact >= 0) {
		/* include first key returned in the scan */
	}

	while ((ret = cursor->next(cursor)) == 0) {
		/* the rest of the scan */
	}
	/*! [Forward scan greater than or equal] */

	/*! [Backward scan less than] */
	/*
	 * An example of a backward scan through the table, where all keys
	 * less than a specified prefix are included in the scan.
	 */
	cursor->set_key(cursor, key);
	ret = cursor->search_near(cursor, &exact);
	if (ret == 0 && exact < 0) {
		/* include first key returned in the scan */
	}

	while ((ret = cursor->prev(cursor)) == 0) {
		/* the rest of the scan */
	}
	/*! [Backward scan less than] */
}

void
session_ops(WT_SESSION *session)
{
	int ret;

	cursor_ops(session);

	/*! [Create a table] */
	ret = session->create(session, "table:mytable",
	    "key_format=S,value_format=S");
	/*! [Create a table] */

	ret = session->rename(session, "table:old", "table:new", NULL);

	ret = session->drop(session, "table:mytable", NULL);

	ret = session->sync(session, "table:mytable", NULL);

	ret = session->truncate(session, "table:mytable", NULL, NULL, NULL);

	ret = session->verify(session, "table:mytable", NULL);

	ret = session->begin_transaction(session, NULL);

	ret = session->commit_transaction(session, NULL);

	ret = session->rollback_transaction(session, NULL);

	ret = session->checkpoint(session, NULL);

	ret = session->close(session, NULL);
}

/*! [Implement WT_CURSOR_TYPE] */
static int
my_cursor_size(WT_CURSOR_TYPE *ctype, const char *obj, size_t *sizep)
{
	(void)ctype;
	(void)obj;

	*sizep = sizeof (WT_CURSOR);
	return (0);
}

static int
my_init_cursor(WT_CURSOR_TYPE *ctype, WT_SESSION *session,
    const char *obj, WT_CURSOR *old_cursor, const char *config,
    WT_CURSOR *new_cursor)
{
	/* Unused parameters */
	(void)ctype;
	(void)session;
	(void)obj;
	(void)old_cursor;
	(void)config;
	(void)new_cursor;

	return (0);
}
/*! [Implement WT_CURSOR_TYPE] */

void
add_cursor_type(WT_CONNECTION *conn)
{
	int ret;

	/*! [Register a new cursor type] */
	static WT_CURSOR_TYPE my_ctype = { my_cursor_size, my_init_cursor };
	ret = conn->add_cursor_type(conn, NULL, &my_ctype, NULL);
	/*! [Register a new cursor type] */
}

/*! [Implement WT_COLLATOR] */
static int
my_compare(WT_COLLATOR *collator, WT_SESSION *session,
    const WT_ITEM *value1, const WT_ITEM *value2, int *cmp)
{
	const char *p1, *p2;

	/* Unused parameters */
	(void)collator;
	(void)session;

	p1 = (const char *)value1->data;
	p2 = (const char *)value2->data;
	while (*p1 != '\0' && *p1 == *p2)
		p1++, p2++;

	*cmp = (int)*p2 - (int)*p1;
	return (0);
}
/*! [Implement WT_COLLATOR] */

void
add_collator(WT_CONNECTION *conn)
{
	int ret;

	/*! [Register a new collator] */
	static WT_COLLATOR my_collator = { my_compare };
	ret = conn->add_collator(conn, "my_collator", &my_collator, NULL);
	/*! [Register a new collator] */
}

/*! [Implement WT_COMPRESSOR] */
static int
my_compress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    const WT_ITEM *source, WT_ITEM *dest, int *compression_failed)
{
	/* Unused parameters */
	(void)compressor;
	(void)session;

	*compression_failed = 0;
	if (dest->size < source->size) {
		*compression_failed = 1;
		return (0);
	}
	memcpy((void *)dest->data, source->data, source->size);
	dest->size = source->size;
	return (0);
}

static int
my_decompress(WT_COMPRESSOR *compressor,
    WT_SESSION *session, const WT_ITEM *source, WT_ITEM *dest)
{
	/* Unused parameters */
	(void)compressor;
	(void)session;

	if (dest->size < source->size)
		return (ENOMEM);

	memcpy((void *)dest->data, source->data, source->size);
	dest->size = source->size;
	return (0);
}
/*! [Implement WT_COMPRESSOR] */

void
add_compressor(WT_CONNECTION *conn)
{
	int ret;
	
	/*! [Register a new compressor] */
	static WT_COMPRESSOR my_compressor = { my_compress, my_decompress };
	ret = conn->add_compressor(conn, "my_compress", &my_compressor, NULL);
	/*! [Register a new compressor] */
}

/*! [Implement WT_EXTRACTOR] */
static int
my_extract(WT_EXTRACTOR *extractor, WT_SESSION *session,
    const WT_ITEM *key, const WT_ITEM *value,
    WT_ITEM *result)
{
	/* Unused parameters */
	(void)extractor;
	(void)session;
	(void)key;

	*result = *value;
	return (0);
}
/*! [Implement WT_EXTRACTOR] */

void
add_extractor(WT_CONNECTION *conn)
{
	int ret;

	/*! [Register a new extractor] */
	static WT_EXTRACTOR my_extractor;
	my_extractor.extract = my_extract;
	ret = conn->add_extractor(conn, "my_extractor", &my_extractor, NULL);
	/*! [Register a new extractor] */
}

void
connection_ops(WT_CONNECTION *conn)
{
	int ret;

	ret = conn->load_extension(conn, "my_extension.dll", NULL);

	add_cursor_type(conn);
	add_collator(conn);
	add_extractor(conn);

	ret = conn->close(conn, NULL);

	printf("The home is %s\n", conn->get_home(conn));

	/*! [is_new] */
	if (conn->is_new(conn)) {
		/* First time initialization. */
	}
	/*! [is_new] */

	{
	/*! [Open a session] */
	WT_SESSION *session;
	ret = conn->open_session(conn, NULL, NULL, &session);
	/*! [Open a session] */

	session_ops(session);
	}
}

int main(void)
{
	int ret;

	{
	/*! [Open a connection] */
	WT_CONNECTION *conn;
	const char *home = "WT_TEST";
	ret = wiredtiger_open(home, NULL, "create,transactional", &conn);
	/*! [Open a connection] */
	}

	{
	/*! [Get the packed size] */
	size_t size;
	size = wiredtiger_struct_size("iSh", 42, "hello", -3);
	assert(size < 100);
	/*! [Get the packed size] */
	}

	{
	/*! [Pack fields into a buffer] */
	char buf[100];
	ret = wiredtiger_struct_pack(buf, sizeof (buf), "iSh", 42, "hello", -3);
	/*! [Pack fields into a buffer] */
 
	{
	/*! [Unpack fields from a buffer] */
	int i;
	char *s;
	short h;
	ret = wiredtiger_struct_unpack(buf, sizeof (buf), "iSh", &i, &s, &h);
	/*! [Unpack fields from a buffer] */
	}
	}

	{
	/*! [Get the WiredTiger library version] */
	int major, minor, patch;
	printf("WiredTiger version %s\n",
	    wiredtiger_version(&major, &minor, &patch));
	/*! [Get the WiredTiger library version] */
	}

	return (0);
}
