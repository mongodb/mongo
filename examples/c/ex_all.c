/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * ex_all.c
 *	Containing a call to every method in the WiredTiger API.
 *
 *	It doesn't do anything very useful, just demonstrates how to call each
 *	method.  This file is used to populate the API reference with code
 *	fragments.
 */

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <wiredtiger.h>

int add_collator(WT_CONNECTION *conn);
int add_compressor(WT_CONNECTION *conn);
int add_data_source(WT_CONNECTION *conn);
int add_extractor(WT_CONNECTION *conn);
int connection_ops(WT_CONNECTION *conn);
int cursor_ops(WT_SESSION *session);
int cursor_search_near(WT_CURSOR *cursor);
int session_ops(WT_SESSION *session);

int
cursor_ops(WT_SESSION *session)
{
	WT_CURSOR *cursor, *other;
	int ret;

	other = NULL;

	/*! [Open a cursor] */
	ret = session->open_cursor(
	    session, "table:mytable", NULL, NULL, &cursor);
	/*! [Open a cursor] */

	{
	/*! [Get the cursor's string key] */
	const char *key;	/* Get the cursor's string key. */
	ret = cursor->get_key(cursor, &key);
	/*! [Get the cursor's string key] */
	}

	{
	/*! [Get the cursor's record number key] */
	uint64_t recno;		/* Get the cursor's record number key. */
	ret = cursor->get_key(cursor, &recno);
	/*! [Get the cursor's record number key] */
	}

	{
	/*! [Get the cursor's string value] */
	const char *value;	/* Get the cursor's string value. */
	ret = cursor->get_value(cursor, &value);
	/*! [Get the cursor's string value] */
	}

	{
	/*! [Get the cursor's raw value] */
	WT_ITEM value;		/* Get the cursor's raw value. */
	ret = cursor->get_value(cursor, &value);
	/*! [Get the cursor's raw value] */
	}

	{
	/*! [Set the cursor's string key] */
				/* Set the cursor's string key. */
	const char *key = "another key";
	cursor->set_key(cursor, key);
	/*! [Set the cursor's string key] */
	}

	{
	/*! [Set the cursor's record number key] */
	uint64_t recno = 37;	/* Set the cursor's record number key. */
	cursor->set_key(cursor, recno);
	/*! [Set the cursor's record number key] */
	}

	{
	/*! [Set the cursor's string value] */
				/* Set the cursor's string value. */
	const char *value = "another value";
	cursor->set_value(cursor, value);
	/*! [Set the cursor's string value] */
	}
	{
	/*! [Set the cursor's raw value] */
	WT_ITEM value;		/* Set the cursor's raw value. */
	value.data = "another value";
	value.size = strlen("another value");
	cursor->set_value(cursor, &value);
	/*! [Set the cursor's raw value] */
	}

	/*! [Return the next record] */
	ret = cursor->next(cursor);
	/*! [Return the next record] */

	/*! [Return the previous record] */
	ret = cursor->prev(cursor);
	/*! [Return the previous record] */

	/*! [Reset the cursor] */
	ret = cursor->reset(cursor);
	/*! [Reset the cursor] */

	/*! [Test cursor equality] */
	if (cursor->equals(cursor, other)) {
		/* Take some action. */
	}
	/*! [Test cursor equality] */

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
	/* Insert a new record. */
	const char *key = "some key";
	const char *value = "some value";
	cursor->set_key(cursor, key);
	cursor->set_value(cursor, value);
	ret = cursor->insert(cursor);
	/*! [Insert a new record] */
	}

	{
	/*! [Insert a new record or overwrite an existing record] */
	/* Insert a new record or overwrite an existing record. */
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
	/* Insert a new record and assign a record number. */
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

	{
	/*! [Display an error] */
	const char *key = "some key";
	cursor->set_key(cursor, key);
	if ((ret = cursor->remove(cursor)) != 0) {
		fprintf(stderr,
		    "cursor.remove: %s\n", wiredtiger_strerror(ret));
		return (ret);
	}
	/*! [Display an error] */
	}

	/*! [Close the cursor] */
	ret = cursor->close(cursor);
	/*! [Close the cursor] */

	return (ret);
}

int
cursor_search_near(WT_CURSOR *cursor)
{
	int exact, ret;
	const char *key = "some key";

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

	return (ret);
}

