/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __raw_to_dump --
 *	We have a buffer where the data item contains a raw value,
 *	convert it to a printable string.
 */
static int
__raw_to_dump(
    WT_SESSION_IMPL *session, WT_ITEM *from, WT_ITEM *to, bool hexonly)
{
	if (hexonly)
		WT_RET(__wt_raw_to_hex(session, from->data, from->size, to));
	else
		WT_RET(
		    __wt_raw_to_esc_hex(session, from->data, from->size, to));

	return (0);
}

/*
 * __dump_to_raw --
 *	We have a buffer containing a dump string,
 *	convert it to a raw value.
 */
static int
__dump_to_raw(
    WT_SESSION_IMPL *session, const char *src_arg, WT_ITEM *item, bool hexonly)
{
	if (hexonly)
		WT_RET(__wt_hex_to_raw(session, src_arg, item));
	else
		WT_RET(__wt_esc_hex_to_raw(session, src_arg, item));

	return (0);
}

/*
 * __curdump_get_key --
 *	WT_CURSOR->get_key for dump cursors.
 */
static int
__curdump_get_key(WT_CURSOR *cursor, ...)
{
	WT_CURSOR *child;
	WT_CURSOR_DUMP *cdump;
	WT_CURSOR_JSON *json;
	WT_DECL_RET;
	WT_ITEM item, *itemp;
	WT_SESSION_IMPL *session;
	size_t size;
	uint64_t recno;
	const char *fmt;
	const void *buffer;
	va_list ap;

	cdump = (WT_CURSOR_DUMP *)cursor;
	child = cdump->child;

	va_start(ap, cursor);
	CURSOR_API_CALL(cursor, session, get_key, NULL);

	if (F_ISSET(cursor, WT_CURSTD_DUMP_JSON)) {
		json = (WT_CURSOR_JSON *)cursor->json_private;
		WT_ASSERT(session, json != NULL);
		if (WT_CURSOR_RECNO(cursor)) {
			WT_ERR(child->get_key(child, &recno));
			buffer = &recno;
			size = sizeof(recno);
			fmt = "R";
		} else {
			WT_ERR(__wt_cursor_get_raw_key(child, &item));
			buffer = item.data;
			size = item.size;
			if (F_ISSET(cursor, WT_CURSTD_RAW))
				fmt = "u";
			else
				fmt = cursor->key_format;
		}
		ret = __wt_json_alloc_unpack(
		    session, buffer, size, fmt, json, true, ap);
	} else {
		if (WT_CURSOR_RECNO(cursor) &&
		    !F_ISSET(cursor, WT_CURSTD_RAW)) {
			WT_ERR(child->get_key(child, &recno));

			WT_ERR(__wt_buf_fmt(session, &cursor->key, "%"
			    PRIu64, recno));
		} else {
			WT_ERR(child->get_key(child, &item));

			WT_ERR(__raw_to_dump(session, &item, &cursor->key,
			    F_ISSET(cursor, WT_CURSTD_DUMP_HEX)));
		}

		if (F_ISSET(cursor, WT_CURSTD_RAW)) {
			itemp = va_arg(ap, WT_ITEM *);
			itemp->data = cursor->key.data;
			itemp->size = cursor->key.size;
		} else
			*va_arg(ap, const char **) = cursor->key.data;
	}

err:	va_end(ap);
	API_END_RET(session, ret);
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
	recno = __wt_strtouq(p, &endptr, 0);
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
	WT_CURSOR_DUMP *cdump;
	WT_CURSOR *child;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	uint64_t recno;
	va_list ap;
	const char *p;

	cdump = (WT_CURSOR_DUMP *)cursor;
	child = cdump->child;
	CURSOR_API_CALL(cursor, session, set_key, NULL);

	va_start(ap, cursor);
	if (F_ISSET(cursor, WT_CURSTD_RAW))
		p = va_arg(ap, WT_ITEM *)->data;
	else
		p = va_arg(ap, const char *);
	va_end(ap);

	if (WT_CURSOR_RECNO(cursor) && !F_ISSET(cursor, WT_CURSTD_RAW)) {
		WT_ERR(str2recno(session, p, &recno));

		child->set_key(child, recno);
	} else {
		if (F_ISSET(cursor, WT_CURSTD_DUMP_JSON))
			WT_ERR(__wt_json_to_item(session, p, cursor->key_format,
			    (WT_CURSOR_JSON *)cursor->json_private, true,
			    &cursor->key));
		else
			WT_ERR(__dump_to_raw(session, p, &cursor->key,
			    F_ISSET(cursor, WT_CURSTD_DUMP_HEX)));

		child->set_key(child, &cursor->key);
	}

	if (0) {
err:		cursor->saved_err = ret;
		F_CLR(cursor, WT_CURSTD_KEY_SET);
	}
	API_END(session, ret);
}

