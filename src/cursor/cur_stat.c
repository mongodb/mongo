/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
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
	va_start(ap, cursor);
	CURSOR_API_CALL(cursor, session, get_key, cst->btree);

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
	va_start(ap, cursor);
	CURSOR_API_CALL(cursor, session, get_value, cst->btree);

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
	F_CLR(cursor, WT_CURSTD_KEY_SET);

	va_start(ap, cursor);
	if (F_ISSET(cursor, WT_CURSTD_RAW)) {
		item = va_arg(ap, WT_ITEM *);
		ret = __wt_struct_unpack(session, item->data, item->size,
		    cursor->key_format, &cst->key);
	} else
		cst->key = va_arg(ap, int);
	va_end(ap);

	if ((cursor->saved_err = ret) == 0)
		F_SET(cursor, WT_CURSTD_KEY_APP);

err:	API_END(session);
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
	F_SET(cursor, WT_CURSTD_KEY_RET | WT_CURSTD_VALUE_RET);

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
		F_CLR(cursor, WT_CURSTD_KEY_RET | WT_CURSTD_VALUE_RET);
		WT_ERR(WT_NOTFOUND);
	}

	cst->v = cst->stats_first[cst->key].v;
	WT_ERR(__curstat_print_value(session, cst->v, &cst->pv));
	F_SET(cursor, WT_CURSTD_KEY_RET | WT_CURSTD_VALUE_RET);

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
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cst = (WT_CURSOR_STAT *)cursor;
	CURSOR_API_CALL(cursor, session, reset, cst->btree);

	cst->notpositioned = 1;
	F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

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
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cst = (WT_CURSOR_STAT *)cursor;
	CURSOR_API_CALL(cursor, session, search, cst->btree);

	WT_CURSOR_NEEDKEY(cursor);
	F_CLR(cursor, WT_CURSTD_VALUE_SET | WT_CURSTD_VALUE_SET);

	if (cst->key < 0 || cst->key >= cst->stats_count)
		WT_ERR(WT_NOTFOUND);

	cst->v = cst->stats_first[cst->key].v;
	WT_ERR(__curstat_print_value(session, cst->v, &cst->pv));
	F_SET(cursor, WT_CURSTD_KEY_RET | WT_CURSTD_VALUE_RET);

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

	__wt_free(session, cst->stats);
	WT_TRET(__wt_cursor_close(cursor));

err:	API_END(session);
	return (ret);
}

/*
 * __curstat_conn_init --
 *	Initialize the statistics for a connection.
 */
static void
__curstat_conn_init(
    WT_SESSION_IMPL *session, WT_CURSOR_STAT *cst, uint32_t flags)
{
	__wt_conn_stat_init(session, flags);

	cst->btree = NULL;
	cst->notpositioned = 1;
	cst->stats_first = (WT_STATS *)&S2C(session)->stats;
	cst->stats_count = sizeof(S2C(session)->stats) / sizeof(WT_STATS);
	cst->clear_func = LF_ISSET(WT_STATISTICS_CLEAR) ?
	    __wt_stat_clear_connection_stats : NULL;
}

/*
 * __curstat_file_init --
 *	Initialize the statistics for a file.
 */
static int
__curstat_file_init(WT_SESSION_IMPL *session,
    const char *uri, const char *cfg[], WT_CURSOR_STAT *cst, uint32_t flags)
{
	WT_BTREE *btree;

	WT_RET(__wt_session_get_btree_ckpt(session, uri, cfg, 0));
	btree = S2BT(session);
	WT_RET(__wt_btree_stat_init(session, flags));

	cst->btree = btree;
	cst->notpositioned = 1;
	cst->stats_first = (WT_STATS *)&btree->dhandle->stats;
	cst->stats_count = sizeof(WT_DSRC_STATS) / sizeof(WT_STATS);
	cst->clear_func = LF_ISSET(WT_STATISTICS_CLEAR) ?
	    __wt_stat_clear_dsrc_stats : NULL;
	return (0);
}