int
session_ops(WT_SESSION *session)
{
	int ret;

	cursor_ops(session);

	/*! [Create a table] */
	ret = session->create(session, "table:mytable",
	    "key_format=S,value_format=S");
	/*! [Create a table] */

	/*! [session checkpoint] */
	ret = session->checkpoint(session, NULL);
	/*! [session checkpoint] */

	/*! [session drop] */
	/* Discard a table. */
	ret = session->drop(session, "table:mytable", NULL);

	/* Drop the "midnight" snapshot. */
	ret = session->drop(session, "table:mytable", "snapshot=midnight");

	/* Drop all snapshots from a table. */
	ret = session->drop(session, "table:mytable", "snapshot=(all)");

	/* Drop all snapshots after and including "noon". */
	ret = session->drop(session, "table:mytable", "snapshot=(from=noon)");

	/* Drop all snapshots before and including "midnight". */
	ret = session->drop(session, "table:mytable", "snapshot=(to=midnight)");
	/*! [session drop] */

	/*! [session dumpfile] */
	ret = session->dumpfile(session, "file:myfile", NULL);
	/*! [session dumpfile] */

	/*! [session msg_printf] */
	ret = session->msg_printf(
	    session, "process ID %" PRIuMAX, (uintmax_t)getpid());
	/*! [session msg_printf] */

	/*! [session rename] */
	ret = session->rename(session, "table:old", "table:new", NULL);
	/*! [session rename] */

	/*! [session salvage] */
	ret = session->salvage(session, "table:mytable", NULL);
	/*! [session salvage] */

	/*! [session sync] */
	ret = session->sync(session, "table:mytable", NULL);
	/*! [session sync] */

	/*! [session truncate] */
	ret = session->truncate(session, "table:mytable", NULL, NULL, NULL);
	/*! [session truncate] */

	{
	/*! [session range truncate] */
	WT_CURSOR *start, *stop;

	ret = session->open_cursor(
	    session, "table:mytable", NULL, NULL, &start);
	start->set_key(start, "June01");
	ret = start->search(start);

	ret = session->open_cursor(
	    session, "table:mytable", NULL, NULL, &stop);
	stop->set_key(stop, "June30");
	ret = stop->search(stop);

	ret = session->truncate(session, NULL, start, stop, NULL);
	/*! [session range truncate] */
	}

	/*! [session upgrade] */
	ret = session->upgrade(session, "table:mytable", NULL);
	/*! [session upgrade] */

	/*! [session verify] */
	ret = session->verify(session, "table:mytable", NULL);
	/*! [session verify] */

	/*! [session begin transaction] */
	ret = session->begin_transaction(session, NULL);
	/*! [session begin transaction] */

	/*! [session commit transaction] */
	ret = session->commit_transaction(session, NULL);
	/*! [session commit transaction] */

	/*! [session rollback transaction] */
	ret = session->rollback_transaction(session, NULL);
	/*! [session rollback transaction] */

	/*! [session close] */
	ret = session->close(session, NULL);
	/*! [session close] */

	return (ret);
}

/*! [WT_DATA_SOURCE create] */
static int
my_create(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *name, const char *config)
{
	/* Unused parameters */
	(void)dsrc;
	(void)session;
	(void)name;
	(void)config;

	return (0);
}
/*! [WT_DATA_SOURCE create] */

/*! [WT_DATA_SOURCE drop] */
static int
my_drop(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *name, const char *config)
{
	/* Unused parameters */
	(void)dsrc;
	(void)session;
	(void)name;
	(void)config;

	return (0);
}
/*! [WT_DATA_SOURCE drop] */

/*! [WT_DATA_SOURCE open_cursor] */
static int
my_open_cursor(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *obj, WT_CURSOR *old_cursor, const char *config,
    WT_CURSOR **new_cursor)
{
	/* Unused parameters */
	(void)dsrc;

	(void)session;
	(void)obj;
	(void)old_cursor;
	(void)config;
	(void)new_cursor;

	return (0);
}
/*! [WT_DATA_SOURCE open_cursor] */

/*! [WT_DATA_SOURCE rename] */
static int
my_rename(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *oldname, const char *newname, const char *config)
{
	/* Unused parameters */
	(void)dsrc;
	(void)session;
	(void)oldname;
	(void)newname;
	(void)config;

	return (0);
}
/*! [WT_DATA_SOURCE rename] */

/*! [WT_DATA_SOURCE sync] */
static int
my_sync(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *name, const char *config)
{
	/* Unused parameters */
	(void)dsrc;
	(void)session;
	(void)name;
	(void)config;

	return (0);
}
/*! [WT_DATA_SOURCE sync] */

/*! [WT_DATA_SOURCE truncate] */
static int
my_truncate(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *name, const char *config)
{
	/* Unused parameters */
	(void)dsrc;
	(void)session;
	(void)name;
	(void)config;

	return (0);
}
/*! [WT_DATA_SOURCE truncate] */