/*
 * __curdump_get_value --
 *	WT_CURSOR->get_value for dump cursors.
 */
static int
__curdump_get_value(WT_CURSOR *cursor, ...)
{
	WT_CURSOR_DUMP *cdump;
	WT_CURSOR_JSON *json;
	WT_CURSOR *child;
	WT_DECL_RET;
	WT_ITEM item, *itemp;
	WT_SESSION_IMPL *session;
	va_list ap;
	const char *fmt;

	cdump = (WT_CURSOR_DUMP *)cursor;
	child = cdump->child;

	va_start(ap, cursor);
	CURSOR_API_CALL(cursor, session, get_value, NULL);

	if (F_ISSET(cursor, WT_CURSTD_DUMP_JSON)) {
		json = (WT_CURSOR_JSON *)cursor->json_private;
		WT_ASSERT(session, json != NULL);
		WT_ERR(__wt_cursor_get_raw_value(child, &item));
		fmt = F_ISSET(cursor, WT_CURSTD_RAW) ?
		    "u" : cursor->value_format;
		ret = __wt_json_alloc_unpack(
		    session, item.data, item.size, fmt, json, false, ap);
	} else {
		WT_ERR(child->get_value(child, &item));

		WT_ERR(__raw_to_dump(session, &item, &cursor->value,
		    F_ISSET(cursor, WT_CURSTD_DUMP_HEX)));

		if (F_ISSET(cursor, WT_CURSTD_RAW)) {
			itemp = va_arg(ap, WT_ITEM *);
			itemp->data = cursor->value.data;
			itemp->size = cursor->value.size;
		} else
			*va_arg(ap, const char **) = cursor->value.data;
	}

err:	va_end(ap);
	API_END_RET(session, ret);
}

/*
 * __curdump_set_value --
 *	WT_CURSOR->set_value for dump cursors.
 */
static void
__curdump_set_value(WT_CURSOR *cursor, ...)
{
	WT_CURSOR_DUMP *cdump;
	WT_CURSOR *child;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	va_list ap;
	const char *p;

	cdump = (WT_CURSOR_DUMP *)cursor;
	child = cdump->child;
	CURSOR_API_CALL(cursor, session, set_value, NULL);

	va_start(ap, cursor);
	if (F_ISSET(cursor, WT_CURSTD_RAW))
		p = va_arg(ap, WT_ITEM *)->data;
	else
		p = va_arg(ap, const char *);
	va_end(ap);

	if (F_ISSET(cursor, WT_CURSTD_DUMP_JSON))
		WT_ERR(__wt_json_to_item(session, p, cursor->value_format,
		    (WT_CURSOR_JSON *)cursor->json_private, false,
		    &cursor->value));
	else
		WT_ERR(__dump_to_raw(session, p, &cursor->value,
		    F_ISSET(cursor, WT_CURSTD_DUMP_HEX)));

	child->set_value(child, &cursor->value);

	if (0) {
err:		cursor->saved_err = ret;
		F_CLR(cursor, WT_CURSTD_VALUE_SET);
	}
	API_END(session, ret);
}

