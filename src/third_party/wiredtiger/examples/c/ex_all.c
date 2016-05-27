/*-
 * Public Domain 2014-2016 MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
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

#include <sys/stat.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#else
#include "windows_shim.h"
#endif

#include <wiredtiger.h>

int add_collator(WT_CONNECTION *conn);
int add_extractor(WT_CONNECTION *conn);
int backup(WT_SESSION *session);
int checkpoint_ops(WT_SESSION *session);
int connection_ops(WT_CONNECTION *conn);
int cursor_ops(WT_SESSION *session);
int cursor_search_near(WT_CURSOR *cursor);
int cursor_statistics(WT_SESSION *session);
int pack_ops(WT_SESSION *session);
int named_snapshot_ops(WT_SESSION *session);
int session_ops(WT_SESSION *session);
int transaction_ops(WT_CONNECTION *conn, WT_SESSION *session);

static const char * const progname = "ex_all";
static const char *home;

int
cursor_ops(WT_SESSION *session)
{
	WT_CURSOR *cursor;
	int ret;

	/*! [Open a cursor] */
	ret = session->open_cursor(
	    session, "table:mytable", NULL, NULL, &cursor);
	/*! [Open a cursor] */

	/*! [Open a cursor on the metadata] */
	ret = session->open_cursor(
	    session, "metadata:", NULL, NULL, &cursor);
	/*! [Open a cursor on the metadata] */

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
	const char *key = "some key", *value = "some value";
	/*! [Reconfigure a cursor] */
	ret = session->open_cursor(
	    session, "table:mytable", NULL, "overwrite=false", &cursor);
	cursor->set_key(cursor, key);
	cursor->set_value(cursor, value);

	/* Reconfigure the cursor to overwrite the record. */
	ret = cursor->reconfigure(cursor, "overwrite=true");
	ret = cursor->insert(cursor);
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
	ret = cursor->get_key(cursor, &first, &second, &third);
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
	WT_CURSOR *other = NULL;
	/*! [Cursor equality] */
	int equal;
	ret = cursor->equals(cursor, other, &equal);
	if (equal) {
		/* Cursors reference the same key */
	} else {
		/* Cursors don't reference the same key */
	}
	/*! [Cursor equality] */
	}

	{
	/*! [Search for an exact match] */
	const char *key = "some key";
	cursor->set_key(cursor, key);
	ret = cursor->search(cursor);
	/*! [Search for an exact match] */
	}

	ret = cursor_search_near(cursor);

	{
	/*! [Insert a new record or overwrite an existing record] */
	/* Insert a new record or overwrite an existing record. */
	const char *key = "some key", *value = "some value";
	ret = session->open_cursor(
	    session, "table:mytable", NULL, NULL, &cursor);
	cursor->set_key(cursor, key);
	cursor->set_value(cursor, value);
	ret = cursor->insert(cursor);
	/*! [Insert a new record or overwrite an existing record] */
	}

	{
	/*! [Insert a new record and fail if the record exists] */
	/* Insert a new record and fail if the record exists. */
	const char *key = "some key", *value = "some value";
	ret = session->open_cursor(
	    session, "table:mytable", NULL, "overwrite=false", &cursor);
	cursor->set_key(cursor, key);
	cursor->set_value(cursor, value);
	ret = cursor->insert(cursor);
	/*! [Insert a new record and fail if the record exists] */
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
	/*! [Update an existing record or insert a new record] */
	const char *key = "some key", *value = "some value";
	ret = session->open_cursor(
	    session, "table:mytable", NULL, NULL, &cursor);
	cursor->set_key(cursor, key);
	cursor->set_value(cursor, value);
	ret = cursor->update(cursor);
	/*! [Update an existing record or insert a new record] */
	}

	{
	/*! [Update an existing record and fail if DNE] */
	const char *key = "some key", *value = "some value";
	ret = session->open_cursor(
	    session, "table:mytable", NULL, "overwrite=false", &cursor);
	cursor->set_key(cursor, key);
	cursor->set_value(cursor, value);
	ret = cursor->update(cursor);
	/*! [Update an existing record and fail if DNE] */
	}

	{
	/*! [Remove a record] */
	const char *key = "some key";
	ret = session->open_cursor(
	    session, "table:mytable", NULL, NULL, &cursor);
	cursor->set_key(cursor, key);
	ret = cursor->remove(cursor);
	/*! [Remove a record] */
	}

	{
	/*! [Remove a record and fail if DNE] */
	const char *key = "some key";
	ret = session->open_cursor(
	    session, "table:mytable", NULL, "overwrite=false", &cursor);
	cursor->set_key(cursor, key);
	ret = cursor->remove(cursor);
	/*! [Remove a record and fail if DNE] */
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

	{
	/*! [Display an error thread safe] */
	const char *key = "non-existent key";
	cursor->set_key(cursor, key);
	if ((ret = cursor->remove(cursor)) != 0) {
		fprintf(stderr,
		    "cursor.remove: %s\n",
		    cursor->session->strerror(cursor->session, ret));
		return (ret);
	}
	/*! [Display an error thread safe] */
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
cursor_statistics(WT_SESSION *session)
{
	WT_CURSOR *cursor;
	int ret;

	/*! [Statistics cursor database] */
	ret = session->open_cursor(
	    session, "statistics:", NULL, NULL, &cursor);
	/*! [Statistics cursor database] */

	/*! [Statistics cursor table] */
	ret = session->open_cursor(
	    session, "statistics:table:mytable", NULL, NULL, &cursor);
	/*! [Statistics cursor table] */

	/*! [Statistics cursor table fast] */
	ret = session->open_cursor(session,
	    "statistics:table:mytable", NULL, "statistics=(fast)", &cursor);
	/*! [Statistics cursor table fast] */

	/*! [Statistics clear configuration] */
	ret = session->open_cursor(session,
	    "statistics:", NULL, "statistics=(fast,clear)", &cursor);
	/*! [Statistics clear configuration] */

	/*! [Statistics cursor clear configuration] */
	ret = session->open_cursor(session,
	    "statistics:table:mytable",
	    NULL, "statistics=(all,clear)", &cursor);
	/*! [Statistics cursor clear configuration] */

	return (ret);
}

int
named_snapshot_ops(WT_SESSION *session)
{
	int ret;

	/*! [Snapshot examples] */

	/* Create a named snapshot */
	ret = session->snapshot(session, "name=June01");

	/* Open a transaction at a given snapshot */
	ret = session->begin_transaction(session, "snapshot=June01");

	/* Drop all named snapshots */
	ret = session->snapshot(session, "drop=(all)");
	/*! [Snapshot examples] */

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

	/*! [Create a table and configure the page size] */
	ret = session->create(session,
	    "table:mytable", "key_format=S,value_format=S,"
	    "internal_page_max=16KB,leaf_page_max=1MB,leaf_value_max=64KB");
	/*! [Create a table and configure the page size] */
	ret = session->drop(session, "table:mytable", NULL);

	/*! [Create a table and configure a large leaf value max] */
	ret = session->create(session,
	    "table:mytable", "key_format=S,value_format=S,"
	    "leaf_page_max=16KB,leaf_value_max=256KB");
	/*! [Create a table and configure a large leaf value max] */
	ret = session->drop(session, "table:mytable", NULL);

	/*
	 * This example code gets run, and the compression libraries might not
	 * be loaded, causing the create to fail.  The documentation requires
	 * the code snippets, use #ifdef's to avoid running it.
	 */
#ifdef MIGHT_NOT_RUN
	/*! [Create a lz4 compressed table] */
	ret = session->create(session,
	    "table:mytable",
	    "block_compressor=lz4,key_format=S,value_format=S");
	/*! [Create a lz4 compressed table] */
	ret = session->drop(session, "table:mytable", NULL);

	/*! [Create a snappy compressed table] */
	ret = session->create(session,
	    "table:mytable",
	    "block_compressor=snappy,key_format=S,value_format=S");
	/*! [Create a snappy compressed table] */
	ret = session->drop(session, "table:mytable", NULL);

	/*! [Create a zlib compressed table] */
	ret = session->create(session,
	    "table:mytable",
	    "block_compressor=zlib,key_format=S,value_format=S");
	/*! [Create a zlib compressed table] */
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

	/*! [Configure key prefix compression on] */
	ret = session->create(session, "table:mytable",
	    "key_format=S,value_format=S,prefix_compression=true");
	/*! [Configure key prefix compression on] */
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
	/*! [Configure block_allocation] */
	ret = session->create(session, "table:mytable",
	    "key_format=S,value_format=S,block_allocation=first");
	/*! [Configure block_allocation] */
	ret = session->drop(session, "table:mytable", NULL);

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

	/*! [Rebalance a table] */
	ret = session->rebalance(session, "table:mytable", NULL);
	/*! [Rebalance a table] */

	/*! [Rename a table] */
	ret = session->rename(session, "table:old", "table:new", NULL);
	/*! [Rename a table] */

	/*! [Salvage a table] */
	ret = session->salvage(session, "table:mytable", NULL);
	/*! [Salvage a table] */

	/*! [Truncate a table] */
	ret = session->truncate(session, "table:mytable", NULL, NULL, NULL);
	/*! [Truncate a table] */

	/*! [Transaction sync] */
	ret = session->transaction_sync(session, NULL);
	/*! [Transaction sync] */

	/*! [Reset the session] */
	ret = session->reset(session);
	/*! [Reset the session] */

	{
	/*
	 * Insert a pair of keys so we can truncate a range.
	 */
	WT_CURSOR *cursor;
	ret = session->open_cursor(
	    session, "table:mytable", NULL, NULL, &cursor);
	cursor->set_key(cursor, "June01");
	cursor->set_value(cursor, "value");
	ret = cursor->update(cursor);
	cursor->set_key(cursor, "June30");
	cursor->set_value(cursor, "value");
	ret = cursor->update(cursor);
	ret = cursor->close(cursor);

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
	/*
	 * Cursors may be opened before or after the transaction begins, and in
	 * either case, subsequent operations are included in the transaction.
	 * Opening cursors before the transaction begins allows applications to
	 * cache cursors and use them for multiple operations.
	 */
	ret =
	    session->open_cursor(session, "table:mytable", NULL, NULL, &cursor);
	ret = session->begin_transaction(session, NULL);

	cursor->set_key(cursor, "key");
	cursor->set_value(cursor, "value");
	switch (ret = cursor->update(cursor)) {
	case 0:					/* Update success */
		ret = session->commit_transaction(session, NULL);
		/*
		 * If commit_transaction succeeds, cursors remain positioned; if
		 * commit_transaction fails, the transaction was rolled-back and
		 * and all cursors are reset.
		 */
		break;
	case WT_ROLLBACK:			/* Update conflict */
	default:				/* Other error */
		ret = session->rollback_transaction(session, NULL);
		/* The rollback_transaction call resets all cursors. */
		break;
	}

	/*
	 * Cursors remain open and may be used for multiple transactions.
	 */
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

	{
	/*! [transaction pinned range] */
	/* Check the transaction ID range pinned by the session handle. */
	uint64_t range;

	ret = session->transaction_pinned_range(session, &range);
	/*! [transaction pinned range] */
	}

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
	static WT_COLLATOR my_collator = { my_compare, NULL, NULL };
	ret = conn->add_collator(conn, "my_collator", &my_collator, NULL);
	/*! [WT_COLLATOR register] */

	return (ret);
}

/*! [WT_EXTRACTOR] */
static int
my_extract(WT_EXTRACTOR *extractor, WT_SESSION *session,
    const WT_ITEM *key, const WT_ITEM *value,
    WT_CURSOR *result_cursor)
{
	/* Unused parameters */
	(void)extractor;
	(void)session;
	(void)key;

	result_cursor->set_key(result_cursor, value);
	return (result_cursor->insert(result_cursor));
}
/*! [WT_EXTRACTOR] */

int
add_extractor(WT_CONNECTION *conn)
{
	int ret;

	/*! [WT_EXTRACTOR register] */
	static WT_EXTRACTOR my_extractor = {my_extract, NULL, NULL};

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

	ret = conn->load_extension(conn,
	    "datasource/libdatasource.so",
	    "config=[device=/dev/sd1,alignment=64]");
	/*! [Load an extension] */
#endif

	ret = add_collator(conn);
	ret = add_extractor(conn);

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

	/*! [Validate a configuration string] */
	/*
	 * Validate a configuration string for a WiredTiger function or method.
	 *
	 * Functions are specified by name (for example, "wiredtiger_open").
	 *
	 * Methods are specified using a concatenation of the handle name, a
	 * period and the method name (for example, session create would be
	 * "WT_SESSION.create" and cursor close would be WT_CURSOR.close").
	 */
	ret = wiredtiger_config_validate(
	    NULL, NULL, "WT_SESSION.create", "allocation_size=32KB");
	/*! [Validate a configuration string] */

	{
	/*! [Open a session] */
	WT_SESSION *session;
	ret = conn->open_session(conn, NULL, NULL, &session);
	/*! [Open a session] */

	ret = session_ops(session);
	}

	/*! [Configure method configuration] */
	/*
	 * Applications opening a cursor for the data-source object "my_data"
	 * have an additional configuration option "entries", which is an
	 * integer type, defaults to 5, and must be an integer between 1 and 10.
	 *
	 * The method being configured is specified using a concatenation of the
	 * handle name, a period and the method name.
	 */
	ret = conn->configure_method(conn,
	    "WT_SESSION.open_cursor",
	    "my_data:", "entries=5", "int", "min=1,max=10");

	/*
	 * Applications opening a cursor for the data-source object "my_data"
	 * have an additional configuration option "devices", which is a list
	 * of strings.
	 */
	ret = conn->configure_method(conn,
	    "WT_SESSION.open_cursor", "my_data:", "devices", "list", NULL);
	/*! [Configure method configuration] */

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
backup(WT_SESSION *session)
{
	char buf[1024];

	/*! [backup]*/
	WT_CURSOR *cursor;
	const char *filename;
	int ret;

	/* Create the backup directory. */
	ret = mkdir("/path/database.backup", 077);

	/* Open the backup data source. */
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
		    progname, session->strerror(session, ret));

	ret = cursor->close(cursor);
	/*! [backup]*/

	/*! [incremental backup]*/
	/* Open the backup data source for incremental backup. */
	ret = session->open_cursor(
	    session, "backup:", NULL, "target=(\"log:\")", &cursor);
	/*! [incremental backup]*/
	ret = cursor->close(cursor);

	/*! [backup of a checkpoint]*/
	ret = session->checkpoint(session, "drop=(from=June01),name=June01");
	/*! [backup of a checkpoint]*/

	return (ret);
}

int
main(void)
{
	WT_CONNECTION *conn;
	int ret;

	/*
	 * Create a clean test directory for this run of the test program if the
	 * environment variable isn't already set (as is done by make check).
	 */
	if (getenv("WIREDTIGER_HOME") == NULL) {
		home = "WT_HOME";
		ret = system("rm -rf WT_HOME && mkdir WT_HOME");
	} else
		home = NULL;

	/*! [Open a connection] */
	ret = wiredtiger_open(home, NULL,
	    "create,cache_size=5GB,log=(enabled,recover=on)", &conn);
	/*! [Open a connection] */

	if (ret == 0)
		ret = connection_ops(conn);
	/*
	 * The connection has been closed.
	 */

#ifdef MIGHT_NOT_RUN
	/*
	 * This example code gets run, and the compression libraries might not
	 * be installed, causing the open to fail.  The documentation requires
	 * the code snippets, use #ifdef's to avoid running it.
	 */
	/*! [Configure lz4 extension] */
	ret = wiredtiger_open(home, NULL,
	    "create,"
	    "extensions=[/usr/local/lib/libwiredtiger_lz4.so]", &conn);
	/*! [Configure lz4 extension] */
	if (ret == 0)
		(void)conn->close(conn, NULL);

	/*! [Configure snappy extension] */
	ret = wiredtiger_open(home, NULL,
	    "create,"
	    "extensions=[/usr/local/lib/libwiredtiger_snappy.so]", &conn);
	/*! [Configure snappy extension] */
	if (ret == 0)
		(void)conn->close(conn, NULL);

	/*! [Configure zlib extension] */
	ret = wiredtiger_open(home, NULL,
	    "create,"
	    "extensions=[/usr/local/lib/libwiredtiger_zlib.so]", &conn);
	/*! [Configure zlib extension] */
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

	/*! [Configure file_extend] */
	ret = wiredtiger_open(
	    home, NULL, "create,file_extend=(data=16MB)", &conn);
	/*! [Configure file_extend] */
	if (ret == 0)
		(void)conn->close(conn, NULL);

	/*! [Eviction configuration] */
	/*
	 * Configure eviction to begin at 90% full, and run until the cache
	 * is only 75% dirty.
	 */
	ret = wiredtiger_open(home, NULL,
	    "create,eviction_trigger=90,eviction_dirty_target=75", &conn);
	/*! [Eviction configuration] */
	if (ret == 0)
		(void)conn->close(conn, NULL);

	/*! [Eviction worker configuration] */
	/* Configure up to four eviction threads */
	ret = wiredtiger_open(home, NULL,
	    "create,eviction_trigger=90,eviction=(threads_max=4)", &conn);
	/*! [Eviction worker configuration] */
	if (ret == 0)
		(void)conn->close(conn, NULL);

	/*! [Statistics configuration] */
	ret = wiredtiger_open(home, NULL, "create,statistics=(all)", &conn);
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
	    "create, statistics_log=("
	    "sources=(\"lsm:table1\",\"lsm:table2\"), wait=5)",
	    &conn);
	/*! [Statistics logging with a table] */
	if (ret == 0)
		(void)conn->close(conn, NULL);

	/*! [Statistics logging with all tables] */
	ret = wiredtiger_open(home, NULL,
	    "create, statistics_log=(sources=(\"lsm:\"), wait=5)",
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
	    "statistics_log=(wait=120,path=/log/log.%m.%d.%y)", &conn);
	/*! [Statistics logging with path] */
	if (ret == 0)
		(void)conn->close(conn, NULL);

	/*
	 * Don't run this code, because memory checkers get very upset when we
	 * leak memory.
	 */
	(void)wiredtiger_open(home, NULL, "create", &conn);
	/*! [Connection close leaking memory] */
	ret = conn->close(conn, "leak_memory=true");
	/*! [Connection close leaking memory] */
#endif

	/*! [Get the WiredTiger library version #1] */
	printf("WiredTiger version %s\n", wiredtiger_version(NULL, NULL, NULL));
	/*! [Get the WiredTiger library version #1] */

	{
	/*! [Get the WiredTiger library version #2] */
	int major_v, minor_v, patch;
	(void)wiredtiger_version(&major_v, &minor_v, &patch);
	printf("WiredTiger version is %d, %d (patch %d)\n",
	    major_v, minor_v, patch);
	/*! [Get the WiredTiger library version #2] */
	}

	return (ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
