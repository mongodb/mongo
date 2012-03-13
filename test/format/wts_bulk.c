/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "format.h"

static int  bulk(WT_ITEM **, WT_ITEM **);

int
wts_bulk_load(void)
{
	WT_CURSOR *cursor;
	WT_SESSION *session;
	WT_ITEM *key, *value;
	uint64_t insert_count;
	int ret;

	session = g.wts_session;
	key = value = NULL;		/* -Wuninitialized */

	/*
	 * Avoid bulk load with a custom collator, because the order of
	 * insertion will not match the collation order.
	 */
	if ((ret = session->open_cursor(session, WT_TABLENAME, NULL,
	    (g.c_file_type == ROW && g.c_reverse) ? NULL : "bulk",
	    &cursor)) != 0) {
		fprintf(stderr, "%s: cursor open failed: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}

	insert_count = 0;
	while (bulk(&key, &value) == 0) {
		/* Report on progress every 100 inserts. */
		if (++insert_count % 100 == 0)
			track("bulk load", insert_count);

		if (key != NULL)
			cursor->set_key(cursor, key);
		if (g.c_file_type == FIX)
			cursor->set_value(cursor, *(uint8_t *)value->data);
		else
			cursor->set_value(cursor, value);
		if ((ret = cursor->insert(cursor)) != 0) {
			fprintf(stderr, "%s: cursor insert failed: %s\n",
			    g.progname, wiredtiger_strerror(ret));
			ret = 1;
			goto err;
		}
	}

err:	(void)cursor->close(cursor);
	return (ret);
}

/*
 * bulk --
 *	WiredTiger bulk load routine.
 */
static int
bulk(WT_ITEM **keyp, WT_ITEM **valuep)
{
	static WT_ITEM key, value;
	WT_SESSION *session;

	session = g.wts_session;

	if (++g.key_cnt > g.c_rows) {
		g.key_cnt = g.rows = g.c_rows;
		return (1);
	}

	key_gen(&key.data, &key.size, (uint64_t)g.key_cnt, 0);
	value_gen(&value.data, &value.size, (uint64_t)g.key_cnt);

	switch (g.c_file_type) {
	case FIX:
		*keyp = NULL;
		*valuep = &value;
		if (g.logging == LOG_OPS)
			(void)session->msg_printf(session,
			    "%-10s %" PRIu32 " {0x%02" PRIx8 "}",
			    "bulk V",
			    g.key_cnt, ((uint8_t *)value.data)[0]);
		break;
	case VAR:
		*keyp = NULL;
		*valuep = &value;
		if (g.logging == LOG_OPS)
			(void)session->msg_printf(session,
			    "%-10s %" PRIu32 " {%.*s}", "bulk V",
			    g.key_cnt, (int)value.size, (char *)value.data);
		break;
	case ROW:
		*keyp = &key;
		if (g.logging == LOG_OPS)
			(void)session->msg_printf(session,
			    "%-10s %" PRIu32 " {%.*s}", "bulk K",
		    g.key_cnt, (int)key.size, (char *)key.data);
		*valuep = &value;
		if (g.logging == LOG_OPS)
			(void)session->msg_printf(session,
			    "%-10s %" PRIu32 " {%.*s}", "bulk V",
			    g.key_cnt, (int)value.size, (char *)value.data);
		break;
	}

	/* Insert the item into BDB. */
	bdb_insert(key.data, key.size, value.data, value.size);

	return (0);
}
