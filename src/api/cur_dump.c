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
__convert_to_dump(CONNECTION *conn, WT_SCRATCH *scratch)
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
__convert_from_dump(CONNECTION *conn, WT_SCRATCH *scratch)
{
	WT_UNUSED(conn);
	WT_UNUSED(scratch);

	return (0);
}

static int
__curdump_get_key(WT_CURSOR *cursor, ...)
{
	CONNECTION *conn;
	WT_CURSOR_STD *stdc = (WT_CURSOR_STD *)cursor;
	va_list ap;

	conn = (CONNECTION *)cursor->session->connection;

	if (!F_ISSET(stdc, WT_CURSTD_DUMPKEY)) {
		WT_RET(__convert_to_dump(conn, &stdc->key));
		F_SET(stdc, WT_CURSTD_DUMPKEY);
	}
	va_start(ap, cursor);
	*va_arg(ap, WT_DATAITEM *) = stdc->key.item;
	va_end(ap);

	return (0);
}

static int
__curdump_get_value(WT_CURSOR *cursor, ...)
{
	CONNECTION *conn;
	WT_CURSOR_STD *stdc = (WT_CURSOR_STD *)cursor;
	va_list ap;

	conn = (CONNECTION *)cursor->session->connection;

	if (!F_ISSET(stdc, WT_CURSTD_DUMPVALUE)) {
		WT_RET(__convert_to_dump(conn, &stdc->value));
		F_SET(stdc, WT_CURSTD_DUMPVALUE);
	}
	va_start(ap, cursor);
	*va_arg(ap, WT_DATAITEM *) = stdc->value.item;
	va_end(ap);

	return (0);
}

static void
__curdump_set_key(WT_CURSOR *cursor, ...)
{
	CONNECTION *conn;
	WT_CURSOR_STD *stdc = (WT_CURSOR_STD *)cursor;
	va_list ap;

	conn = (CONNECTION *)cursor->session->connection;

	va_start(ap, cursor);
	stdc->key.item = *va_arg(ap, WT_DATAITEM *);

	if (__convert_from_dump(conn, &stdc->key) == 0)
		F_CLR(stdc, WT_CURSTD_BADKEY);
	else
		F_SET(stdc, WT_CURSTD_BADKEY);

	va_end(ap);
}

static void
__curdump_set_value(WT_CURSOR *cursor, ...)
{
	CONNECTION *conn;
	WT_CURSOR_STD *stdc = (WT_CURSOR_STD *)cursor;
	va_list ap;

	conn = (CONNECTION *)cursor->session->connection;

	va_start(ap, cursor);
	stdc->value.item = *va_arg(ap, WT_DATAITEM *);

	if (__convert_from_dump(conn, &stdc->value) == 0)
		F_CLR(stdc, WT_CURSTD_BADKEY);
	else
		F_SET(stdc, WT_CURSTD_BADKEY);

	va_end(ap);
}

void
__wt_curdump_init(WT_CURSOR_STD *stdc)
{
	WT_CURSOR *c = &stdc->iface;

	c->get_key = __curdump_get_key;
	c->get_value = __curdump_get_value;
	c->set_key = __curdump_set_key;
	c->set_value = __curdump_set_value;
}
