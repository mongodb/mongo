/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_kv_not_set --
 *	Standard error message for key/values not set.
 */
int
__wt_kv_not_set(WT_SESSION_IMPL *session, int key, int saved_err)
{
	WT_RET_MSG(session,
	    saved_err == 0 ? EINVAL : saved_err,
	    "requires %s be set", key ? "key" : "value");
}

/*
 * __wt_kv_set_keyv --
 *	WT_CURSOR->set_key default implementation.
 */
void
__wt_kv_set_keyv(
    WT_SESSION_IMPL *session, WT_CURSOR *cursor, uint32_t flags, va_list ap)
{
	WT_DECL_RET;
	WT_ITEM *buf, *item;
	size_t sz;
	va_list ap_copy;
	const char *fmt, *str;

	F_CLR(cursor, WT_CURSTD_KEY_SET);

	/* Fast path some common cases: single strings or byte arrays. */
	if (WT_CURSOR_RECNO(cursor)) {
		if (LF_ISSET(WT_CURSTD_RAW)) {
			item = va_arg(ap, WT_ITEM *);
			WT_ERR(__wt_struct_unpack(session,
			    item->data, item->size, "q", &cursor->recno));
		} else
			cursor->recno = va_arg(ap, uint64_t);
		if (cursor->recno == 0)
			WT_ERR_MSG(session, EINVAL,
			    "Record numbers must be greater than zero");
		cursor->key.data = &cursor->recno;
		sz = sizeof(cursor->recno);
	} else {
		fmt = cursor->key_format;
		if (LF_ISSET(WT_CURSOR_RAW_OK) || strcmp(fmt, "u") == 0) {
			item = va_arg(ap, WT_ITEM *);
			sz = item->size;
			cursor->key.data = item->data;
		} else if (strcmp(fmt, "S") == 0) {
			str = va_arg(ap, const char *);
			sz = strlen(str) + 1;
			cursor->key.data = (void *)str;
		} else {
			buf = &cursor->key;

			va_copy(ap_copy, ap);
			ret = __wt_struct_sizev(
			    session, &sz, cursor->key_format, ap_copy);
			va_end(ap_copy);
			WT_ERR(ret);

			WT_ERR(__wt_buf_initsize(session, buf, sz));
			WT_ERR(__wt_struct_packv(
			    session, buf->mem, sz, cursor->key_format, ap));
		}
	}
	if (sz == 0)
		WT_ERR_MSG(session, EINVAL, "Empty keys not permitted");
	else if ((uint32_t)sz != sz)
		WT_ERR_MSG(session, EINVAL,
		    "Key size (%" PRIu64 ") out of range", (uint64_t)sz);
	cursor->saved_err = 0;
	cursor->key.size = sz;
	F_SET(cursor, WT_CURSTD_KEY_EXT);
	if (0) {
err:		cursor->saved_err = ret;
	}
}

/*
 * __wt_kv_get_value --
 *	WT_CURSOR->get_value default implementation.
 */
int
__wt_kv_get_value(WT_SESSION_IMPL *session,
    WT_ITEM *wtvalue, const char *fmt, va_list ap)
{
	WT_DECL_RET;
	WT_ITEM *value;

	/* Fast path some common cases: single strings, byte arrays and bits. */
	if (strcmp(fmt, "S") == 0)
		*va_arg(ap, const char **) = wtvalue->data;
	else if (strcmp(fmt, "u") == 0) {
		value = va_arg(ap, WT_ITEM *);
		value->data = wtvalue->data;
		value->size = wtvalue->size;
	} else if (strcmp(fmt, "t") == 0 ||
	    (isdigit(fmt[0]) && strcmp(fmt + 1, "t") == 0))
		*va_arg(ap, uint8_t *) = *(uint8_t *)wtvalue->data;
	else
		ret = __wt_struct_unpackv(session,
		    wtvalue->data, wtvalue->size, fmt, ap);

	return (ret);
}

/*
 * __wt_kv_set_value --
 *	WT_CURSOR->set_value default implementation.
 */
void
__wt_kv_set_value(WT_CURSOR *cursor, va_list ap)
{
	WT_DECL_RET;
	WT_ITEM *buf, *item;
	WT_SESSION_IMPL *session;
	const char *fmt, *str;
	size_t sz;
	va_list apcopy;

	session = (WT_SESSION_IMPL *)cursor->session;
	F_CLR(cursor, WT_CURSTD_VALUE_SET);

	fmt = F_ISSET(cursor, WT_CURSOR_RAW_OK) ? "u" : cursor->value_format;

	/* Fast path some common cases: single strings, byte arrays and bits. */
	if (strcmp(fmt, "S") == 0) {
		str = va_arg(ap, const char *);
		sz = strlen(str) + 1;
		cursor->value.data = str;
	} else if (F_ISSET(cursor, WT_CURSOR_RAW_OK) || strcmp(fmt, "u") == 0) {
		item = va_arg(ap, WT_ITEM *);
		sz = item->size;
		cursor->value.data = item->data;
	} else if (strcmp(fmt, "t") == 0 ||
	    (isdigit(fmt[0]) && strcmp(fmt + 1, "t") == 0)) {
		sz = 1;
		buf = &cursor->value;
		WT_ERR(__wt_buf_initsize(session, buf, sz));
		*(uint8_t *)buf->mem = (uint8_t)va_arg(ap, int);
	} else {
		va_copy(apcopy, ap);
		WT_ERR(
		    __wt_struct_sizev(session, &sz, cursor->value_format, ap));
		buf = &cursor->value;
		WT_ERR(__wt_buf_initsize(session, buf, sz));
		WT_ERR(__wt_struct_packv(session, buf->mem, sz,
		    cursor->value_format, apcopy));
		va_end(apcopy);
	}
	F_SET(cursor, WT_CURSTD_VALUE_EXT);
	cursor->value.size = sz;

	if (0) {
err:		cursor->saved_err = ret;
	}
}