/*
 * __curstat_lsm_init --
 *	Initialize the statistics for a LSM tree.
 */
static int
__curstat_lsm_init(WT_SESSION_IMPL *session,
    const char *uri, WT_CURSOR_STAT *cst, uint32_t flags)
{
	WT_DECL_RET;
	WT_LSM_TREE *lsm_tree;

	WT_WITH_SCHEMA_LOCK_OPT(session,
	    ret = __wt_lsm_tree_get(session, uri, 0, &lsm_tree));
	WT_RET(ret);

	ret = __wt_lsm_stat_init(session, lsm_tree, cst, flags);
	__wt_lsm_tree_release(session, lsm_tree);
	WT_RET(ret);

	cst->btree = NULL;
	cst->notpositioned = 1;
	cst->clear_func = LF_ISSET(WT_STATISTICS_CLEAR) ?
	    __wt_stat_clear_dsrc_stats : NULL;
	return (0);
}

/*
 * __wt_curstat_init --
 *	Initialize a statistics cursor.
 */
int
__wt_curstat_init(WT_SESSION_IMPL *session,
    const char *uri, const char *cfg[], WT_CURSOR_STAT *cst, uint32_t flags)
{
	if (strcmp(uri, "statistics:") == 0) {
		__curstat_conn_init(session, cst, flags);
		return (0);
	} else if (WT_PREFIX_MATCH(uri, "statistics:file:"))
		return (__curstat_file_init(session,
		    uri + strlen("statistics:"), cfg, cst, flags));
	else if (WT_PREFIX_MATCH(uri, "statistics:lsm:"))
		return (__curstat_lsm_init(session,
		    uri + strlen("statistics:"), cst, flags));
	else
		return (__wt_schema_stat_init(session, uri, cfg, cst, flags));
}

/*
 * __wt_curstat_open --
 *	WT_SESSION->open_cursor method for the statistics cursor type.
 */
int
__wt_curstat_open(WT_SESSION_IMPL *session,
    const char *uri, const char *cfg[], WT_CURSOR **cursorp)
{
	WT_CURSOR_STATIC_INIT(iface,
	    __curstat_get_key,		/* get-key */
	    __curstat_get_value,	/* get-value */
	    __curstat_set_key,		/* set-key */
	    __curstat_set_value,	/* set-value */
	    NULL,			/* compare */
	    __curstat_next,		/* next */
	    __curstat_prev,		/* prev */
	    __curstat_reset,		/* reset */
	    __curstat_search,		/* search */
	    __wt_cursor_notsup,		/* search-near */
	    __wt_cursor_notsup,		/* insert */
	    __wt_cursor_notsup,		/* update */
	    __wt_cursor_notsup,		/* remove */
	    __curstat_close);		/* close */
	WT_CONFIG_ITEM cval;
	WT_CURSOR *cursor;
	WT_CURSOR_STAT *cst;
	WT_DECL_RET;
	uint32_t flags;

	cst = NULL;
	flags = 0;

	WT_RET(
	    __wt_config_gets_def(session, cfg, "statistics_clear", 0, &cval));
	if (cval.val != 0)
		LF_SET(WT_STATISTICS_CLEAR);
	WT_RET(__wt_config_gets_def(session, cfg, "statistics_fast", 0, &cval));
	if (cval.val != 0)
		LF_SET(WT_STATISTICS_FAST);

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
	WT_ERR(__wt_curstat_init(session, uri, cfg, cst, flags));

	/* __wt_cursor_init is last so we don't have to clean up on error. */
	STATIC_ASSERT(offsetof(WT_CURSOR_STAT, iface) == 0);
	WT_ERR(__wt_cursor_init(cursor, uri, NULL, cfg, cursorp));

	if (0) {
err:		__wt_free(session, cst->stats);
		__wt_free(session, cst);
	}

	return (ret);
}
