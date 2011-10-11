/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_cursor_get_key --
 *	WT_CURSOR->get_key default implementation.
 */
int
__wt_cursor_get_key(WT_CURSOR *cursor, ...)
{
	va_list ap;
	int ret;

	va_start(ap, cursor);
	ret = __wt_cursor_get_keyv(cursor, ap);
	va_end(ap);
	return (ret);
}

/*
 * __wt_cursor_get_keyv --
 *	WT_CURSOR->get_key worker function.
 */
int
__wt_cursor_get_keyv(WT_CURSOR *cursor, va_list ap)
{
	WT_ITEM *key;
	WT_SESSION_IMPL *session;
	const char *fmt;
	int ret;

	CURSOR_API_CALL(cursor, session, get_key, NULL);
	WT_CURSOR_NEEDKEY(cursor);

	if (WT_CURSOR_RECNO(cursor)) {
		if (F_ISSET(cursor, WT_CURSTD_RAW)) {
			key = va_arg(ap, WT_ITEM *);
			key->data = cursor->raw_recno_buf;
			key->size = (uint32_t)
			    __wt_struct_size(session, "q", cursor->recno);
			ret = __wt_struct_pack(session, cursor->raw_recno_buf,
			    sizeof(cursor->raw_recno_buf), "q", cursor->recno);
		} else
			*va_arg(ap, uint64_t *) = cursor->recno;
	} else {
		fmt = cursor->key_format;
		if (F_ISSET(cursor,
		    WT_CURSTD_DUMP_HEX | WT_CURSTD_DUMP_PRINT | WT_CURSTD_RAW))
			fmt = "u";
		ret = __wt_struct_unpackv(
		    session, cursor->key.data, cursor->key.size, fmt, ap);
	}

err:	API_END(session);
	return (ret);
}

/*
 * __wt_cursor_get_value --
 *	WT_CURSOR->get_value default implementation.
 */
int
__wt_cursor_get_value(WT_CURSOR *cursor, ...)
{
	WT_SESSION_IMPL *session;
	const char *fmt;
	va_list ap;
	int ret;

	CURSOR_API_CALL(cursor, session, get_value, NULL);
	WT_CURSOR_NEEDVALUE(cursor);

	va_start(ap, cursor);
	fmt = F_ISSET(cursor,
	    WT_CURSTD_DUMP_HEX | WT_CURSTD_DUMP_PRINT | WT_CURSTD_RAW) ?
	    "u" : cursor->value_format;
	ret = __wt_struct_unpackv(session,
	    cursor->value.data, cursor->value.size, fmt, ap);
	va_end(ap);

err:	API_END(session);
	return (ret);
}

/*
 * __cursor_set_key --
 *	WT_CURSOR->set_key default implementation.
 */
static void
__cursor_set_key(WT_CURSOR *cursor, ...)
{
	WT_SESSION_IMPL *session;
	WT_BUF *buf;
	WT_ITEM *item;
	const char *fmt, *str;
	size_t sz;
	va_list ap;
	int ret;

	CURSOR_API_CALL(cursor, session, set_key, NULL);

	va_start(ap, cursor);
	/* Fast path some common cases: single strings or byte arrays. */
	if (WT_CURSOR_RECNO(cursor)) {
		if (F_ISSET(cursor, WT_CURSTD_RAW)) {
			item = va_arg(ap, WT_ITEM *);
			WT_ERR(__wt_struct_unpack(session,
			    item->data, item->size, "q", &cursor->recno));
		} else
			cursor->recno = va_arg(ap, uint64_t);
		if (cursor->recno == 0) {
			__wt_errx(session,
			    "Record numbers must be greater than zero");
			ret = EINVAL;
			goto err;
		}
		cursor->key.data = &cursor->recno;
		sz = sizeof(cursor->recno);
	} else {
		if (F_ISSET(cursor, WT_CURSTD_RAW))
			fmt = "u";
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
			sz = __wt_struct_sizev(session, cursor->key_format, ap);
			va_end(ap);
			va_start(ap, cursor);
			WT_ERR(__wt_buf_initsize(session, buf, sz));
			WT_ERR(__wt_struct_packv(session, buf->mem, sz,
			    cursor->key_format, ap));
		}
	}
	if (sz == 0) {
		__wt_errx(session, "Empty keys not permitted");
		ret = EINVAL;
		goto err;
	} else if ((uint32_t)sz != sz) {
		__wt_errx(session, "Key size (%" PRIu64 ") out of range",
		    (uint64_t)sz);
		ret = EINVAL;
		goto err;
	}
	cursor->saved_err = 0;
	cursor->key.size = WT_STORE_SIZE(sz);
	F_SET(cursor, WT_CURSTD_KEY_SET);
	if (0) {
err:		cursor->saved_err = ret;
		F_CLR(cursor, WT_CURSTD_KEY_SET);
	}

	va_end(ap);
	API_END(session);
}

