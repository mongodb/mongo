/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * We have a scratch buffer where the data item contains a raw value,
 * convert it to a dumpable string.
 */
static int
__convert_to_dump(CONNECTION *conn, WT_BUF *scratch)
{
	WT_UNUSED(conn);
	WT_UNUSED(scratch);

	return (0);
}

/*
 * We have a scratch buffer where the data item contains a dump string,
 * convert it to a raw value.
 */
static int
__convert_from_dump(CONNECTION *conn, WT_BUF *scratch)
{
	WT_UNUSED(conn);
	WT_UNUSED(scratch);

	return (0);
}

static int
__curdump_get_key(WT_CURSOR *cursor, ...)
{
	CONNECTION *conn;
	va_list ap;

	conn = (CONNECTION *)cursor->session->connection;

	if (!F_ISSET(cursor, WT_CURSTD_DUMPKEY)) {
		WT_RET(__convert_to_dump(conn, &cursor->key));
		F_SET(cursor, WT_CURSTD_DUMPKEY);
	}
	va_start(ap, cursor);
	*va_arg(ap, WT_ITEM *) = cursor->key.item;
	va_end(ap);

	return (0);
}

static int
__curdump_get_value(WT_CURSOR *cursor, ...)
{
	CONNECTION *conn;
	va_list ap;

	conn = (CONNECTION *)cursor->session->connection;

	if (!F_ISSET(cursor, WT_CURSTD_DUMPVALUE)) {
		WT_RET(__convert_to_dump(conn, &cursor->value));
		F_SET(cursor, WT_CURSTD_DUMPVALUE);
	}
	va_start(ap, cursor);
	*va_arg(ap, WT_ITEM *) = cursor->value.item;
	va_end(ap);

	return (0);
}

static void
__curdump_set_key(WT_CURSOR *cursor, ...)
{
	CONNECTION *conn;
	va_list ap;

	conn = (CONNECTION *)cursor->session->connection;

	va_start(ap, cursor);
	cursor->key.item = *va_arg(ap, WT_ITEM *);

	if (__convert_from_dump(conn, &cursor->key) == 0)
		F_CLR(cursor, WT_CURSTD_BADKEY);
	else
		F_SET(cursor, WT_CURSTD_BADKEY);

	va_end(ap);
}

static void
__curdump_set_value(WT_CURSOR *cursor, ...)
{
	CONNECTION *conn;
	va_list ap;

	conn = (CONNECTION *)cursor->session->connection;

	va_start(ap, cursor);
	cursor->value.item = *va_arg(ap, WT_ITEM *);

	if (__convert_from_dump(conn, &cursor->value) == 0)
		F_CLR(cursor, WT_CURSTD_BADKEY);
	else
		F_SET(cursor, WT_CURSTD_BADKEY);

	va_end(ap);
}

void
__wt_curdump_init(WT_CURSOR *cursor)
{
	cursor->get_key = __curdump_get_key;
	cursor->get_value = __curdump_get_value;
	cursor->set_key = __curdump_set_key;
	cursor->set_value = __curdump_set_value;
}
