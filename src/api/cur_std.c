/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

void
__wt_scratch_init(WT_SCRATCH *scratch)
{
	scratch->item.data = scratch->buf = NULL;
	scratch->mem_size = scratch->item.size = 0;
}

int
__wt_scratch_grow(ENV *env, WT_SCRATCH *scratch, size_t sz)
{
	if (sz > scratch->mem_size)
		WT_RET(__wt_realloc(env, &scratch->mem_size, sz, &scratch->buf));

	scratch->item.data = scratch->buf;
	WT_ASSERT(env, sz < UINT32_MAX);
	scratch->item.size = (uint32_t)sz;
	return (0);
}

void
__wt_scratch_free(ENV *env, WT_SCRATCH *scratch)
{

	if (scratch->buf != NULL)
		__wt_free(env, scratch->buf, scratch->mem_size);

	scratch->item.data = NULL;
	scratch->mem_size = scratch->item.size = 0;
}

static int
__curstd_get_key(WT_CURSOR *cursor, ...)
{
	WT_CURSOR_STD *cstd = (WT_CURSOR_STD *)cursor;
	WT_DATAITEM *item;
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
	WT_DATAITEM *item;
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
	ENV *env;
	WT_CURSOR_STD *cstd = (WT_CURSOR_STD *)cursor;
	WT_DATAITEM *item;
	const char *fmt, *str;
	size_t sz;
	va_list ap;

	env = ((ICONNECTION *)cursor->session->connection)->env;

	va_start(ap, cursor);
	fmt = F_ISSET(cstd, WT_CURSTD_RAW) ? "u" : cursor->key_format;
	if (fmt[0] == 'S' && fmt[1] == '\0') {
		str = va_arg(ap, const char *);
		sz = strlen(str) + 1;
		cstd->key.item.data = str;
	} else if (fmt[0] == 'u' && fmt[1] == '\0') {
		item = va_arg(ap, WT_DATAITEM *);
		sz = item->size;
		cstd->key.item.data = item->data;
	} else {
		sz = wiredtiger_struct_sizev(fmt, ap);
		if (__wt_scratch_grow(env, &cstd->key, sz) == 0 &&
		    wiredtiger_struct_packv(cstd->key.buf, sz, fmt, ap) == 0)
			F_CLR(cstd, WT_CURSTD_BADKEY);
		else {
			F_SET(cstd, WT_CURSTD_BADKEY);
			return;
		}
		cstd->key.item.data = cstd->key.buf;
	}
	WT_ASSERT(NULL, sz <= UINT32_MAX);
	cstd->key.item.size = (uint32_t)sz;
	va_end(ap);
}

static void
__curstd_set_value(WT_CURSOR *cursor, ...)
{
	ENV *env;
	WT_CURSOR_STD *cstd = (WT_CURSOR_STD *)cursor;
	WT_DATAITEM *item;
	const char *fmt, *str;
	size_t sz;
	va_list ap;

	env = ((ICONNECTION *)cursor->session->connection)->env;

	va_start(ap, cursor);
	fmt = F_ISSET(cstd, WT_CURSTD_RAW) ? "u" : cursor->value_format;
	if (fmt[0] == 'S' && fmt[1] == '\0') {
		str = va_arg(ap, const char *);
		sz = strlen(str) + 1;
		cstd->value.item.data = str;
	} else if (fmt[0] == 'u' && fmt[1] == '\0') {
		item = va_arg(ap, WT_DATAITEM *);
		sz = item->size;
		cstd->value.item.data = item->data;
	} else {
		sz = wiredtiger_struct_sizev(fmt, ap);
		if (__wt_scratch_grow(env, &cstd->value, sz) == 0 &&
		    wiredtiger_struct_packv(cstd->value.buf, sz, fmt, ap) == 0)
			F_CLR(cstd, WT_CURSTD_BADVALUE);
		else {
			F_SET(cstd, WT_CURSTD_BADVALUE);
			return;
		}
		cstd->value.item.data = cstd->value.buf;
	}
	WT_ASSERT(NULL, sz <= UINT32_MAX);
	cstd->value.item.size = (uint32_t)sz;
	va_end(ap);
}

int
__wt_curstd_close(WT_CURSOR *cursor, const char *config)
{
	ENV *env;
	ISESSION *isession;
	WT_CURSOR_STD *cstd;
	int ret;

	WT_UNUSED(config);

	cstd = (WT_CURSOR_STD *)cursor;
	env = ((ICONNECTION *)cursor->session->connection)->env;
	isession = (ISESSION *)cursor->session;
	ret = 0;

	__wt_scratch_free(env, &cstd->key);
	__wt_scratch_free(env, &cstd->value);

	TAILQ_REMOVE(&isession->cursors, cstd, q);
	__wt_free(NULL, cursor, sizeof(ICURSOR_TABLE));

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

	cstd->value.item.data = cstd->value.buf = NULL;
	cstd->value.mem_size = 0;

	cstd->flags = WT_CURSTD_BADKEY | WT_CURSTD_BADVALUE;
}