/*
 * __cursor_set_value --
 *	WT_CURSOR->set_value default implementation.
 */
static void
__cursor_set_value(WT_CURSOR *cursor, ...)
{
	WT_SESSION_IMPL *session;
	WT_BUF *buf;
	WT_ITEM *item;
	const char *fmt, *str;
	size_t sz;
	va_list ap;
	int ret;

	CURSOR_API_CALL(cursor, session, set_value, NULL);

	va_start(ap, cursor);
	fmt = F_ISSET(cursor, WT_CURSTD_RAW) ? "u" : cursor->value_format;
	/* Fast path some common cases: single strings or byte arrays. */
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
		sz = __wt_struct_sizev(session, cursor->value_format, ap);
		va_end(ap);
		va_start(ap, cursor);
		if ((ret = __wt_buf_initsize(session, buf, sz)) != 0 ||
		    (ret = __wt_struct_packv(session, buf->mem, sz,
		    cursor->value_format, ap)) != 0) {
			cursor->saved_err = ret;
			F_CLR(cursor, WT_CURSTD_VALUE_SET);
			goto err;
		}
		cursor->value.data = buf->mem;
	}
	F_SET(cursor, WT_CURSTD_VALUE_SET);
	cursor->value.size = WT_STORE_SIZE(sz);
	va_end(ap);

err:	API_END(session);
}

/*
 * __cursor_search --
 *	WT_CURSOR->search default implementation.
 */
static int
__cursor_search(WT_CURSOR *cursor)
{
	int exact;

	WT_RET(cursor->search_near(cursor, &exact));
	return ((exact == 0) ? 0 : WT_NOTFOUND);
}

/*
 * __wt_cursor_close --
 *	WT_CURSOR->close default implementation.
 */
int
__wt_cursor_close(WT_CURSOR *cursor, const char *config)
{
	WT_SESSION_IMPL *session;
	int ret;

	CURSOR_API_CALL_CONF(cursor, session, close, NULL, config, cfg);
	WT_UNUSED(cfg);

	__wt_buf_free(session, &cursor->key);
	__wt_buf_free(session, &cursor->value);

	if (F_ISSET(cursor, WT_CURSTD_PUBLIC))
		TAILQ_REMOVE(&session->cursors, cursor, q);
	__wt_free(session, cursor);

err:	API_END(session);
	return (ret);
}

/*
 * __wt_cursor_init --
 *	Default cursor initialization.
 *
 *	Most cursors are "public", and added to the list in the session
 *	to be closed when the cursor is closed.  However, some cursors are
 *	opened for internal use, or are opened inside another cursor (such
 *	as column groups or indices within a table cursor), and adding those
 *	cursors to the list introduces ordering dependencies into
 *	WT_SESSION->close that we prefer to avoid.
 */
void
__wt_cursor_init(WT_CURSOR *cursor, int is_public, const char *cfg[])
{
	WT_SESSION_IMPL *session;

	WT_UNUSED(cfg);
	session = (WT_SESSION_IMPL *)cursor->session;

	if (cursor->get_key == NULL)
		cursor->get_key = __wt_cursor_get_key;
	if (cursor->get_value == NULL)
		cursor->get_value = __wt_cursor_get_value;
	if (cursor->set_key == NULL)
		cursor->set_key = __cursor_set_key;
	if (cursor->set_value == NULL)
		cursor->set_value = __cursor_set_value;
	if (cursor->search == NULL)
		cursor->search = __cursor_search;

	WT_CLEAR(cursor->key);
	WT_CLEAR(cursor->value);

	if (is_public) {
		F_SET(cursor, WT_CURSTD_PUBLIC);
		TAILQ_INSERT_HEAD(&session->cursors, cursor, q);
	}
}

/*
 * __wt_cursor_notsup --
 *	WT_CURSOR->XXX method for unsupported cursor actions.
 */
int
__wt_cursor_notsup(WT_CURSOR *cursor)
{
	WT_UNUSED(cursor);
	return (ENOTSUP);
}

/*
 * __wt_cursor_kv_not_set --
 *	Standard error message for key/values not set.
 */
int
__wt_cursor_kv_not_set(WT_CURSOR *cursor, int key)
{
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)cursor->session;
	if (cursor->saved_err != 0)
		return (cursor->saved_err);

	__wt_errx(session, "requires %s be set", key ? "key" : "value");
	return (EINVAL);
}
