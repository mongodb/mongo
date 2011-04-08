/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

#define	CURSOR_API_CALL(c, s, n)	do {				\
	(s) = (SESSION *)(c)->session;					\
	(s)->cursor = (c);						\
	(s)->btree = ((CURSOR_BTREE *)(c))->btree;			\
	(s)->name = (n);						\
} while (0)

/*
 * __curbtree_first --
 *	WT_CURSOR->first method for the btree cursor type.
 */
static int
__curbtree_first(WT_CURSOR *cursor)
{
	SESSION *session;

	CURSOR_API_CALL(cursor, session, "first");

	return (__wt_btcur_first((CURSOR_BTREE *)cursor));
}

/*
 * __curbtree_last --
 *	WT_CURSOR->last method for the btree cursor type.
 */
static int
__curbtree_last(WT_CURSOR *cursor)
{
	WT_UNUSED(cursor);

	return (ENOTSUP);
}

/*
 * __curbtree_next --
 *	WT_CURSOR->next method for the btree cursor type.
 */
static int
__curbtree_next(WT_CURSOR *cursor)
{
	SESSION *session;

	CURSOR_API_CALL(cursor, session, "next");
	return (__wt_btcur_next((CURSOR_BTREE *)cursor));
}

/*
 * __curbtree_prev --
 *	WT_CURSOR->prev method for the btree cursor type.
 */
static int
__curbtree_prev(WT_CURSOR *cursor)
{
	WT_UNUSED(cursor);

	return (ENOTSUP);
}

/*
 * __curbtree_search_near --
 *	WT_CURSOR->search_near method for the btree cursor type.
 */
static int
__curbtree_search_near(WT_CURSOR *cursor, int *lastcmp)
{
	SESSION *session;

	CURSOR_API_CALL(cursor, session, "search_near");
	WT_RET(__wt_btcur_search_near((CURSOR_BTREE *)cursor, lastcmp));
	return (0);
}

/*
 * __curbtree_insert --
 *	WT_CURSOR->insert method for the btree cursor type.
 */
static int
__curbtree_insert(WT_CURSOR *cursor)
{
	SESSION *session;

	/* Only support exact searches for now */
	CURSOR_API_CALL(cursor, session, "insert");
	return (__wt_btcur_insert((CURSOR_BTREE *)cursor));
}

/*
 * __curbtree_update --
 *	WT_CURSOR->update method for the btree cursor type.
 */
static int
__curbtree_update(WT_CURSOR *cursor)
{
	WT_UNUSED(cursor);

	return (ENOTSUP);
}

/*
 * __curbtree_remove --
 *	WT_CURSOR->remove method for the btree cursor type.
 */
static int
__curbtree_remove(WT_CURSOR *cursor)
{
	WT_UNUSED(cursor);

	return (ENOTSUP);
}

/*
 * __curbtree_close --
 *	WT_CURSOR->close method for the btree cursor type.
 */
static int
__curbtree_close(WT_CURSOR *cursor, const char *config)
{
	int ret;

	ret = 0;
	WT_TRET(__wt_btcur_close((CURSOR_BTREE *)cursor, config));
	WT_TRET(__wt_cursor_close(cursor, config));
	return (ret);
}

/*
 * __add_btree --
 *	Add a btree handle to the session's cache.
 */
int
__wt_session_add_btree(SESSION *session, BTREE *btree,
    const char *key_format, const char *value_format)
{
	BTREE_SESSION *btree_session;
	WT_RET(__wt_calloc(session, 1, sizeof(BTREE_SESSION), &btree_session));

	btree_session->btree = btree;
	btree_session->key_format = key_format;
	btree_session->value_format = value_format;

	TAILQ_INSERT_HEAD(&session->btrees, btree_session, q);

	return (0);
}

/*
 * __get_btree --
 *	Get the btree handle for the named table.
 */
static int
__get_btree(SESSION *session,
    const char *name, size_t namelen, BTREE_SESSION **btree_sessionp)
{
	BTREE *btree;
	BTREE_SESSION *btree_session;

	TAILQ_FOREACH(btree_session, &session->btrees, q) {
		btree = btree_session->btree;
		if (strncmp(name, btree->name, namelen) == 0 &&
		    btree->name[namelen] == '\0') {
			*btree_sessionp = btree_session;
			return (0);
		}
	}

	return (WT_NOTFOUND);
}

/*
 * __wt_cursor_open --
 *	WT_SESSION->open_cursor method for the btree cursor type.
 */
int
__wt_cursor_open(SESSION *session,
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
	const char *tablename;
	BTREE *btree;
	BTREE_SESSION *btree_session;
	CONNECTION *conn;
	CURSOR_BTREE *cbt;
	WT_CONFIG_ITEM cvalue;
	WT_CURSOR *cursor;
	const char *key_format, *value_format;
	int bulk, dump, ret;
	size_t csize;

	conn = S2C(session);

	/* TODO: handle projections. */
	tablename = uri + 6;

	ret = __get_btree(session,
	    tablename, strlen(tablename), &btree_session);
	if (ret == WT_NOTFOUND) {
		ret = 0;
		WT_RET(conn->btree(conn, 0, &btree));
		WT_RET(btree->open(btree, session, tablename, 0666, 0));

		/*
		 * !!! TODO read key / value formats from a table-of-tables.
		 */
		key_format = F_ISSET(btree, WT_COLUMN) ? "r" : "u";
		value_format = "u";
		WT_RET(__wt_session_add_btree(session, btree,
		    key_format, value_format));
	} else {
		WT_ERR(ret);
		btree = btree_session->btree;
		key_format = btree_session->key_format;
		value_format = btree_session->value_format;
	}

	bulk = dump = 0;
	CONFIG_LOOP(session, config, cvalue)
		CONFIG_ITEM("dump")
			dump = (cvalue.val != 0);
		CONFIG_ITEM("bulk")
			bulk = (cvalue.val != 0);
	CONFIG_END(session);

	csize = bulk ? sizeof(CURSOR_BULK) : sizeof(CURSOR_BTREE);
	WT_RET(__wt_calloc(session, 1, csize, &cbt));

	cursor = &cbt->iface;
	*cursor = iface;
	cursor->session = &session->iface;
	cursor->key_format = key_format;
	cursor->value_format = value_format;
	__wt_cursor_init(cursor, config);

	cbt->btree = btree;
	if (bulk)
		WT_ERR(__wt_curbulk_init((CURSOR_BULK *)cbt));
	if (dump)
		__wt_curdump_init(cursor);

	STATIC_ASSERT(offsetof(CURSOR_BTREE, iface) == 0);
	TAILQ_INSERT_HEAD(&session->cursors, cursor, q);
	*cursorp = cursor;

	if (0) {
err:		__wt_free(session, cbt);
	}

	return (ret);
}
