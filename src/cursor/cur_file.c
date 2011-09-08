/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __curfile_first --
 *	WT_CURSOR->first method for the btree cursor type.
 */
static int
__curfile_first(WT_CURSOR *cursor)
{
	WT_CURSOR_BTREE *cbt;
	WT_SESSION_IMPL *session;
	int ret;

	cbt = (WT_CURSOR_BTREE *)cursor;
	CURSOR_API_CALL(cursor, session, first, cbt->btree);
	ret = __wt_btcur_first(cbt);
	API_END(session);

	return (ret);
}

/*
 * __curfile_last --
 *	WT_CURSOR->last method for the btree cursor type.
 */
static int
__curfile_last(WT_CURSOR *cursor)
{
	WT_CURSOR_BTREE *cbt;
	WT_SESSION_IMPL *session;
	int ret;

	cbt = (WT_CURSOR_BTREE *)cursor;
	CURSOR_API_CALL(cursor, session, last, cbt->btree);
	ret = __wt_btcur_last(cbt);
	API_END(session);

	return (ret);
}

/*
 * __curfile_next --
 *	WT_CURSOR->next method for the btree cursor type.
 */
static int
__curfile_next(WT_CURSOR *cursor)
{
	WT_CURSOR_BTREE *cbt;
	WT_SESSION_IMPL *session;
	int ret;

	cbt = (WT_CURSOR_BTREE *)cursor;
	CURSOR_API_CALL(cursor, session, next, cbt->btree);
	ret = __wt_btcur_next((WT_CURSOR_BTREE *)cursor);
	API_END(session);

	return (ret);
}

/*
 * __curfile_prev --
 *	WT_CURSOR->prev method for the btree cursor type.
 */
static int
__curfile_prev(WT_CURSOR *cursor)
{
	WT_CURSOR_BTREE *cbt;
	WT_SESSION_IMPL *session;
	int ret;

	cbt = (WT_CURSOR_BTREE *)cursor;
	CURSOR_API_CALL(cursor, session, prev, cbt->btree);
	ret = __wt_btcur_prev((WT_CURSOR_BTREE *)cursor);
	API_END(session);

	return (ret);
}

/*
 * __curfile_search --
 *	WT_CURSOR->search method for the btree cursor type.
 */
static int
__curfile_search(WT_CURSOR *cursor)
{
	WT_CURSOR_BTREE *cbt;
	WT_SESSION_IMPL *session;
	int ret;

	cbt = (WT_CURSOR_BTREE *)cursor;
	CURSOR_API_CALL(cursor, session, search, cbt->btree);
	WT_CURSOR_NEEDKEY(cursor);
	ret = __wt_btcur_search(cbt);
err:	API_END(session);

	return (ret);
}

/*
 * __curfile_search_near --
 *	WT_CURSOR->search_near method for the btree cursor type.
 */
static int
__curfile_search_near(WT_CURSOR *cursor, int *exact)
{
	WT_CURSOR_BTREE *cbt;
	WT_SESSION_IMPL *session;
	int ret;

	cbt = (WT_CURSOR_BTREE *)cursor;
	CURSOR_API_CALL(cursor, session, search_near, cbt->btree);
	WT_CURSOR_NEEDKEY(cursor);
	ret = __wt_btcur_search_near(cbt, exact);
err:	API_END(session);

	return (ret);
}

/*
 * __curfile_insert --
 *	WT_CURSOR->insert method for the btree cursor type.
 */
static int
__curfile_insert(WT_CURSOR *cursor)
{
	WT_CURSOR_BTREE *cbt;
	WT_SESSION_IMPL *session;
	int ret;

	cbt = (WT_CURSOR_BTREE *)cursor;
	CURSOR_API_CALL(cursor, session, insert, cbt->btree);
	if (cbt->btree->type == BTREE_ROW)
		WT_CURSOR_NEEDKEY(cursor);
	WT_CURSOR_NEEDVALUE(cursor);
	ret = __wt_btcur_insert((WT_CURSOR_BTREE *)cursor);
err:	API_END(session);

	return (ret);
}

/*
 * __curfile_update --
 *	WT_CURSOR->update method for the btree cursor type.
 */
static int
__curfile_update(WT_CURSOR *cursor)
{
	WT_CURSOR_BTREE *cbt;
	WT_SESSION_IMPL *session;
	int ret;

	cbt = (WT_CURSOR_BTREE *)cursor;
	CURSOR_API_CALL(cursor, session, update, cbt->btree);
	WT_CURSOR_NEEDKEY(cursor);
	WT_CURSOR_NEEDVALUE(cursor);
	ret = __wt_btcur_update((WT_CURSOR_BTREE *)cursor);
err:	API_END(session);

	return (ret);
}

/*
 * __curfile_remove --
 *	WT_CURSOR->remove method for the btree cursor type.
 */