/* Pass through a call to the underlying cursor. */
#define	WT_CURDUMP_PASS(op)						\
static int								\
__curdump_##op(WT_CURSOR *cursor)					\
{									\
	WT_CURSOR *child;						\
									\
	child = ((WT_CURSOR_DUMP *)cursor)->child;			\
	return (child->op(child));					\
}

WT_CURDUMP_PASS(next)
WT_CURDUMP_PASS(prev)
WT_CURDUMP_PASS(reset)
WT_CURDUMP_PASS(search)

/*
 * __curdump_search_near --
 *	WT_CURSOR::search_near for dump cursors.
 */
static int
__curdump_search_near(WT_CURSOR *cursor, int *exact)
{
	WT_CURSOR_DUMP *cdump;

	cdump = (WT_CURSOR_DUMP *)cursor;
	return (cdump->child->search_near(cdump->child, exact));
}

WT_CURDUMP_PASS(insert)
WT_CURDUMP_PASS(update)
WT_CURDUMP_PASS(remove)

/*
 * __curdump_close --
 *	WT_CURSOR::close for dump cursors.
 */
static int
__curdump_close(WT_CURSOR *cursor)
{
	WT_CURSOR_DUMP *cdump;
	WT_CURSOR *child;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cdump = (WT_CURSOR_DUMP *)cursor;
	child = cdump->child;

	CURSOR_API_CALL(cursor, session, close, NULL);
	if (child != NULL)
		WT_TRET(child->close(child));
	/* We shared the child's URI. */
	cursor->internal_uri = NULL;
	__wt_json_close(session, cursor);
	WT_TRET(__wt_cursor_close(cursor));

err:	API_END_RET(session, ret);
}

/*
 * __wt_curdump_create --
 *	initialize a dump cursor.
 */
int
__wt_curdump_create(WT_CURSOR *child, WT_CURSOR *owner, WT_CURSOR **cursorp)
{
	WT_CURSOR_STATIC_INIT(iface,
	    __curdump_get_key,			/* get-key */
	    __curdump_get_value,		/* get-value */
	    __curdump_set_key,			/* set-key */
	    __curdump_set_value,		/* set-value */
	    __wt_cursor_compare_notsup,		/* compare */
	    __wt_cursor_equals_notsup,		/* equals */
	    __curdump_next,			/* next */
	    __curdump_prev,			/* prev */
	    __curdump_reset,			/* reset */
	    __curdump_search,			/* search */
	    __curdump_search_near,		/* search-near */
	    __curdump_insert,			/* insert */
	    __curdump_update,			/* update */
	    __curdump_remove,			/* remove */
	    __wt_cursor_reconfigure_notsup,	/* reconfigure */
	    __curdump_close);			/* close */
	WT_CURSOR *cursor;
	WT_CURSOR_DUMP *cdump;
	WT_CURSOR_JSON *json;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	const char *cfg[2];

	WT_STATIC_ASSERT(offsetof(WT_CURSOR_DUMP, iface) == 0);

	session = (WT_SESSION_IMPL *)child->session;

	WT_RET(__wt_calloc_one(session, &cdump));
	cursor = &cdump->iface;
	*cursor = iface;
	cursor->session = child->session;
	cursor->internal_uri = child->internal_uri;
	cursor->key_format = child->key_format;
	cursor->value_format = child->value_format;
	cdump->child = child;

	/* Copy the dump flags from the child cursor. */
	F_SET(cursor, F_MASK(child,
	    WT_CURSTD_DUMP_HEX | WT_CURSTD_DUMP_JSON | WT_CURSTD_DUMP_PRINT));
	if (F_ISSET(cursor, WT_CURSTD_DUMP_JSON)) {
		WT_ERR(__wt_calloc_one(session, &json));
		cursor->json_private = child->json_private = json;
	}

	/* __wt_cursor_init is last so we don't have to clean up on error. */
	cfg[0] = WT_CONFIG_BASE(session, WT_SESSION_open_cursor);
	cfg[1] = NULL;
	WT_ERR(__wt_cursor_init(cursor, NULL, owner, cfg, cursorp));

	if (0) {
err:		__wt_free(session, cursor);
	}
	return (ret);
}
