/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __curbtree_first --
 *	WT_CURSOR->first method for the btree cursor type.
 */
static int
__curbtree_first(WT_CURSOR *cursor)
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
 * __curbtree_last --
 *	WT_CURSOR->last method for the btree cursor type.
 */
static int
__curbtree_last(WT_CURSOR *cursor)
{
	WT_CURSOR_BTREE *cbt;
	WT_SESSION_IMPL *session;

	cbt = (WT_CURSOR_BTREE *)cursor;
	CURSOR_API_CALL(cursor, session, last, cbt->btree);
	API_END(session);

	return (ENOTSUP);
}

/*
 * __curbtree_next --
 *	WT_CURSOR->next method for the btree cursor type.
 */
static int
__curbtree_next(WT_CURSOR *cursor)
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
 * __curbtree_prev --
 *	WT_CURSOR->prev method for the btree cursor type.
 */
static int
__curbtree_prev(WT_CURSOR *cursor)
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
 * __curbtree_search_near --
 *	WT_CURSOR->search_near method for the btree cursor type.
 */
static int
__curbtree_search_near(WT_CURSOR *cursor, int *lastcmp)
{
	WT_CURSOR_BTREE *cbt;
	WT_SESSION_IMPL *session;
	int ret;

	cbt = (WT_CURSOR_BTREE *)cursor;
	CURSOR_API_CALL(cursor, session, search_near, cbt->btree);
	ret = __wt_btcur_search_near((WT_CURSOR_BTREE *)cursor, lastcmp);
	API_END(session);

	return (ret);
}

/*
 * __curbtree_insert --
 *	WT_CURSOR->insert method for the btree cursor type.
 */
static int
__curbtree_insert(WT_CURSOR *cursor)
{
	WT_CURSOR_BTREE *cbt;
	WT_SESSION_IMPL *session;
	int ret;

	cbt = (WT_CURSOR_BTREE *)cursor;
	CURSOR_API_CALL(cursor, session, insert, cbt->btree);
	ret = __wt_btcur_insert((WT_CURSOR_BTREE *)cursor);
	API_END(session);

	return (ret);
}

/*
 * __curbtree_update --
 *	WT_CURSOR->update method for the btree cursor type.
 */
static int
__curbtree_update(WT_CURSOR *cursor)
{
	WT_CURSOR_BTREE *cbt;
	WT_SESSION_IMPL *session;
	int ret;

	cbt = (WT_CURSOR_BTREE *)cursor;
	CURSOR_API_CALL(cursor, session, update, cbt->btree);
	ret = __wt_btcur_update((WT_CURSOR_BTREE *)cursor);
	API_END(session);

	return (ret);
}

/*
 * __curbtree_remove --
 *	WT_CURSOR->remove method for the btree cursor type.
 */
static int
__curbtree_remove(WT_CURSOR *cursor)
{
	WT_CURSOR_BTREE *cbt;
	WT_SESSION_IMPL *session;
	int ret;

	cbt = (WT_CURSOR_BTREE *)cursor;
	CURSOR_API_CALL(cursor, session, remove, cbt->btree);
	ret = __wt_btcur_remove((WT_CURSOR_BTREE *)cursor);
	API_END(session);

	return (ret);
}

/*
 * __curbtree_close --
 *	WT_CURSOR->close method for the btree cursor type.
 */
static int
__curbtree_close(WT_CURSOR *cursor, const char *config)
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
 * __wt_curbtree_create --
 *	Open a cursor for a given btree handle.
 */
int
__wt_curbtree_create(WT_SESSION_IMPL *session,
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
		__curbtree_first,
		__curbtree_last,
		__curbtree_next,
		__curbtree_prev,
		NULL,
		__curbtree_search_near,
		__curbtree_insert,
		__curbtree_update,
		__curbtree_remove,
		__curbtree_close,
		{ NULL, NULL },		/* TAILQ_ENTRY q */
		0,			/* recno key */
		{ NULL, 0, 0, NULL, 0 },/* WT_BUF key */
		{ NULL, 0, 0, NULL, 0 },/* WT_BUF value */
		0,			/* int saved_err */
		0			/* uint32_t flags */
	};
	WT_BTREE *btree;
	WT_CURSOR_BTREE *cbt;
	WT_CONFIG_ITEM cval;
	WT_CURSOR *cursor;
	int bulk, dump, printable, raw, ret;
	size_t csize;
	const char *cfg[] = API_CONF_DEFAULTS(session, open_cursor, config);

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

	STATIC_ASSERT(offsetof(WT_CURSOR_BTREE, iface) == 0);
	__wt_cursor_init(cursor, is_public, config);
	*cursorp = cursor;

	if (0) {
err:		__wt_free(session, cbt);
	}

	return (ret);
}

/*
 * __wt_curbtree_open --
 *	WT_SESSION->open_cursor method for the btree cursor type.
 */
int
__wt_curbtree_open(WT_SESSION_IMPL *session,
    const char *uri, const char *config, WT_CURSOR **cursorp)
{
	WT_BTREE_SESSION *btree_session;
	const char *name, *treeconf;
	int ret;

	/* TODO: handle projections. */
	name = uri + strlen("table:");

	if ((ret = __wt_session_get_btree(session,
	    name, strlen(name), &btree_session)) == 0)
		session->btree = btree_session->btree;
	else if (ret == WT_NOTFOUND) {
		WT_RET(__wt_btconf_read(session, name, &treeconf));
		WT_RET(__wt_btree_open(session, name, treeconf, 0));
		WT_RET(__wt_session_add_btree(session, &btree_session));
	} else
		return (ret);

	return (__wt_curbtree_create(session, 1, config, cursorp));
}