static int
__curfile_remove(WT_CURSOR *cursor)
{
	WT_CURSOR_BTREE *cbt;
	WT_SESSION_IMPL *session;
	int ret;

	cbt = (WT_CURSOR_BTREE *)cursor;
	CURSOR_API_CALL(cursor, session, remove, cbt->btree);
	WT_CURSOR_NEEDKEY(cursor);
	ret = __wt_btcur_remove((WT_CURSOR_BTREE *)cursor);
err:	API_END(session);

	return (ret);
}

/*
 * __curfile_close --
 *	WT_CURSOR->close method for the btree cursor type.
 */
static int
__curfile_close(WT_CURSOR *cursor, const char *config)
{
	WT_CURSOR_BTREE *cbt;
	WT_SESSION_IMPL *session;
	int ret;

	cbt = (WT_CURSOR_BTREE *)cursor;
	CURSOR_API_CALL_CONF(cursor, session, close, cbt->btree, config, cfg);
	WT_UNUSED(cfg);
	ret = 0;
	WT_TRET(__wt_btcur_close((WT_CURSOR_BTREE *)cursor, config));
	WT_TRET(__wt_cursor_close(cursor, config));
	API_END(session);

	return (ret);
}

/*
 * __wt_curfile_create --
 *	Open a cursor for a given btree handle.
 */
int
__wt_curfile_create(WT_SESSION_IMPL *session,
    int is_public, const char *config, WT_CURSOR **cursorp)
{
	static WT_CURSOR iface = {
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		__curfile_first,
		__curfile_last,
		__curfile_next,
		__curfile_prev,
		__curfile_search,
		__curfile_search_near,
		__curfile_insert,
		__curfile_update,
		__curfile_remove,
		__curfile_close,
		{ NULL, NULL },		/* TAILQ_ENTRY q */
		0,			/* recno key */
		{ NULL, 0, 0, NULL, 0 },/* WT_BUF key */
		{ NULL, 0, 0, NULL, 0 },/* WT_BUF value */
		0,			/* int saved_err */
		0			/* uint32_t flags */
	};
	WT_BTREE *btree;
	WT_CONFIG_ITEM cval;
	WT_CURSOR *cursor;
	WT_CURSOR_BTREE *cbt;
	size_t csize;
	int bulk, dump, printable, raw, ret;
	const char *cfg[] = API_CONF_DEFAULTS(session, open_cursor, config);

	cbt = NULL;
	ret = 0;

	btree = session->btree;
	WT_ASSERT(session, btree != NULL);

	WT_ERR(__wt_config_gets(session, cfg, "bulk", &cval));
	bulk = (cval.val != 0);
	WT_ERR(__wt_config_gets(session, cfg, "dump", &cval));
	dump = (cval.val != 0);
	WT_ERR(__wt_config_gets(session, cfg, "printable", &cval));
	printable = (cval.val != 0);
	WT_ERR(__wt_config_gets(session, cfg, "raw", &cval));
	raw = (cval.val != 0);

	csize = bulk ? sizeof(WT_CURSOR_BULK) : sizeof(WT_CURSOR_BTREE);
	WT_RET(__wt_calloc(session, 1, csize, &cbt));

	cursor = &cbt->iface;
	*cursor = iface;
	cursor->session = &session->iface;
	cursor->key_format = btree->key_format;
	cursor->value_format = btree->value_format;

	cbt->btree = session->btree;
	if (bulk)
		WT_ERR(__wt_curbulk_init((WT_CURSOR_BULK *)cbt));
	if (dump)
		__wt_curdump_init(cursor, printable);
	if (raw)
		F_SET(cursor, WT_CURSTD_RAW);

	WT_ERR(__wt_config_gets(session, cfg, "overwrite", &cval));
	if (cval.val != 0)
		F_SET(cursor, WT_CURSTD_OVERWRITE);

	STATIC_ASSERT(offsetof(WT_CURSOR_BTREE, iface) == 0);
	__wt_cursor_init(cursor, is_public, config);
	*cursorp = cursor;

	if (0) {
err:		__wt_free(session, cbt);
	}

	return (ret);
}

/*
 * __wt_curfile_open --
 *	WT_SESSION->open_cursor method for the btree cursor type.
 */
int
__wt_curfile_open(WT_SESSION_IMPL *session,
    const char *name, const char *config, WT_CURSOR **cursorp)
{
	const char *filename;

	/* TODO: handle projections. */

	filename = name;
	if (WT_PREFIX_MATCH(name, "colgroup:"))
		WT_RET(__wt_schema_get_btree(session, name, strlen(name)));
	else if (WT_PREFIX_SKIP(filename, "file:"))
		WT_RET(__wt_session_get_btree(session, name, filename, NULL));
	else
		return (EINVAL);

	return (__wt_curfile_create(session, 1, config, cursorp));
}
