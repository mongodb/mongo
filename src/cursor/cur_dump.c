/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __raw_to_dump --
 *	We have a buffer where the data item contains a raw value, convert it
 * to a printable string.
 */
static int
__raw_to_dump(
    WT_SESSION_IMPL *session, WT_ITEM *from, WT_BUF *to, int hexonly)
{
	WT_BUF *tmp;
	uint32_t size;

	/*
	 * In the worst case, every character takes up 3 spaces, plus a
	 * trailing nul byte.
	 */
	WT_RET(__wt_scr_alloc(session, from->size * 3 + 10, &tmp));
	size = from->size;
	if (hexonly)
		__wt_raw_to_hex(from->data, tmp->mem, &size);
	else
		__wt_raw_to_esc_hex(from->data, tmp->mem, &size);
	tmp->size = size;

	__wt_buf_swap(to, tmp);
	__wt_scr_free(&tmp);

	return (0);
}

/*
 * __dump_to_raw --
 *	We have a scratch buffer where the data item contains a dump string,
 *	convert it to a raw value.
 */
static int
__dump_to_raw(
    WT_SESSION_IMPL *session, const char *src_arg, WT_ITEM *item, int hexonly)
{
	uint32_t size;

	/*
	 * XXX
	 * Overwrite the string in place: the underlying cursor set_key and
	 * set_value functions are going to use the cursor's key and value
	 * buffers, which means we can't.  This should probably be fixed by
	 * layering the dump cursor on top of other cursors and then we can
	 * use the dump cursor's key/value buffers.
	 */
	if (hexonly)
		WT_RET(__wt_hex_to_raw(
		    session, (void *)src_arg, (void *)src_arg, &size));
	else
		WT_RET(__wt_esc_hex_to_raw(
		    session, (void *)src_arg, (void *)src_arg, &size));

	memset(item, 0, sizeof(WT_ITEM));
	item->data = src_arg;
	item->size = size;
	return (0);
}

/*
 * __curdump_get_key --
 *	WT_CURSOR->get_key for dump cursors.
 */
static int
__curdump_get_key(WT_CURSOR *cursor, ...)
{
	WT_ITEM item, *itemp;
	WT_SESSION_IMPL *session;
	int ret;
	uint64_t recno;
	va_list ap;

	CURSOR_API_CALL(cursor, session, get_key, NULL);

	if (WT_CURSOR_RECNO(cursor) && !F_ISSET(cursor, WT_CURSTD_RAW)) {
		if (F_ISSET(cursor, WT_CURSTD_TABLE))
			WT_ERR(__wt_curtable_get_key(cursor, &recno));
		else
			WT_ERR(__wt_cursor_get_key(cursor, &recno));

		WT_ERR(__wt_buf_fmt(session, &cursor->key, "%" PRIu64, recno));
	} else {
		if (F_ISSET(cursor, WT_CURSTD_TABLE))
			WT_ERR(__wt_curtable_get_key(cursor, &item));
		else
			WT_ERR(__wt_cursor_get_key(cursor, &item));

		WT_ERR(__raw_to_dump(session, &item,
		    &cursor->key, F_ISSET(cursor, WT_CURSTD_DUMP_HEX) ? 1 : 0));
	}

	va_start(ap, cursor);
	if (F_ISSET(cursor, WT_CURSTD_RAW)) {
		itemp = va_arg(ap, WT_ITEM *);
		itemp->data = cursor->key.data;
		itemp->size = cursor->key.size;
	} else
		*va_arg(ap, const char **) = cursor->key.data;
	va_end(ap);

err:	API_END(session);
	return (ret);
}

/*
 * str2recno --
 *	Convert a string to a record number.
 */
static int
str2recno(WT_SESSION_IMPL *session, const char *p, uint64_t *recnop)
{
	uint64_t recno;
	char *endptr;

	/*
	 * strtouq takes lots of things like hex values, signs and so on and so
	 * forth -- none of them are OK with us.  Check the string starts with
	 * digit, that turns off the special processing.
	 */
	if (!isdigit(p[0]))
		goto format;

	errno = 0;
	recno = strtouq(p, &endptr, 0);
	if (recno == ULLONG_MAX && errno == ERANGE)
		WT_RET_MSG(session, ERANGE, "%s: invalid record number", p);
	if (endptr[0] != '\0')
format:		WT_RET_MSG(session, EINVAL, "%s: invalid record number", p);

	*recnop = recno;
	return (0);
}

