/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __curstat_first --
 *	WT_CURSOR->first method for the btree cursor type.
 */
static int
__curstat_first(WT_CURSOR *cursor)
{
	SESSION *session;
	int ret;

	CURSOR_API_CALL(cursor, session, first);
	ret = WT_NOTFOUND;
	API_END();

	return (ret);
}

/*
 * __curstat_last --
 *	WT_CURSOR->last method for the btree cursor type.
 */
static int
__curstat_last(WT_CURSOR *cursor)
{
	SESSION *session;
	int ret;

	CURSOR_API_CALL(cursor, session, last);
	ret = ENOTSUP;
	API_END();

	return (ret);
}

/*
 * __curstat_next --
 *	WT_CURSOR->next method for the btree cursor type.
 */
static int
__curstat_next(WT_CURSOR *cursor)
{
	SESSION *session;
	int ret;

	CURSOR_API_CALL(cursor, session, next);
	ret = WT_NOTFOUND;
	API_END();

	return (ret);
}

/*
 * __curstat_prev --
 *	WT_CURSOR->prev method for the btree cursor type.
 */
static int
__curstat_prev(WT_CURSOR *cursor)
{
	SESSION *session;
	int ret;

	CURSOR_API_CALL(cursor, session, prev);
	ret = ENOTSUP;
	API_END();

	return (ret);
}

/*
 * __curstat_search_near --
 *	WT_CURSOR->search_near method for the btree cursor type.
 */
static int
__curstat_search_near(WT_CURSOR *cursor, int *lastcmp)
{
	SESSION *session;
	int ret;

	WT_UNUSED(lastcmp);

	CURSOR_API_CALL(cursor, session, search_near);
	ret = ENOTSUP;
	API_END();

	return (ret);
}

/*
 * __curstat_insert --
 *	WT_CURSOR->insert method for the btree cursor type.
 */
static int
__curstat_insert(WT_CURSOR *cursor)
{
	SESSION *session;
	int ret;

	CURSOR_API_CALL(cursor, session, insert);
	ret = ENOTSUP;
	API_END();

	return (ret);
}

/*
 * __curstat_update --
 *	WT_CURSOR->update method for the btree cursor type.
 */
static int
__curstat_update(WT_CURSOR *cursor)
{
	SESSION *session;
	int ret;

	CURSOR_API_CALL(cursor, session, update);
	ret = ENOTSUP;
	API_END();

	return (ret);
}

/*
 * __curstat_remove --
 *	WT_CURSOR->remove method for the btree cursor type.
 */
static int
__curstat_remove(WT_CURSOR *cursor)
{
	SESSION *session;
	int ret;

	CURSOR_API_CALL(cursor, session, insert);
	ret = ENOTSUP;
	API_END();

	return (ret);
}

/*
 * __curstat_close --
 *	WT_CURSOR->close method for the btree cursor type.
 */
static int
__curstat_close(WT_CURSOR *cursor, const char *config)
{
	SESSION *session;
	int ret;

	CURSOR_API_CALL(cursor, session, close);
	ret = 0;
	WT_TRET(__wt_cursor_close(cursor, config));
	API_END();

	return (ret);
}

/*
 * __wt_cursor_open --
 *	WT_SESSION->open_cursor method for the btree cursor type.
 */
int
__wt_curstat_open(SESSION *session,
    const char *uri, const char *config, WT_CURSOR **cursorp)
{
	static WT_CURSOR iface = {
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		__curstat_first,
		__curstat_last,
		__curstat_next,
		__curstat_prev,
		NULL,
		__curstat_search_near,
		__curstat_insert,
		__curstat_update,
		__curstat_remove,
		__curstat_close,
		{ NULL, NULL },		/* TAILQ_ENTRY q */
		0,			/* recno key */
		{ NULL, 0, 0, NULL, 0 },/* WT_BUF key */
		{ NULL, 0, 0, NULL, 0 },/* WT_BUF value */
		0,			/* int saved_err */
		0			/* uint32_t flags */
	};
	CURSOR_STAT *cst;
	WT_CONFIG_ITEM cval;
	WT_CURSOR *cursor;
	const char *key_format, *value_format;
	int raw, ret;
	uint32_t dump;
	size_t csize;
	API_CONF_INIT(session, open_cursor, config);

	/* Skip "stat:". */
	uri += 5;

	dump = 0;
	WT_ERR(__wt_config_gets(__cfg, "dump", &cval));
	if ((cval.type == ITEM_STRING || cval.type == ITEM_ID) &&
	    cval.len > 0) {
		if (strncasecmp("printable", cval.str, cval.len) == 0)
			dump = WT_DUMP_PRINT;
		else if (strncasecmp("raw", cval.str, cval.len) == 0)
			dump = WT_DUMP_RAW;
	}
	WT_ERR(__wt_config_gets(__cfg, "raw", &cval));
	raw = (cval.val != 0);

	WT_RET(__wt_calloc(session, 1, csize, &cst));

	cursor = &cst->iface;
	*cursor = iface;
	cursor->session = &session->iface;
	cursor->key_format = key_format;
	cursor->value_format = value_format;
	__wt_cursor_init(cursor, config);

	if (dump)
		__wt_curdump_init(cursor, dump);
	if (raw)
		F_SET(cursor, WT_CURSTD_RAW);

	STATIC_ASSERT(offsetof(CURSOR_STAT, iface) == 0);
	TAILQ_INSERT_HEAD(&session->cursors, cursor, q);
	*cursorp = cursor;

	if (0) {
err:		__wt_free(session, cst);
	}

	return (ret);
}
