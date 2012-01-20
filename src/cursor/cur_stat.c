/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int  __curstat_next(WT_CURSOR *cursor);
static int  __curstat_prev(WT_CURSOR *cursor);

/*
 * __curstat_print_value --
 *	Convert statistics cursor value to printable format.
 */
static int
__curstat_print_value(WT_SESSION_IMPL *session, uint64_t v, WT_BUF *buf)
{
	if (v >= WT_BILLION)
		WT_RET(__wt_buf_fmt(session, buf,
		    "%" PRIu64 "B (%" PRIu64 ")", v / WT_BILLION, v));
	else if (v >= WT_MILLION)
		WT_RET(__wt_buf_fmt(session, buf,
		    "%" PRIu64 "M (%" PRIu64 ")", v / WT_MILLION, v));
	else
		WT_RET(__wt_buf_fmt(session, buf, "%" PRIu64, v));

	return (0);
}

/*
 * __curstat_get_key --
 *	WT_CURSOR->get_key for statistics cursors.
 */
static int
__curstat_get_key(WT_CURSOR *cursor, ...)
{
	WT_CURSOR_STAT *cst;
	WT_ITEM *item;
	WT_SESSION_IMPL *session;
	va_list ap;
	int ret;

	ret = 0;
	CURSOR_API_CALL(cursor, session, get_key, NULL);
	cst = (WT_CURSOR_STAT *)cursor;
	va_start(ap, cursor);

	WT_CURSOR_NEEDKEY(cursor);

	if (F_ISSET(cursor, WT_CURSTD_RAW)) {
		item = va_arg(ap, WT_ITEM *);
		item->data = &cst->key;
		item->size = sizeof(cst->key);
	} else
		*va_arg(ap, int *) = cst->key;

err:	va_end(ap);
	API_END(session);
	return (ret);
}

/*
 * __curstat_get_value --
 *	WT_CURSOR->get_value for statistics cursors.
 */
static int
__curstat_get_value(WT_CURSOR *cursor, ...)
{
	WT_CURSOR_STAT *cst;
	WT_ITEM *item;
	WT_SESSION_IMPL *session;
	va_list ap;
	size_t size;
	int ret;

	ret = 0;
	CURSOR_API_CALL(cursor, session, get_value, NULL);
	cst = (WT_CURSOR_STAT *)cursor;
	va_start(ap, cursor);

	WT_CURSOR_NEEDVALUE(cursor);

	if (F_ISSET(cursor, WT_CURSTD_RAW)) {
		size = __wt_struct_size(
		    session, cursor->value_format,
		    cst->stats_first[cst->key].desc, cst->pv.data, cst->v);
		WT_ERR(__wt_buf_initsize(session, &cursor->value, size));
		WT_ERR(__wt_struct_pack(session, cursor->value.mem,
		    size, cursor->value_format, cst->pv.data, cst->v));
		item = va_arg(ap, WT_ITEM *);
		item->data = cursor->value.data;
		item->size = cursor->value.size;
	} else {
		*va_arg(ap, const char **) = cst->stats_first[cst->key].desc;
		*va_arg(ap, const char **) = cst->pv.data;
		*va_arg(ap, uint64_t *) = cst->v;
	}

err:	va_end(ap);
	API_END(session);
	return (ret);
}

/*
 * __curstat_set_key --
 *	WT_CURSOR->set_key for statistics cursors.
 */
static void
__curstat_set_key(WT_CURSOR *cursor, ...)
{
	WT_SESSION_IMPL *session;
	WT_CURSOR_STAT *cst;
	va_list ap;
	int ret;

	ret = 0;
	CURSOR_API_CALL(cursor, session, set_key, NULL);
	cst = (WT_CURSOR_STAT *)cursor;

	va_start(ap, cursor);
	cst->key = va_arg(ap, int);
	va_end(ap);

	/*
	 * There is currently no way for this call to fail, but do something
	 * with ret to avoid compile warnings.
	 */
	cursor->saved_err = ret;
	F_SET(cursor, WT_CURSTD_KEY_SET);

	API_END(session);
}