/*
 * __curdump_set_key --
 *	WT_CURSOR->set_key for dump cursors.
 */
static void
__curdump_set_key(WT_CURSOR *cursor, ...)
{
	WT_ITEM item;
	WT_SESSION_IMPL *session;
	uint64_t recno;
	va_list ap;
	const char *p;
	int ret;

	CURSOR_API_CALL(cursor, session, set_key, NULL);

	va_start(ap, cursor);
	if (F_ISSET(cursor, WT_CURSTD_RAW))
		p = va_arg(ap, WT_ITEM *)->data;
	else
		p = va_arg(ap, const char *);
	va_end(ap);

	if (WT_CURSOR_RECNO(cursor) && !F_ISSET(cursor, WT_CURSTD_RAW)) {
		WT_ERR(str2recno(session, p, &recno));

		if (F_ISSET(cursor, WT_CURSTD_TABLE))
			__wt_curtable_set_key(cursor, recno);
		else
			__wt_cursor_set_key(cursor, recno);
	} else {
		WT_ERR(__dump_to_raw(session,
		    p, &item, F_ISSET(cursor, WT_CURSTD_DUMP_HEX) ? 1 : 0));

		if (F_ISSET(cursor, WT_CURSTD_TABLE))
			__wt_curtable_set_key(cursor, &item);
		else
			__wt_cursor_set_key(cursor, &item);
	}

	if (0) {
err:		cursor->saved_err = ret;
		F_CLR(cursor, WT_CURSTD_KEY_SET);
	}

	API_END(session);
}

/*
 * __curdump_get_value --
 *	WT_CURSOR->get_value for dump cursors.
 */
static int
__curdump_get_value(WT_CURSOR *cursor, ...)
{
	WT_ITEM item, *itemp;
	WT_SESSION_IMPL *session;
	va_list ap;
	int ret;

	CURSOR_API_CALL(cursor, session, get_value, NULL);

	if (F_ISSET(cursor, WT_CURSTD_TABLE))
		WT_ERR(__wt_curtable_get_value(cursor, &item));
	else
		WT_ERR(__wt_cursor_get_value(cursor, &item));

	WT_ERR(__raw_to_dump(session, &item,
	    &cursor->value, F_ISSET(cursor, WT_CURSTD_DUMP_HEX) ? 1 : 0));

	va_start(ap, cursor);
	if (F_ISSET(cursor, WT_CURSTD_RAW)) {
		itemp = va_arg(ap, WT_ITEM *);
		itemp->data = cursor->value.data;
		itemp->size = cursor->value.size;
	} else
		*va_arg(ap, const char **) = cursor->value.data;
	va_end(ap);

err:	API_END(session);
	return (ret);
}

/*
 * __curdump_set_value --
 *	WT_CURSOR->set_value for dump cursors.
 */
static void
__curdump_set_value(WT_CURSOR *cursor, ...)
{
	WT_ITEM item;
	WT_SESSION_IMPL *session;
	va_list ap;
	int ret;
	const char *p;

	CURSOR_API_CALL(cursor, session, set_value, NULL);

	va_start(ap, cursor);
	if (F_ISSET(cursor, WT_CURSTD_RAW))
		p = va_arg(ap, WT_ITEM *)->data;
	else
		p = va_arg(ap, const char *);
	va_end(ap);

	WT_ERR(__dump_to_raw(session,
	    p, &item, F_ISSET(cursor, WT_CURSTD_DUMP_HEX) ? 1 : 0));

	if (F_ISSET(cursor, WT_CURSTD_TABLE))
		__wt_curtable_set_value(cursor, &item);
	else
		__wt_cursor_set_value(cursor, &item);

	if (0) {
err:		cursor->saved_err = ret;
		F_CLR(cursor, WT_CURSTD_VALUE_SET);
	}

	API_END(session);
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
