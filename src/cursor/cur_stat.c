/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int  __curstat_next(WT_CURSOR *cursor);
static int  __curstat_prev(WT_CURSOR *cursor);

/*
 * __curstat_print_value --
 *	Convert statistics cursor value to printable format.
 */
static int
__curstat_print_value(WT_SESSION_IMPL *session, uint64_t v, WT_ITEM *buf)
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
	WT_DECL_RET;
	WT_ITEM *item;
	WT_SESSION_IMPL *session;
	size_t size;
	va_list ap;

	cst = (WT_CURSOR_STAT *)cursor;
	CURSOR_API_CALL(cursor, session, get_key, cst->btree);
	va_start(ap, cursor);

	WT_CURSOR_NEEDKEY(cursor);

	if (F_ISSET(cursor, WT_CURSTD_RAW)) {
		WT_ERR(__wt_struct_size(
		    session, &size, cursor->key_format, cst->key));
		WT_ERR(__wt_buf_initsize(session, &cursor->key, size));
		WT_ERR(__wt_struct_pack(session, cursor->key.mem, size,
		    cursor->key_format, cst->key));

		item = va_arg(ap, WT_ITEM *);
		item->data = cursor->key.data;
		item->size = cursor->key.size;
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
	WT_DECL_RET;
	WT_ITEM *item;
	WT_SESSION_IMPL *session;
	va_list ap;
	size_t size;

	cst = (WT_CURSOR_STAT *)cursor;
	CURSOR_API_CALL(cursor, session, get_value, cst->btree);
	va_start(ap, cursor);

	WT_CURSOR_NEEDVALUE(cursor);

	if (F_ISSET(cursor, WT_CURSTD_RAW)) {
		WT_ERR(__wt_struct_size(session, &size, cursor->value_format,
		    cst->stats_first[cst->key].desc, cst->pv.data, cst->v));
		WT_ERR(__wt_buf_initsize(session, &cursor->value, size));
		WT_ERR(__wt_struct_pack(session, cursor->value.mem, size,
		    cursor->value_format,
		    cst->stats_first[cst->key].desc, cst->pv.data, cst->v));

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
	WT_CURSOR_STAT *cst;
	WT_DECL_RET;
	WT_ITEM *item;
	WT_SESSION_IMPL *session;
	va_list ap;

	cst = (WT_CURSOR_STAT *)cursor;
	CURSOR_API_CALL(cursor, session, set_key, cst->btree);

	va_start(ap, cursor);
	if (F_ISSET(cursor, WT_CURSTD_RAW)) {
		item = va_arg(ap, WT_ITEM *);
		ret = __wt_struct_unpack(session, item->data, item->size,
		    cursor->key_format, &cst->key);
	} else
		cst->key = va_arg(ap, int);
	va_end(ap);

	if ((cursor->saved_err = ret) == 0)
		F_SET(cursor, WT_CURSTD_KEY_SET);
	else
		F_CLR(cursor, WT_CURSTD_KEY_SET);

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
 * __curstat_next --
 *	WT_CURSOR->next method for the statistics cursor type.
 */
static int
__curstat_next(WT_CURSOR *cursor)
{
	WT_CURSOR_STAT *cst;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cst = (WT_CURSOR_STAT *)cursor;
	CURSOR_API_CALL(cursor, session, next, cst->btree);

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
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cst = (WT_CURSOR_STAT *)cursor;
	CURSOR_API_CALL(cursor, session, prev, cst->btree);

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
 * __curstat_reset --
 *	WT_CURSOR->reset method for the statistics cursor type.
 */
static int
__curstat_reset(WT_CURSOR *cursor)
{
	WT_CURSOR_STAT *cst;
	WT_SESSION_IMPL *session;

	cst = (WT_CURSOR_STAT *)cursor;
	CURSOR_API_CALL(cursor, session, reset, cst->btree);

	cst->notpositioned = 1;
	F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

	API_END(session);
	return (0);
}

/*
 * __curstat_search --
 *	WT_CURSOR->search method for the statistics cursor type.
 */
static int
__curstat_search(WT_CURSOR *cursor)
{
	WT_CURSOR_STAT *cst;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cst = (WT_CURSOR_STAT *)cursor;
	CURSOR_API_CALL(cursor, session, search, cst->btree);

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
__curstat_close(WT_CURSOR *cursor)
{
	WT_CURSOR_STAT *cst;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cst = (WT_CURSOR_STAT *)cursor;
	CURSOR_API_CALL(cursor, session, close, cst->btree);

	if (cst->clear_func)
		cst->clear_func(cst->stats_first);

	__wt_buf_free(session, &cst->pv);

	if (cst->btree != NULL)
		WT_TRET(__wt_session_release_btree(session));

	WT_TRET(__wt_cursor_close(cursor));

	API_END(session);
	return (ret);
}

/*
 * __curstat_conn_init --
 *	Initialize the statistics for a connection.
 */
static void
__curstat_conn_init(
    WT_SESSION_IMPL *session, WT_CURSOR_STAT *cst, int statistics_clear)
{
	__wt_conn_stat_init(session);

	cst->btree = NULL;
	cst->notpositioned = 1;
	cst->stats_first = (WT_STATS *)S2C(session)->stats;
	cst->stats_count = sizeof(WT_CONNECTION_STATS) / sizeof(WT_STATS);
	cst->clear_func =
	    statistics_clear ? __wt_stat_clear_connection_stats : NULL;
}

/*
 * __curstat_file_init --
 *	Initialize the statistics for a file.
 */
static int
__curstat_file_init(WT_SESSION_IMPL *session,
    const char *uri, WT_CURSOR_STAT *cst, int statistics_clear)
{
	WT_BTREE *btree;

	WT_RET(__wt_session_get_btree(session, uri, NULL, NULL, 0));
	btree = S2BT(session);
	WT_RET(__wt_btree_stat_init(session));

	cst->btree = btree;
	cst->notpositioned = 1;
	cst->stats_first = (WT_STATS *)btree->stats;
	cst->stats_count = sizeof(WT_BTREE_STATS) / sizeof(WT_STATS);
	cst->clear_func = statistics_clear ? __wt_stat_clear_btree_stats : NULL;
	return (0);
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
		__curstat_get_key,
		__curstat_get_value,
		__curstat_set_key,
		__curstat_set_value,
		NULL,			/* compare */
		__curstat_next,
		__curstat_prev,
		__curstat_reset,
		__curstat_search,
					/* search-near */
		(int (*)(WT_CURSOR *, int *))__wt_cursor_notsup,
		__wt_cursor_notsup,	/* insert */
		__wt_cursor_notsup,	/* update */
		__wt_cursor_notsup,	/* remove */
		__curstat_close,
		{ NULL, NULL },		/* TAILQ_ENTRY q */
		0,			/* recno key */
		{ 0 },			/* recno raw buffer */
		{ NULL, 0, 0, NULL, 0 },/* WT_ITEM key */
		{ NULL, 0, 0, NULL, 0 },/* WT_ITEM value */
		0,			/* int saved_err */
		0			/* uint32_t flags */
	};
	WT_CONFIG_ITEM cval;
	WT_CURSOR *cursor;
	WT_CURSOR_STAT *cst;
	WT_DECL_RET;
	int statistics_clear;

	cst = NULL;

	WT_RET(__wt_config_gets_defno(session, cfg, "statistics_clear", &cval));
	statistics_clear = (cval.val != 0);

	WT_ERR(__wt_calloc_def(session, 1, &cst));
	cursor = &cst->iface;
	*cursor = iface;
	cursor->session = &session->iface;

	/*
	 * We return the statistics field's offset as the key, and a string
	 * description, a string value,  and a uint64_t value as the value
	 * columns.
	 */
	cursor->key_format = "i";
	cursor->value_format = "SSq";

	if (strcmp(uri, "statistics:") == 0)
		__curstat_conn_init(session, cst, statistics_clear);
	else if (WT_PREFIX_MATCH(uri, "statistics:file:"))
		WT_ERR(__curstat_file_init(session,
		    uri + strlen("statistics:"), cst, statistics_clear));
	else
		WT_ERR(__wt_bad_object_type(session, uri));

	/* __wt_cursor_init is last so we don't have to clean up on error. */
	STATIC_ASSERT(offsetof(WT_CURSOR_STAT, iface) == 0);
	WT_ERR(__wt_cursor_init(cursor, uri, NULL, cfg, cursorp));

	if (0) {
err:		__wt_free(session, cst);
	}

	return (ret);
}
