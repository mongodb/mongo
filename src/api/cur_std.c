/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int
__curstd_get_key(WT_CURSOR *cursor, ...)
{
	WT_CURSOR_STD *cstd = (WT_CURSOR_STD *)cursor;
	WT_ITEM *item;
	const char *fmt;
	va_list ap;
	int ret;

	va_start(ap, cursor);
	fmt = F_ISSET(cstd, WT_CURSTD_RAW) ? "u" : cursor->key_format;
	item = &cstd->key.item;
	ret = wiredtiger_struct_unpackv(item->data, item->size, fmt, ap);
	va_end(ap);

	return (ret);
}

static int
__curstd_get_value(WT_CURSOR *cursor, ...)
{
	WT_CURSOR_STD *cstd = (WT_CURSOR_STD *)cursor;
	WT_ITEM *item;
	const char *fmt;
	va_list ap;
	int ret;

	va_start(ap, cursor);
	fmt = F_ISSET(cstd, WT_CURSTD_RAW) ? "u" : cursor->value_format;
	item = &cstd->value.item;
	ret = wiredtiger_struct_unpackv(item->data, item->size, fmt, ap);
	va_end(ap);

	return (ret);
}

static void
__curstd_set_key(WT_CURSOR *cursor, ...)
{
	SESSION *session;
	WT_CURSOR_STD *cstd = (WT_CURSOR_STD *)cursor;
	WT_ITEM *item;
	const char *fmt, *str;
	size_t sz;
	va_list ap;

	session = (SESSION *)cursor->session;

	va_start(ap, cursor);
	fmt = F_ISSET(cstd, WT_CURSTD_RAW) ? "u" : cursor->key_format;
	if (fmt[0] == 'S' && fmt[1] == '\0') {
		str = va_arg(ap, const char *);
		sz = strlen(str) + 1;
		cstd->key.item.data = str;
	} else if (fmt[0] == 'u' && fmt[1] == '\0') {
		item = va_arg(ap, WT_ITEM *);
		sz = item->size;
		cstd->key.item.data = item->data;
	} else {
		sz = wiredtiger_struct_sizev(fmt, ap);
		if (__wt_buf_grow(session, &cstd->key, sz) == 0 &&
		    wiredtiger_struct_packv(cstd->key.mem, sz, fmt, ap) == 0)
			F_CLR(cstd, WT_CURSTD_BADKEY);
		else {
			F_SET(cstd, WT_CURSTD_BADKEY);
			return;
		}
		cstd->key.item.data = cstd->key.mem;
	}
	WT_ASSERT(NULL, sz <= UINT32_MAX);
	cstd->key.item.size = (uint32_t)sz;
	va_end(ap);
}

static void
__curstd_set_value(WT_CURSOR *cursor, ...)
{
	SESSION *session;
	WT_CURSOR_STD *cstd = (WT_CURSOR_STD *)cursor;
	WT_ITEM *item;
	const char *fmt, *str;
	size_t sz;
	va_list ap;

	session = (SESSION *)cursor->session;

	va_start(ap, cursor);
	fmt = F_ISSET(cstd, WT_CURSTD_RAW) ? "u" : cursor->value_format;
	if (fmt[0] == 'S' && fmt[1] == '\0') {
		str = va_arg(ap, const char *);
		sz = strlen(str) + 1;
		cstd->value.item.data = str;
	} else if (fmt[0] == 'u' && fmt[1] == '\0') {
		item = va_arg(ap, WT_ITEM *);
		sz = item->size;
		cstd->value.item.data = item->data;
	} else {
		sz = wiredtiger_struct_sizev(fmt, ap);
		if (__wt_buf_grow(session, &cstd->value, sz) == 0 &&
		    wiredtiger_struct_packv(cstd->value.mem, sz, fmt, ap) == 0)
			F_CLR(cstd, WT_CURSTD_BADVALUE);
		else {
			F_SET(cstd, WT_CURSTD_BADVALUE);
			return;
		}
		cstd->value.item.data = cstd->value.mem;
	}
	WT_ASSERT(NULL, sz <= UINT32_MAX);
	cstd->value.item.size = (uint32_t)sz;
	va_end(ap);
}

int
__wt_curstd_close(WT_CURSOR *cursor, const char *config)
{
	SESSION *session;
	WT_CURSOR_STD *cstd;
	int ret;

	WT_UNUSED(config);

	cstd = (WT_CURSOR_STD *)cursor;
	session = (SESSION *)cursor->session;
	ret = 0;

	__wt_buf_free(session, &cstd->key);
	__wt_buf_free(session, &cstd->value);

	TAILQ_REMOVE(&session->cursors, cstd, q);
	__wt_free(session, cursor, sizeof(ICURSOR_TABLE));

	return (ret);
}

void
__wt_curstd_init(WT_CURSOR_STD *cstd)
{
	WT_CURSOR *c = &cstd->iface;

	c->get_key = __curstd_get_key;
	c->get_value = __curstd_get_value;
	c->set_key = __curstd_set_key;
	c->set_value = __curstd_set_value;

	cstd->value.item.data = cstd->value.mem = NULL;
	cstd->value.mem_size = 0;

	cstd->flags = WT_CURSTD_BADKEY | WT_CURSTD_BADVALUE;
}
