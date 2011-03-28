/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __convert_to_dump --
 *	We have a scratch buffer where the data item contains a raw value,
 *	convert it to a dumpable string.
 */
static int
__convert_to_dump(SESSION *session, WT_BUF *buf)
{
	WT_UNUSED(session);
	WT_UNUSED(buf);

	return (0);
}

/*
 * convert_from_dump --
 *	We have a scratch buffer where the data item contains a dump string,
 *	convert it to a raw value.
 */
static int
__convert_from_dump(SESSION *session, WT_BUF *buf)
{
	WT_UNUSED(session);
	WT_UNUSED(buf);

	return (0);
}

/*
 * __curdump_get_key --
 *	WT_CURSOR->get_key for dump cursors.
 */
static int
__curdump_get_key(WT_CURSOR *cursor, ...)
{
	SESSION *session;
	WT_ITEM *key;
	va_list ap;

	session = (SESSION *)cursor->session;

	if (!F_ISSET(cursor, WT_CURSTD_KEY_SET))
		return ((cursor->saved_err != 0) ? cursor->saved_err : EINVAL);

	if (F_ISSET(cursor, WT_CURSTD_KEY_RAW)) {
		WT_RET(__convert_to_dump(session, &cursor->key));
		F_CLR(cursor, WT_CURSTD_KEY_RAW);
	}
	va_start(ap, cursor);
	key = va_arg(ap, WT_ITEM *);
	key->data = cursor->key.data;
	key->size = cursor->key.size;
	va_end(ap);

	return (0);
}

/*
 * __curdump_get_value --
 *	WT_CURSOR->get_value for dump cursors.
 */
static int
__curdump_get_value(WT_CURSOR *cursor, ...)
{
	SESSION *session;
	WT_ITEM *value;
	va_list ap;

	session = (SESSION *)cursor->session;

	if (!F_ISSET(cursor, WT_CURSTD_VALUE_SET))
		return ((cursor->saved_err != 0) ? cursor->saved_err : EINVAL);

	if (F_ISSET(cursor, WT_CURSTD_VALUE_RAW)) {
		WT_RET(__convert_to_dump(session, &cursor->value));
		F_SET(cursor, WT_CURSTD_VALUE_RAW);
	}
	va_start(ap, cursor);
	value = va_arg(ap, WT_ITEM *);
	value->data = cursor->value.data;
	value->size = cursor->value.size;
	va_end(ap);

	return (0);
}

/*
 * __curdump_set_key --
 *	WT_CURSOR->set_key for dump cursors.
 */
static void
__curdump_set_key(WT_CURSOR *cursor, ...)
{
	SESSION *session;
	va_list ap;
	int ret;

	session = (SESSION *)cursor->session;

	va_start(ap, cursor);
	*(WT_ITEM *)&cursor->key = *va_arg(ap, WT_ITEM *);

	if ((ret = __convert_from_dump(session, &cursor->key)) == 0)
		F_SET(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_KEY_RAW);
	else {
		cursor->saved_err = ret;
		F_CLR(cursor, WT_CURSTD_KEY_SET);
	}

	va_end(ap);
}

/*
 * __curdump_set_value --
 *	WT_CURSOR->set_value for dump cursors.
 */
static void
__curdump_set_value(WT_CURSOR *cursor, ...)
{
	SESSION *session;
	va_list ap;
	int ret;

	session = (SESSION *)cursor->session;

	va_start(ap, cursor);
	*(WT_ITEM *)&cursor->value = *va_arg(ap, WT_ITEM *);

	if ((ret = __convert_from_dump(session, &cursor->value)) == 0)
		F_SET(cursor, WT_CURSTD_VALUE_SET | WT_CURSTD_VALUE_RAW);
	else {
		cursor->saved_err = ret;
		F_CLR(cursor, WT_CURSTD_VALUE_SET);
	}

	va_end(ap);
}

/*
 * __wt_curdump_init --
 *	initialize a dump cursor.
 */
void
__wt_curdump_init(WT_CURSOR *cursor)
{
	cursor->get_key = __curdump_get_key;
	cursor->get_value = __curdump_get_value;
	cursor->set_key = __curdump_set_key;
	cursor->set_value = __curdump_set_value;
}
