/*-
 * Public Domain 2008-2013 WiredTiger, Inc.
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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <wiredtiger.h>

int add_collator(WT_CONNECTION *conn);
int add_compressor(WT_CONNECTION *conn);
int add_data_source(WT_CONNECTION *conn);
int add_extractor(WT_CONNECTION *conn);
int checkpoint_ops(WT_SESSION *session);
int connection_ops(WT_CONNECTION *conn);
int cursor_ops(WT_SESSION *session);
int cursor_search_near(WT_CURSOR *cursor);
int hot_backup(WT_SESSION *session);
int pack_ops(WT_SESSION *session);
int session_ops(WT_SESSION *session);
int transaction_ops(WT_CONNECTION *conn, WT_SESSION *session);

const char *progname;
const char *home = NULL;

int
cursor_ops(WT_SESSION *session)
{
	WT_CURSOR *cursor;
	int ret;

	/*! [Open a cursor] */
	ret = session->open_cursor(
	    session, "table:mytable", NULL, NULL, &cursor);
	/*! [Open a cursor] */

	{
	WT_CURSOR *duplicate;
	const char *key = "some key";
	/*! [Duplicate a cursor] */
	ret = session->open_cursor(
	    session, "table:mytable", NULL, NULL, &cursor);
	cursor->set_key(cursor, key);
	ret = cursor->search(cursor);

	/* Duplicate the cursor. */
	ret = session->open_cursor(session, NULL, cursor, NULL, &duplicate);
	/*! [Duplicate a cursor] */
	}

	{
	WT_CURSOR *overwrite_cursor;
	const char *key = "some key", *value = "some value";
	/*! [Reconfigure a cursor] */
	ret = session->open_cursor(
	    session, "table:mytable", NULL, NULL, &cursor);
	cursor->set_key(cursor, key);

	/* Reconfigure the cursor to overwrite the record. */
	ret = session->open_cursor(
	    session, NULL, cursor, "overwrite", &overwrite_cursor);
	ret = cursor->close(cursor);

	overwrite_cursor->set_value(overwrite_cursor, value);
	ret = overwrite_cursor->insert(cursor);
	/*! [Reconfigure a cursor] */
	}

	{
	/*! [boolean configuration string example] */
	ret = session->open_cursor(session, "table:mytable", NULL,
	    "overwrite", &cursor);
	ret = session->open_cursor(session, "table:mytable", NULL,
	    "overwrite=true", &cursor);
	ret = session->open_cursor(session, "table:mytable", NULL,
	    "overwrite=1", &cursor);
	/*! [boolean configuration string example] */
	}

	{
	/*! [open a named checkpoint] */
	ret = session->open_cursor(session,
	    "table:mytable", NULL, "checkpoint=midnight", &cursor);
	/*! [open a named checkpoint] */
	}

	{
	/*! [open the default checkpoint] */
	ret = session->open_cursor(session,
	    "table:mytable", NULL, "checkpoint=WiredTigerCheckpoint", &cursor);
	/*! [open the default checkpoint] */
	}

	{
	/*! [Get the cursor's string key] */
	const char *key;	/* Get the cursor's string key. */
	ret = cursor->get_key(cursor, &key);
	/*! [Get the cursor's string key] */
	}

	{
	/*! [Set the cursor's string key] */
				/* Set the cursor's string key. */
	const char *key = "another key";
	cursor->set_key(cursor, key);
	/*! [Set the cursor's string key] */
	}

	{
	/*! [Get the cursor's record number key] */
	uint64_t recno;		/* Get the cursor's record number key. */
	ret = cursor->get_key(cursor, &recno);
	/*! [Get the cursor's record number key] */
	}

	{
	/*! [Set the cursor's record number key] */
	uint64_t recno = 37;	/* Set the cursor's record number key. */
	cursor->set_key(cursor, recno);
	/*! [Set the cursor's record number key] */
	}

	{
	/*! [Get the cursor's composite key] */
			/* Get the cursor's "SiH" format composite key. */
	const char *first;
	int32_t second;
	uint16_t third;
	cursor->get_key(cursor, &first, &second, &third);
	/*! [Get the cursor's composite key] */
	}

	{
	/*! [Set the cursor's composite key] */
			/* Set the cursor's "SiH" format composite key. */
	cursor->set_key(cursor, "first", (int32_t)5, (uint16_t)7);
	/*! [Set the cursor's composite key] */
	}

	{
	/*! [Get the cursor's string value] */
	const char *value;	/* Get the cursor's string value. */
	ret = cursor->get_value(cursor, &value);
	/*! [Get the cursor's string value] */
	}

	{
	/*! [Set the cursor's string value] */
				/* Set the cursor's string value. */
	const char *value = "another value";
	cursor->set_value(cursor, value);
	/*! [Set the cursor's string value] */
	}

	{
	/*! [Get the cursor's raw value] */
	WT_ITEM value;		/* Get the cursor's raw value. */
	ret = cursor->get_value(cursor, &value);
	/*! [Get the cursor's raw value] */
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

	{
	WT_CURSOR *other = NULL;
	/*! [Cursor comparison] */
	int compare;
	ret = cursor->compare(cursor, other, &compare);
	if (compare == 0) {
		/* Cursors reference the same key */
	} else if (compare < 0) {
		/* Cursor key less than other key */
	} else if (compare > 0) {
		/* Cursor key greater than other key */
	}
	/*! [Cursor comparison] */
	}

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
	const char *key = "some key", *value = "some value";
	cursor->set_key(cursor, key);
	cursor->set_value(cursor, value);
	ret = cursor->insert(cursor);
	/*! [Insert a new record] */
	}

	{
	const char *key = "some key", *value = "some value";
	/*! [Insert a new record or overwrite an existing record] */
	/* Insert a new record or overwrite an existing record. */
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
		ret = cursor->get_key(cursor, &recno);
	/*! [Insert a new record and assign a record number] */
	}

	{
	/*! [Update an existing record] */
	const char *key = "some key", *value = "some value";
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
	const char *key = "non-existent key";
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
checkpoint_ops(WT_SESSION *session)
{
	int ret;

	/*! [Checkpoint examples] */
	/* Checkpoint the database. */
	ret = session->checkpoint(session, NULL);

	/* Checkpoint of the database, creating a named snapshot. */
	ret = session->checkpoint(session, "name=June01");

	/*
	 * Checkpoint a list of objects.
	 * JSON parsing requires quoting the list of target URIs.
	 */
	ret = session->
	    checkpoint(session, "target=(\"table:table1\",\"table:table2\")");

	/*
	 * Checkpoint a list of objects, creating a named snapshot.
	 * JSON parsing requires quoting the list of target URIs.
	 */
	ret = session->
	    checkpoint(session, "target=(\"table:mytable\"),name=midnight");

	/* Checkpoint the database, discarding all previous snapshots. */
	ret = session->checkpoint(session, "drop=(from=all)");

	/* Checkpoint the database, discarding the "midnight" snapshot. */
	ret = session->checkpoint(session, "drop=(midnight)");

	/*
	 * Checkpoint the database, discarding all snapshots after and
	 * including "noon".
	 */
	ret = session->checkpoint(session, "drop=(from=noon)");

	/*
	 * Checkpoint the database, discarding all snapshots before and
	 * including "midnight".
	 */
	ret = session->checkpoint(session, "drop=(to=midnight)");

	/*
	 * Create a checkpoint of a table, creating the "July01" snapshot and
	 * discarding the "May01" and "June01" snapshots.
	 * JSON parsing requires quoting the list of target URIs.
	 */
	ret = session->checkpoint(session,
	    "target=(\"table:mytable\"),name=July01,drop=(May01,June01)");
	/*! [Checkpoint examples] */

	/*! [JSON quoting example] */
	/*
	 * Checkpoint a list of objects.
	 * JSON parsing requires quoting the list of target URIs.
	 */
	ret = session->
	    checkpoint(session, "target=(\"table:table1\",\"table:table2\")");
	/*! [JSON quoting example] */

	return (ret);
}

int
session_ops(WT_SESSION *session)
{
	int ret;

	/*! [Reconfigure a session] */
	ret = session->reconfigure(session, "isolation=snapshot");
	/*! [Reconfigure a session] */

	/*! [Create a table] */
	ret = session->create(session,
	    "table:mytable", "key_format=S,value_format=S");
	/*! [Create a table] */
	ret = session->drop(session, "table:mytable", NULL);

	/*! [Create a column-store table] */
	ret = session->create(session,
	    "table:mytable", "key_format=r,value_format=S");
	/*! [Create a column-store table] */
	ret = session->drop(session, "table:mytable", NULL);

	/*! [Create a table with columns] */
	/*
	 * Create a table with columns: keys are record numbers, values are
	 * (string, signed 32-bit integer, unsigned 16-bit integer).
	 */
	ret = session->create(session, "table:mytable",
	    "key_format=r,value_format=SiH,"
	    "columns=(id,department,salary,year-started)");
	/*! [Create a table with columns] */
	ret = session->drop(session, "table:mytable", NULL);

	/*
	 * This example code gets run, and the compression libraries might not
	 * be loaded, causing the create to fail.  The documentation requires
	 * the code snippets, use #ifdef's to avoid running it.
	 */
#ifdef MIGHT_NOT_RUN
	/*! [Create a bzip2 compressed table] */
	ret = session->create(session,
	    "table:mytable",
	    "block_compressor=bzip2,key_format=S,value_format=S");
	/*! [Create a bzip2 compressed table] */
	ret = session->drop(session, "table:mytable", NULL);

	/*! [Create a snappy compressed table] */
	ret = session->create(session,
	    "table:mytable",
	    "block_compressor=snappy,key_format=S,value_format=S");
	/*! [Create a snappy compressed table] */
	ret = session->drop(session, "table:mytable", NULL);
#endif

	/*! [Configure checksums to uncompressed] */
	ret = session->create(session, "table:mytable",
	    "key_format=S,value_format=S,checksum=uncompressed");
	/*! [Configure checksums to uncompressed] */
	ret = session->drop(session, "table:mytable", NULL);

	/*! [Configure dictionary compression on] */
	ret = session->create(session, "table:mytable",
	    "key_format=S,value_format=S,dictionary=1000");
	/*! [Configure dictionary compression on] */
	ret = session->drop(session, "table:mytable", NULL);

	/*! [Configure key prefix compression off] */
	ret = session->create(session, "table:mytable",
	    "key_format=S,value_format=S,prefix_compression=false");
	/*! [Configure key prefix compression off] */
	ret = session->drop(session, "table:mytable", NULL);

#ifdef MIGHT_NOT_RUN
						/* Requires sync_file_range */
	/*! [os_cache_dirty_max configuration] */
	ret = session->create(
	    session, "table:mytable", "os_cache_dirty_max=500MB");
	/*! [os_cache_dirty_max configuration] */
	ret = session->drop(session, "table:mytable", NULL);

						/* Requires posix_fadvise */
	/*! [os_cache_max configuration] */
	ret = session->create(session, "table:mytable", "os_cache_max=1GB");
	/*! [os_cache_max configuration] */
	ret = session->drop(session, "table:mytable", NULL);
#endif

	/*! [Create a cache-resident object] */
	ret = session->create(session,
	    "table:mytable", "key_format=r,value_format=S,cache_resident=true");
	/*! [Create a cache-resident object] */
	ret = session->drop(session, "table:mytable", NULL);

	{
	/* Create a table for the session operations. */
	ret = session->create(
	    session, "table:mytable", "key_format=S,value_format=S");

	/*! [Compact a table] */
	ret = session->compact(session, "table:mytable", NULL);
	/*! [Compact a table] */

	/*! [Print to the message stream] */
	ret = session->msg_printf(
	    session, "process ID %" PRIuMAX, (uintmax_t)getpid());
	/*! [Print to the message stream] */

	/*! [Rename a table] */
	ret = session->rename(session, "table:old", "table:new", NULL);
	/*! [Rename a table] */

	/*! [Salvage a table] */
	ret = session->salvage(session, "table:mytable", NULL);
	/*! [Salvage a table] */

	/*! [Truncate a table] */
	ret = session->truncate(session, "table:mytable", NULL, NULL, NULL);
	/*! [Truncate a table] */

	{
	/*! [Truncate a range] */
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
	/*! [Truncate a range] */
	}

	/*! [Upgrade a table] */
	ret = session->upgrade(session, "table:mytable", NULL);
	/*! [Upgrade a table] */

	/*! [Verify a table] */
	ret = session->verify(session, "table:mytable", NULL);
	/*! [Verify a table] */

	/*! [Drop a table] */
	ret = session->drop(session, "table:mytable", NULL);
	/*! [Drop a table] */
	}

	/*! [Close a session] */
	ret = session->close(session, NULL);
	/*! [Close a session] */

	return (ret);
}

int
transaction_ops(WT_CONNECTION *conn, WT_SESSION *session)
{
	WT_CURSOR *cursor;
	int ret;

	/*! [transaction commit/rollback] */
	ret =
	    session->open_cursor(session, "table:mytable", NULL, NULL, &cursor);
	ret = session->begin_transaction(session, NULL);
	/*
	 * Cursors may be opened before or after the transaction begins, and in
	 * either case, subsequent operations are included in the transaction.
	 * The begin_transaction call resets all open cursors.
	 */

	cursor->set_key(cursor, "key");
	cursor->set_value(cursor, "value");
	switch (ret = cursor->update(cursor)) {
	case 0:					/* Update success */
		ret = session->commit_transaction(session, NULL);
		/*
		 * The commit_transaction call resets all open cursors.
		 * If commit_transaction fails, the transaction was rolled-back.
		 */
		break;
	case WT_DEADLOCK:			/* Update conflict */
	default:				/* Other error */
		ret = session->rollback_transaction(session, NULL);
		/* The rollback_transaction call resets all open cursors. */
		break;
	}

	/* Cursors remain open and may be used for multiple transactions. */
	/*! [transaction commit/rollback] */
	ret = cursor->close(cursor);

	/*! [transaction isolation] */
	/* A single transaction configured for snapshot isolation. */
	ret =
	    session->open_cursor(session, "table:mytable", NULL, NULL, &cursor);
	ret = session->begin_transaction(session, "isolation=snapshot");
	cursor->set_key(cursor, "some-key");
	cursor->set_value(cursor, "some-value");
	ret = cursor->update(cursor);
	ret = session->commit_transaction(session, NULL);
	/*! [transaction isolation] */

	/*! [session isolation configuration] */
	/* Open a session configured for read-uncommitted isolation. */
	ret = conn->open_session(
	    conn, NULL, "isolation=read_uncommitted", &session);
	/*! [session isolation configuration] */

	/*! [session isolation re-configuration] */
	/* Re-configure a session for snapshot isolation. */
	ret = session->reconfigure(session, "isolation=snapshot");
	/*! [session isolation re-configuration] */

	return (ret);
}

/*! [WT_DATA_SOURCE create] */
static int
my_create(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *uri, int exclusive, const char *cfg[])
{
	/* Unused parameters */
	(void)dsrc;
	(void)session;
	(void)uri;
	(void)exclusive;
	(void)cfg;

	return (0);
}
/*! [WT_DATA_SOURCE create] */

/*! [WT_DATA_SOURCE drop] */
static int
my_drop(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *uri, const char *cfg[])
{
	/* Unused parameters */
	(void)dsrc;
	(void)session;
	(void)uri;
	(void)cfg;

	return (0);
}
/*! [WT_DATA_SOURCE drop] */

/*! [WT_DATA_SOURCE open_cursor] */
static int
my_open_cursor(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *uri, const char *cfg[], WT_CURSOR **new_cursor)
{
	/* Unused parameters */
	(void)dsrc;

	(void)session;
	(void)uri;
	(void)cfg;
	(void)new_cursor;

	return (0);
}
/*! [WT_DATA_SOURCE open_cursor] */

/*! [WT_DATA_SOURCE rename] */
static int
my_rename(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *uri, const char *newname, const char *cfg[])
{
	/* Unused parameters */
	(void)dsrc;
	(void)session;
	(void)uri;
	(void)newname;
	(void)cfg;

	return (0);
}
/*! [WT_DATA_SOURCE rename] */

/*! [WT_DATA_SOURCE truncate] */
static int
my_truncate(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *uri, const char *cfg[])
{
	/* Unused parameters */
	(void)dsrc;
	(void)session;
	(void)uri;
	(void)cfg;

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
		NULL,			/* No compaction support */
		my_drop,
		my_open_cursor,
		my_rename,
		NULL,			/* No salvage support */
		my_truncate,
		NULL			/* No verify support */
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

static int
my_compress_raw(WT_COMPRESSOR *compressor, WT_SESSION *session,
    size_t page_max, u_int split_pct, size_t extra,
    uint8_t *src, uint32_t *offsets, uint32_t slots,
    uint8_t *dst, size_t dst_len, int final,
    size_t *result_lenp, uint32_t *result_slotsp)
{
	/* Unused parameters */
	(void)compressor;
	(void)session;
	(void)page_max;
	(void)split_pct;
	(void)extra;
	(void)src;
	(void)offsets;
	(void)slots;
	(void)dst;
	(void)dst_len;
	(void)final;
	(void)result_lenp;
	(void)result_slotsp;

	return (0);
}

int
add_compressor(WT_CONNECTION *conn)
{
	int ret;

	/*! [WT_COMPRESSOR register] */
	static WT_COMPRESSOR my_compressor = {
	    my_compress,
	    my_compress_raw,		/* NULL, if no raw compression */
	    my_decompress,
	    my_pre_size			/* NULL, if pre-sizing not needed */
	};
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

#ifdef MIGHT_NOT_RUN
	/*! [Load an extension] */
	ret = conn->load_extension(conn, "my_extension.dll", NULL);
	/*! [Load an extension] */
#endif

	add_collator(conn);
	add_data_source(conn);
	add_extractor(conn);

	/*! [Reconfigure a connection] */
	ret = conn->reconfigure(conn, "eviction_target=75");
	/*! [Reconfigure a connection] */

	/*! [Get the database home directory] */
	printf("The database home is %s\n", conn->get_home(conn));
	/*! [Get the database home directory] */

	/*! [Check if the database is newly created] */
	if (conn->is_new(conn)) {
		/* First time initialization. */
	}
	/*! [Check if the database is newly created] */

	{
	/*! [Open a session] */
	WT_SESSION *session;
	ret = conn->open_session(conn, NULL, NULL, &session);
	/*! [Open a session] */

	session_ops(session);
	}

	/*! [Close a connection] */
	ret = conn->close(conn, NULL);
	/*! [Close a connection] */

	return (ret);
}

int
pack_ops(WT_SESSION *session)
{
	int ret;

	{
	/*! [Get the packed size] */
	size_t size;
	ret = wiredtiger_struct_size(session, &size, "iSh", 42, "hello", -3);
	/*! [Get the packed size] */
	assert(size < 100);
	}

	{
	/*! [Pack fields into a buffer] */
	char buf[100];
	ret = wiredtiger_struct_pack(
	    session, buf, sizeof(buf), "iSh", 42, "hello", -3);
	/*! [Pack fields into a buffer] */

	{
	/*! [Unpack fields from a buffer] */
	int i;
	char *s;
	short h;
	ret = wiredtiger_struct_unpack(
	    session, buf, sizeof(buf), "iSh", &i, &s, &h);
	/*! [Unpack fields from a buffer] */
	}
	}

	return (ret);
}

int
hot_backup(WT_SESSION *session)
{
	char buf[1024];

	/*! [Hot backup]*/
	WT_CURSOR *cursor;
	const char *filename;
	int ret;

	/* Create the backup directory. */
	ret = mkdir("/path/database.backup", 077);

	/* Open the hot backup data source. */
	ret = session->open_cursor(session, "backup:", NULL, NULL, &cursor);

	/* Copy the list of files. */
	while (
	    (ret = cursor->next(cursor)) == 0 &&
	    (ret = cursor->get_key(cursor, &filename)) == 0) {
		(void)snprintf(buf, sizeof(buf),
		    "cp /path/database/%s /path/database.backup/%s",
		    filename, filename);
		ret = system(buf);
	}
	if (ret == WT_NOTFOUND)
		ret = 0;
	if (ret != 0)
		fprintf(stderr, "%s: cursor next(backup:) failed: %s\n",
		    progname, wiredtiger_strerror(ret));

	ret = cursor->close(cursor);
	/*! [Hot backup]*/

	/*! [Hot backup of a checkpoint]*/
	ret = session->checkpoint(session, "drop=(from=June01),name=June01");
	/*! [Hot backup of a checkpoint]*/

	return (0);
}

int
main(void)
{
	WT_CONNECTION *conn;
	int ret;

	/*! [Open a connection] */
	ret = wiredtiger_open(home, NULL, "create,cache_size=500M", &conn);
	/*! [Open a connection] */

	if (ret == 0)
		connection_ops(conn);
	/*
	 * The connection has been closed.
	 */

#ifdef MIGHT_NOT_RUN
	/*
	 * This example code gets run, and the compression libraries might not
	 * be installed, causing the open to fail.  The documentation requires
	 * the code snippets, use #ifdef's to avoid running it.
	 */
	/*! [Configure bzip2 extension] */
	ret = wiredtiger_open(home, NULL,
	    "create,"
	    "extensions=[\"/usr/local/lib/wiredtiger_bzip2.so\"]", &conn);
	/*! [Configure bzip2 extension] */
	if (ret == 0)
		(void)conn->close(conn, NULL);

	/*! [Configure snappy extension] */
	ret = wiredtiger_open(home, NULL,
	    "create,"
	    "extensions=[\"/usr/local/lib/wiredtiger_snappy.so\"]", &conn);
	/*! [Configure snappy extension] */
	if (ret == 0)
		(void)conn->close(conn, NULL);

	/*
	 * This example code gets run, and direct I/O might not be available,
	 * causing the open to fail.  The documentation requires code snippets,
	 * use #ifdef's to avoid running it.
	 */
	/* Might Not Run: direct I/O may not be available. */
	/*! [Configure direct_io for data files] */
	ret = wiredtiger_open(home, NULL, "create,direct_io=[data]", &conn);
	/*! [Configure direct_io for data files] */
	if (ret == 0)
		(void)conn->close(conn, NULL);
#endif

	/*! [Statistics configuration] */
	ret = wiredtiger_open(home, NULL, "create,statistics=true", &conn);
	/*! [Statistics configuration] */
	if (ret == 0)
		(void)conn->close(conn, NULL);

	/*! [Statistics logging] */
	ret = wiredtiger_open(
	    home, NULL, "create,statistics_log=(wait=30)", &conn);
	/*! [Statistics logging] */
	if (ret == 0)
		(void)conn->close(conn, NULL);

	/*! [Statistics logging with a table] */
	ret = wiredtiger_open(home, NULL,
	    "create,"
	    "statistics_log=(sources=(\"table:table1\",\"table:table2\"))",
	    &conn);
	/*! [Statistics logging with a table] */
	if (ret == 0)
		(void)conn->close(conn, NULL);

	/*! [Statistics logging with all tables] */
	ret = wiredtiger_open(home, NULL,
	    "create,statistics_log=(sources=(\"table:\"))",
	    &conn);
	/*! [Statistics logging with all tables] */
	if (ret == 0)
		(void)conn->close(conn, NULL);

#ifdef MIGHT_NOT_RUN
	/*
	 * This example code gets run, and a non-existent log file path might
	 * cause the open to fail.  The documentation requires code snippets,
	 * use #ifdef's to avoid running it.
	 */
	/*! [Statistics logging with path] */
	ret = wiredtiger_open(home, NULL,
	    "create,"
	    "statistics_log=(wait=120,path=\"/log/log.%m.%d.%y\")", &conn);
	/*! [Statistics logging with path] */
	if (ret == 0)
		(void)conn->close(conn, NULL);
#endif

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
