/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __cursor_get_key --
 *	WT_CURSOR->get_key default implementation.
 */
static int
__cursor_get_key(WT_CURSOR *cursor, ...)
{
	const char *fmt;
	va_list ap;
	int ret;

	va_start(ap, cursor);
	fmt = F_ISSET(cursor, WT_CURSTD_RAW) ? "u" : cursor->key_format;
	ret = wiredtiger_struct_unpackv(
	    cursor->key.data, cursor->key.size, fmt, ap);
	va_end(ap);

	return (ret);
}

/*
 * __cursor_get_value --
 *	WT_CURSOR->get_value default implementation.
 */
static int
__cursor_get_value(WT_CURSOR *cursor, ...)
{
	const char *fmt;
	va_list ap;
	int ret;

	va_start(ap, cursor);
	fmt = F_ISSET(cursor, WT_CURSTD_RAW) ? "u" : cursor->value_format;
	ret = wiredtiger_struct_unpackv(
	    cursor->value.data, cursor->value.size, fmt, ap);
	va_end(ap);

	return (ret);
}

/*
 * __cursor_set_key --
 *	WT_CURSOR->set_key default implementation.
 */
static void
__cursor_set_key(WT_CURSOR *cursor, ...)
{
	SESSION *session;
	WT_BUF *buf;
	WT_ITEM *item;
	const char *fmt, *str;
	size_t sz;
	va_list ap;
	int ret;

	session = (SESSION *)cursor->session;

	va_start(ap, cursor);
	fmt = F_ISSET(cursor, WT_CURSTD_RAW) ? "u" : cursor->key_format;
	if (fmt[0] == 'S' && fmt[1] == '\0') {
		str = va_arg(ap, const char *);
		sz = strlen(str) + 1;
		cursor->key.data = (void *)str;
	} else if (fmt[0] == 'u' && fmt[1] == '\0') {
		item = va_arg(ap, WT_ITEM *);
		sz = item->size;
		cursor->key.data = (void *)item->data;
	} else {
		buf = &cursor->key;
		sz = wiredtiger_struct_sizev(fmt, ap);
		if ((ret = __wt_buf_setsize(session, buf, sz)) == 0 &&
		    (ret = wiredtiger_struct_packv(buf->mem, sz, fmt, ap)) == 0)
			F_SET(cursor, WT_CURSTD_KEY_SET);
		else {
			cursor->saved_err = ret;
			F_CLR(cursor, WT_CURSTD_KEY_SET);
			return;
		}
	}
	WT_ASSERT(NULL, sz <= UINT32_MAX);
	cursor->key.size = (uint32_t)sz;
	va_end(ap);
}

/*
 * __cursor_set_value --
 *	WT_CURSOR->set_value default implementation.
 */
static void
__cursor_set_value(WT_CURSOR *cursor, ...)
{
	SESSION *session;
	WT_BUF *buf;
	WT_ITEM *item;
	const char *fmt, *str;
	size_t sz;
	va_list ap;
	int ret;

	session = (SESSION *)cursor->session;

	va_start(ap, cursor);
	fmt = F_ISSET(cursor, WT_CURSTD_RAW) ? "u" : cursor->value_format;
	if (fmt[0] == 'S' && fmt[1] == '\0') {
		str = va_arg(ap, const char *);
		sz = strlen(str) + 1;
		cursor->value.data = str;
	} else if (fmt[0] == 'u' && fmt[1] == '\0') {
		item = va_arg(ap, WT_ITEM *);
		sz = item->size;
		cursor->value.data = item->data;
	} else {
		buf = &cursor->value;
		sz = wiredtiger_struct_sizev(fmt, ap);
		if ((ret = __wt_buf_setsize(session, buf, sz)) == 0 &&
		    (ret = wiredtiger_struct_packv(buf->mem, sz, fmt, ap)) == 0)
			F_SET(cursor, WT_CURSTD_KEY_SET);
		else {
			cursor->saved_err = ret;
			F_CLR(cursor, WT_CURSTD_KEY_SET);
			return;
		}
		cursor->value.data = buf->mem;
	}
	WT_ASSERT(NULL, sz <= UINT32_MAX);
	cursor->value.size = (uint32_t)sz;
	va_end(ap);
}

/*
 * __cursor_search --
 *	WT_CURSOR->search default implementation.
 */
static int
__cursor_search(WT_CURSOR *cursor)
{
	int lastcmp;

	WT_RET(cursor->search_near(cursor, &lastcmp));
	return ((lastcmp != 0) ? WT_NOTFOUND : 0);
}

/*
 * __wt_cursor_close --
 *	WT_CURSOR->close default implementation.
 */
int
__wt_cursor_close(WT_CURSOR *cursor, const char *config)
{
	SESSION *session;
	int ret;

	WT_UNUSED(config);

	session = (SESSION *)cursor->session;
	ret = 0;

	__wt_buf_free(session, &cursor->key);
	__wt_buf_free(session, &cursor->value);

	TAILQ_REMOVE(&session->cursors, cursor, q);
	__wt_free(session, cursor);

	return (ret);
}

/*
 * __wt_cursor_init --
 *	Default cursor initialization.
 */
void
__wt_cursor_init(WT_CURSOR *cursor, const char *config)
{
	WT_UNUSED(config);

	cursor->get_key = __cursor_get_key;
	cursor->get_value = __cursor_get_value;
	cursor->set_key = __cursor_set_key;
	cursor->set_value = __cursor_set_value;

	if (cursor->search == NULL)
		cursor->search = __cursor_search;

	WT_CLEAR(cursor->key);
	WT_CLEAR(cursor->value);

	cursor->flags = 0;
}
