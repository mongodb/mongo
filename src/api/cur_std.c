/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int
__cursor_get_key(WT_CURSOR *cursor, ...)
{
	WT_ITEM *item;
	const char *fmt;
	va_list ap;
	int ret;

	va_start(ap, cursor);
	fmt = F_ISSET(cursor, WT_CURSTD_RAW) ? "u" : cursor->key_format;
	item = &cursor->key.item;
	ret = wiredtiger_struct_unpackv(item->data, item->size, fmt, ap);
	va_end(ap);

	return (ret);
}

static int
__cursor_get_value(WT_CURSOR *cursor, ...)
{
	WT_ITEM *item;
	const char *fmt;
	va_list ap;
	int ret;

	va_start(ap, cursor);
	fmt = F_ISSET(cursor, WT_CURSTD_RAW) ? "u" : cursor->value_format;
	item = &cursor->value.item;
	ret = wiredtiger_struct_unpackv(item->data, item->size, fmt, ap);
	va_end(ap);

	return (ret);
}

static void
__cursor_set_key(WT_CURSOR *cursor, ...)
{
	SESSION *session;
	WT_ITEM *item;
	const char *fmt, *str;
	size_t sz;
	va_list ap;

	session = (SESSION *)cursor->session;

	va_start(ap, cursor);
	fmt = F_ISSET(cursor, WT_CURSTD_RAW) ? "u" : cursor->key_format;
	if (fmt[0] == 'S' && fmt[1] == '\0') {
		str = va_arg(ap, const char *);
		sz = strlen(str) + 1;
		cursor->key.item.data = str;
	} else if (fmt[0] == 'u' && fmt[1] == '\0') {
		item = va_arg(ap, WT_ITEM *);
		sz = item->size;
		cursor->key.item.data = item->data;
	} else {
		sz = wiredtiger_struct_sizev(fmt, ap);
		if (__wt_buf_grow(session, &cursor->key, sz) == 0 &&
		    wiredtiger_struct_packv(cursor->key.mem, sz, fmt, ap) == 0)
			F_CLR(cursor, WT_CURSTD_BADKEY);
		else {
			F_SET(cursor, WT_CURSTD_BADKEY);
			return;
		}
		cursor->key.item.data = cursor->key.mem;
	}
	WT_ASSERT(NULL, sz <= UINT32_MAX);
	cursor->key.item.size = (uint32_t)sz;
	va_end(ap);
}

static void
__cursor_set_value(WT_CURSOR *cursor, ...)
{
	SESSION *session;
	WT_ITEM *item;
	const char *fmt, *str;
	size_t sz;
	va_list ap;

	session = (SESSION *)cursor->session;

	va_start(ap, cursor);
	fmt = F_ISSET(cursor, WT_CURSTD_RAW) ? "u" : cursor->value_format;
	if (fmt[0] == 'S' && fmt[1] == '\0') {
		str = va_arg(ap, const char *);
		sz = strlen(str) + 1;
		cursor->value.item.data = str;
	} else if (fmt[0] == 'u' && fmt[1] == '\0') {
		item = va_arg(ap, WT_ITEM *);
		sz = item->size;
		cursor->value.item.data = item->data;
	} else {
		sz = wiredtiger_struct_sizev(fmt, ap);
		if (__wt_buf_grow(session, &cursor->value, sz) == 0 &&
		    wiredtiger_struct_packv(
		    cursor->value.mem, sz, fmt, ap) == 0)
			F_CLR(cursor, WT_CURSTD_BADVALUE);
		else {
			F_SET(cursor, WT_CURSTD_BADVALUE);
			return;
		}
		cursor->value.item.data = cursor->value.mem;
	}
	WT_ASSERT(NULL, sz <= UINT32_MAX);
	cursor->value.item.size = (uint32_t)sz;
	va_end(ap);
}

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

void
__wt_cursor_init(WT_CURSOR *cursor)
{
	cursor->get_key = __cursor_get_key;
	cursor->get_value = __cursor_get_value;
	cursor->set_key = __cursor_set_key;
	cursor->set_value = __cursor_set_value;

	cursor->value.item.data = cursor->value.mem = NULL;
	cursor->value.mem_size = 0;

	cursor->flags = WT_CURSTD_BADKEY | WT_CURSTD_BADVALUE;
}
