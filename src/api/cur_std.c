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
	WT_CURSOR_STD *stdc = (WT_CURSOR_STD *)cursor;
	const char *fmt;
	va_list ap;

	va_start(ap, cursor);
	fmt = F_ISSET(stdc, WT_CURSTD_RAW) ? "u" : cursor->key_format;
	return wiredtiger_struct_unpackv(stdc->key.data, stdc->key.size,
	    F_ISSET(stdc, WT_CURSTD_RAW) ? "u" : cursor->key_format, ap);
}

static int
__curstd_get_value(WT_CURSOR *cursor, ...)
{
	WT_CURSOR_STD *stdc = (WT_CURSOR_STD *)cursor;
	const char *fmt;
	va_list ap;

	va_start(ap, cursor);
	fmt = F_ISSET(stdc, WT_CURSTD_RAW) ? "u" : cursor->value_format;
	return wiredtiger_struct_unpackv(stdc->value.data, stdc->value.size,
	    F_ISSET(stdc, WT_CURSTD_RAW) ? "u" : cursor->value_format, ap);
}

static void
__curstd_set_key(WT_CURSOR *cursor, ...)
{
	WT_CURSOR_STD *stdc = (WT_CURSOR_STD *)cursor;
	va_list ap;
	const char *fmt;
	size_t sz;

	va_start(ap, cursor);
	fmt = F_ISSET(stdc, WT_CURSTD_RAW) ? "u" : cursor->key_format;
	sz = wiredtiger_struct_sizev(fmt, ap);
	if (stdc->keybufsz < sz) {
		stdc->keybuf = (stdc->keybuf == NULL) ?
		    malloc(sz) : realloc(stdc->keybuf, sz);
		/* TODO ENOMEM */
		stdc->keybufsz = sz;
	}
	stdc->key.data = stdc->keybuf;
	stdc->key.size = sz;
	if (wiredtiger_struct_packv(stdc->keybuf, sz, fmt, ap) == 0)
		stdc->flags &= ~WT_CURSTD_BADKEY;
}

static void
__curstd_set_value(WT_CURSOR *cursor, ...)
{
	WT_CURSOR_STD *stdc = (WT_CURSOR_STD *)cursor;
	const char *fmt;
	size_t sz;
	va_list ap;

	va_start(ap, cursor);
	fmt = F_ISSET(stdc, WT_CURSTD_RAW) ? "u" : cursor->value_format;
	sz = wiredtiger_struct_sizev(fmt, ap);
	if (stdc->valuebufsz < sz) {
		stdc->valuebuf = (stdc->valuebuf == NULL) ?
		    malloc(sz) : realloc(stdc->valuebuf, sz);
		/* TODO ENOMEM */
		stdc->valuebufsz = sz;
	}
	stdc->value.data = stdc->valuebuf;
	stdc->value.size = sz;
	if (wiredtiger_struct_packv(stdc->valuebuf, sz, fmt, ap) == 0)
		stdc->flags &= ~WT_CURSTD_BADVALUE;
}

void
__wt_curstd_init(WT_CURSOR_STD *stdc)
{
	WT_CURSOR *c = &stdc->interface;

	c->get_key = __curstd_get_key;
	c->get_value = __curstd_get_value;
	c->set_key = __curstd_set_key;
	c->set_value = __curstd_set_value;

	stdc->key.data = stdc->keybuf = NULL;
	stdc->keybufsz = 0;
	stdc->value.data = stdc->valuebuf = NULL;
	stdc->valuebufsz = 0;

	stdc->flags = WT_CURSTD_BADKEY | WT_CURSTD_BADVALUE;
}

void
__wt_curstd_close(WT_CURSOR_STD *c)
{
	if (c->key.data != NULL) {
		free((void *)c->key.data);
		c->key.data = NULL;
		c->keybufsz = 0;
	}
	if (c->value.data != NULL) {
		free((void *)c->value.data);
		c->value.data = NULL;
		c->valuebufsz = 0;
	}
}