int
add_data_source(WT_CONNECTION *conn)
{
	int ret;

	/*! [WT_DATA_SOURCE register] */
	static WT_DATA_SOURCE my_dsrc = {
		my_create,
		my_drop,
		my_open_cursor,
		my_rename,
		my_sync,
		my_truncate
	};
	ret = conn->add_data_source(conn, "dsrc:", &my_dsrc, NULL);
	/*! [WT_DATA_SOURCE register] */

	return (ret);
}

/*! [Implement WT_COLLATOR] */
/*
 * A simple example of the collator API: compare the keys as strings.
 */
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

int
add_collator(WT_CONNECTION *conn)
{
	int ret;

	/*! [WT_COLLATOR register] */
	static WT_COLLATOR my_collator = { my_compare };
	ret = conn->add_collator(conn, "my_collator", &my_collator, NULL);
	/*! [WT_COLLATOR register] */

	return (ret);
}

/*! [WT_COMPRESSOR compress] */
/*
 * A simple compression example that passes data through unchanged.
 */
static int
my_compress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp, int *compression_failed)
{
	/* Unused parameters */
	(void)compressor;
	(void)session;

	*compression_failed = 0;
	if (dst_len < src_len) {
		*compression_failed = 1;
		return (0);
	}
	memcpy(dst, src, src_len);
	*result_lenp = src_len;
	return (0);
}
/*! [WT_COMPRESSOR compress] */

/*! [WT_COMPRESSOR decompress] */
/*
 * A simple decompression example that passes data through unchanged.
 */
static int
my_decompress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp)
{
	/* Unused parameters */
	(void)compressor;
	(void)session;

	if (dst_len < src_len)
		return (ENOMEM);

	memcpy(dst, src, src_len);
	*result_lenp = src_len;
	return (0);
}
/*! [WT_COMPRESSOR decompress] */

/*! [WT_COMPRESSOR presize] */
/*
 * A simple pre-size example that returns the source length.
 */
static int
my_pre_size(WT_COMPRESSOR *compressor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    size_t *result_lenp)
{
	/* Unused parameters */
	(void)compressor;
	(void)session;
	(void)src;

	*result_lenp = src_len;
	return (0);
}
/*! [WT_COMPRESSOR presize] */

int
add_compressor(WT_CONNECTION *conn)
{
	int ret;
	
	/*! [WT_COMPRESSOR register] */
	static WT_COMPRESSOR my_compressor = {
	    my_compress, my_decompress, my_pre_size };
	ret = conn->add_compressor(conn, "my_compress", &my_compressor, NULL);
	/*! [WT_COMPRESSOR register] */

	return (ret);
}

/*! [WT_EXTRACTOR] */
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
/*! [WT_EXTRACTOR] */

int
add_extractor(WT_CONNECTION *conn)
{
	int ret;

	/*! [WT_EXTRACTOR register] */
	static WT_EXTRACTOR my_extractor;
	my_extractor.extract = my_extract;
	ret = conn->add_extractor(conn, "my_extractor", &my_extractor, NULL);
	/*! [WT_EXTRACTOR register] */

	return (ret);
}

int
connection_ops(WT_CONNECTION *conn)
{
	int ret;

	/*! [conn load extension] */
	ret = conn->load_extension(conn, "my_extension.dll", NULL);
	/*! [conn load extension] */

	add_collator(conn);
	add_data_source(conn);
	add_extractor(conn);

	/*! [conn close] */
	ret = conn->close(conn, NULL);
	/*! [conn close] */

	/*! [conn get_home] */
	printf("The database home is %s\n", conn->get_home(conn));
	/*! [conn get_home] */

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

	return (ret);
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
	ret = wiredtiger_struct_pack(buf, sizeof(buf), "iSh", 42, "hello", -3);
	/*! [Pack fields into a buffer] */
 
	{
	/*! [Unpack fields from a buffer] */
	int i;
	char *s;
	short h;
	ret = wiredtiger_struct_unpack(buf, sizeof(buf), "iSh", &i, &s, &h);
	/*! [Unpack fields from a buffer] */
	}
	}

	/*! [Get the WiredTiger library version #1] */
	printf("WiredTiger version %s\n", wiredtiger_version(NULL, NULL, NULL));
	/*! [Get the WiredTiger library version #1] */

	{
	/*! [Get the WiredTiger library version #2] */
	int major, minor, patch;
	(void)wiredtiger_version(&major, &minor, &patch);
	printf("WiredTiger version is %d, %d (patch %d)\n",
	    major, minor, patch);
	/*! [Get the WiredTiger library version #2] */
	}

	return (ret);
}