/*
 * __curstat_set_value --
 *	WT_CURSOR->set_value for statistics cursors.
 */
static void
__curstat_set_value(WT_CURSOR *cursor, ...)
{
	WT_UNUSED(cursor);
	return;
}

/*
 * __curstat_first --
 *	WT_CURSOR->first method for the statistics cursor type.
 */
static int
__curstat_first(WT_CURSOR *cursor)
{
	WT_CURSOR_STAT *cst;

	cst = (WT_CURSOR_STAT *)cursor;
	cst->notpositioned = 1;
	return (__curstat_next(cursor));
}

/*
 * __curstat_last --
 *	WT_CURSOR->last method for the statistics cursor type.
 */
static int
__curstat_last(WT_CURSOR *cursor)
{
	WT_CURSOR_STAT *cst;

	cst = (WT_CURSOR_STAT *)cursor;
	cst->notpositioned = 1;
	return (__curstat_prev(cursor));
}

/*
 * __curstat_next --
 *	WT_CURSOR->next method for the statistics cursor type.
 */
static int
__curstat_next(WT_CURSOR *cursor)
{
	WT_CURSOR_STAT *cst;
	WT_SESSION_IMPL *session;
	int ret;

	ret = 0;
	CURSOR_API_CALL(cursor, session, next, NULL);
	cst = (WT_CURSOR_STAT *)cursor;

	/* Move to the next item. */
	if (cst->notpositioned) {
		cst->notpositioned = 0;
		cst->key = 0;
	} else if (cst->key < cst->stats_count - 1)
		++cst->key;
	else {
		F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
		WT_ERR(WT_NOTFOUND);
	}
	cst->v = cst->stats_first[cst->key].v;
	WT_ERR(__curstat_print_value(session, cst->v, &cst->pv));
	F_SET(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

err:	API_END(session);
	return (ret);
}

/*
 * __curstat_prev --
 *	WT_CURSOR->prev method for the statistics cursor type.
 */
static int
__curstat_prev(WT_CURSOR *cursor)
{
	WT_CURSOR_STAT *cst;
	WT_SESSION_IMPL *session;
	int ret;

	ret = 0;
	CURSOR_API_CALL(cursor, session, prev, NULL);
	cst = (WT_CURSOR_STAT *)cursor;

	/* Move to the previous item. */
	if (cst->notpositioned) {
		cst->notpositioned = 0;
		cst->key = cst->stats_count - 1;
	} else if (cst->key > 0)
		--cst->key;
	else {
		F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
		WT_ERR(WT_NOTFOUND);
	}

	cst->v = cst->stats_first[cst->key].v;
	WT_ERR(__curstat_print_value(session, cst->v, &cst->pv));
	F_SET(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

err:	API_END(session);
	return (ret);
}

/*
 * __curstat_search --
 *	WT_CURSOR->search method for the statistics cursor type.
 */
static int
__curstat_search(WT_CURSOR *cursor)
{
	WT_CURSOR_STAT *cst;
	WT_SESSION_IMPL *session;
	int ret;

	ret = 0;
	CURSOR_API_CALL(cursor, session, search, NULL);
	cst = (WT_CURSOR_STAT *)cursor;

	WT_CURSOR_NEEDKEY(cursor);
	F_CLR(cursor, WT_CURSTD_VALUE_SET);

	if (cst->key < 0 || cst->key >= cst->stats_count)
		WT_ERR(WT_NOTFOUND);

	cst->v = cst->stats_first[cst->key].v;
	WT_ERR(__curstat_print_value(session, cst->v, &cst->pv));
	F_SET(cursor, WT_CURSTD_VALUE_SET);

err:	API_END(session);
	return (ret);
}

/*
 * __curstat_close --
 *	WT_CURSOR->close method for the statistics cursor type.
 */
static int
__curstat_close(WT_CURSOR *cursor, const char *config)
{
	WT_CONFIG_ITEM cval;
	WT_CURSOR_STAT *cst;
	WT_SESSION_IMPL *session;
	int ret;

	ret = 0;
	CURSOR_API_CALL_CONF(cursor, session, close, NULL, config, cfg);
	cst = (WT_CURSOR_STAT *)cursor;

	WT_TRET(__wt_config_gets(session, cfg, "clear", &cval));
	if (ret == 0 && cval.val != 0 && cst->clear_func)
		cst->clear_func(cst->stats_first);

	__wt_buf_free(session, &cst->pv);

	if (cst->btree != NULL) {
		session->btree = cst->btree;
		WT_TRET(__wt_session_release_btree(session));
		session->btree = NULL;
	}

	WT_TRET(__wt_cursor_close(cursor, config));

err:	API_END(session);
	return (ret);
}

/*
 * __wt_curstat_open --
 *	WT_SESSION->open_cursor method for the statistics cursor type.
 */
int
__wt_curstat_open(WT_SESSION_IMPL *session,
    const char *uri, const char *cfg[], WT_CURSOR **cursorp)
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
		__curstat_search,
					/* search-near */
		(int (*)(WT_CURSOR *, int *))__wt_cursor_notsup,
		__wt_cursor_notsup,	/* insert */
		__wt_cursor_notsup,	/* update */
		__wt_cursor_notsup,	/* remove */
		__curstat_close,
		{ NULL, NULL },		/* TAILQ_ENTRY q */
		0,			/* recno key */
		{ 0 },                  /* recno raw buffer */
		{ NULL, 0, 0, NULL, 0 },/* WT_BUF key */
		{ NULL, 0, 0, NULL, 0 },/* WT_BUF value */
		0,			/* int saved_err */
		0			/* uint32_t flags */
	};
	WT_BTREE *btree;
	WT_CURSOR_STAT *cst;
	WT_CONFIG_ITEM cval;
	WT_CURSOR *cursor;
	WT_STATS *stats_first;
	void (*clear_func)(WT_STATS *);
	int raw, ret, stats_count;

	btree = NULL;
	cst = NULL;
	ret = 0;

	if (!WT_PREFIX_SKIP(uri, "statistics:"))
		return (EINVAL);
	if (WT_PREFIX_MATCH(uri, "file:")) {
		WT_ERR(
		    __wt_session_get_btree(session, uri, uri, NULL, NULL, 0));
		btree = session->btree;
		WT_ERR(__wt_btree_stat_init(session));
		stats_first = (WT_STATS *)session->btree->stats;
		stats_count = sizeof(WT_BTREE_STATS) / sizeof(WT_STATS);
		clear_func = __wt_stat_clear_btree_stats;
	} else {
		__wt_conn_stat_init(session);
		stats_first = (WT_STATS *)S2C(session)->stats;
		stats_count = sizeof(WT_CONNECTION_STATS) / sizeof(WT_STATS);
		clear_func = __wt_stat_clear_connection_stats;
	}

	WT_ERR(__wt_config_gets(session, cfg, "raw", &cval));
	raw = (cval.val != 0);

	WT_ERR(__wt_calloc_def(session, 1, &cst));
	cst->stats_first = stats_first;
	cst->stats_count = stats_count;
	cst->notpositioned = 1;
	cst->clear_func = clear_func;

	cursor = &cst->iface;
	*cursor = iface;
	cursor->session = &session->iface;

	cursor->get_key = __curstat_get_key;
	cursor->set_key = __curstat_set_key;
	cursor->get_value = __curstat_get_value;
	cursor->set_value = __curstat_set_value;

	cst->btree = btree;
	if (raw)
		F_SET(cursor, WT_CURSTD_RAW);

	STATIC_ASSERT(offsetof(WT_CURSOR_STAT, iface) == 0);
	__wt_cursor_init(cursor, 0, 1, cfg);

	/*
	 * We return the statistics field's offset as the key, and a string
	 * description, a string value,  and a uint64_t value as the value
	 * columns.
	 */
	cursor->key_format = "i";
	cursor->value_format = "SSq";

	*cursorp = cursor;

	if (0) {
err:		__wt_free(session, cst);
	}

	return (ret);
}
