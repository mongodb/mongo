/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int __curstat_next(WT_CURSOR *cursor);
static int __curstat_prev(WT_CURSOR *cursor);

/*
 * __curstat_print_value --
 *	Convert statistics cursor value to printable format.
 */
static int
__curstat_print_value(WT_SESSION_IMPL *session, uint64_t v, WT_BUF *buf)
{
	WT_RET(__wt_buf_init(session, buf, 64));
	if (v >= WT_BILLION)
		WT_RET(__wt_buf_sprintf(session, buf,
		    "%" PRIu64 "B (%" PRIu64 ")", v / WT_BILLION, v));
	else if (v >= WT_MILLION)
		WT_RET(__wt_buf_sprintf(session, buf,
		    "%" PRIu64 "M (%" PRIu64 ")", v / WT_MILLION, v));
	else
		WT_RET(__wt_buf_sprintf(session, buf, "%" PRIu64, v));

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
	WT_STATS *s;
	va_list ap;
	int ret;

	CURSOR_API_CALL(cursor, session, get_key, NULL);
	WT_CURSOR_NEEDKEY(cursor);

	cst = (WT_CURSOR_STAT *)cursor;
	s = cst->stats;
	va_start(ap, cursor);
	if (F_ISSET(cursor, WT_CURSTD_RAW)) {
		item = va_arg(ap, WT_ITEM *);
		item->data = s->desc;
		item->size = WT_STORE_SIZE(strlen(s->desc));
	} else
		*va_arg(ap, const char **) = s->desc;
	va_end(ap);

err:	API_END(session);
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
	WT_STATS *s;
	va_list ap;
	size_t size;
	int ret;

	CURSOR_API_CALL(cursor, session, get_value, NULL);
	WT_CURSOR_NEEDVALUE(cursor);

	cst = (WT_CURSOR_STAT *)cursor;
	s = cst->stats;

	va_start(ap, cursor);
	if (F_ISSET(cursor, WT_CURSTD_RAW)) {
		size = __wt_struct_size(session,
		    cursor->value_format, cst->pvalue.data, s->v);
		WT_ERR(__wt_buf_initsize(session, &cursor->value, size));
		WT_ERR(__wt_struct_pack(session, cursor->value.mem, size,
		    cursor->value_format, cst->pvalue.data, s->v));
		item = va_arg(ap, WT_ITEM *);
		item->data = cursor->value.data;
		item->size = cursor->value.size;
	} else {
		*va_arg(ap, const char **) = cst->pvalue.data;
		*va_arg(ap, uint64_t *) = cst->stats->v;
	}
err:	va_end(ap);
	API_END(session);
	return (ret);
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
	cst->stats = NULL;
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
	cst->stats = NULL;
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
	WT_STATS *s;
	int ret;

	CURSOR_API_CALL(cursor, session, next, NULL);

	/*
	 * Move to the next item (or the last item, if the cursor isn't
	 * positioned).
	 */
	cst = (WT_CURSOR_STAT *)cursor;
	if (cst->stats == NULL)
		cst->stats = cst->stats_first;
	else
		++cst->stats;
	s = cst->stats;
	if (s->desc == NULL) {
		ret = WT_NOTFOUND;
		F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
	} else {
		WT_ERR(__curstat_print_value(session, s->v, &cst->pvalue));
		ret = 0;
		F_SET(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
	}

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
	WT_STATS *s;
	int ret;

	CURSOR_API_CALL(cursor, session, next, NULL);

	/*
	 * Move to the previous item (or the last item, if the cursor isn't
	 * positioned).
	 */
	cst = (WT_CURSOR_STAT *)cursor;
	if (cst->stats == NULL)
		cst->stats = cst->stats_last - 1;
	else {
		if (cst->stats == cst->stats_first)
			goto notfound;
		--cst->stats;
	}
	s = cst->stats;
	if (s->desc == NULL) {
notfound:	ret = WT_NOTFOUND;
		F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
	} else {
		WT_ERR(__curstat_print_value(session, s->v, &cst->pvalue));
		ret = 0;
		F_SET(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
	}

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
	const char *desc;
	size_t len;
	int ret;

	WT_CURSOR_NEEDKEY(cursor);
	len = strlen(cursor->key.data);

	cst = (WT_CURSOR_STAT *)cursor;		/* Search from the beginning */
	cst->stats = NULL;

	while (
	    (ret = cursor->next(cursor)) == 0 &&
	    (ret = cursor->get_key(cursor, &desc)) == 0)
		if (strncmp(cursor->key.data, desc, len) == 0)
			break;
err:	return (ret);
}

/*
 * __curstat_search_near --
 *	WT_CURSOR->search_near method for the statistics cursor type.
 */
static int
__curstat_search_near(WT_CURSOR *cursor, int *exact)
{
	WT_CURSOR_STAT *cst;
	const char *desc;
	int ret;

	WT_CURSOR_NEEDKEY(cursor);

	cst = (WT_CURSOR_STAT *)cursor;		/* Search from the beginning */
	cst->stats = NULL;

	while (
	    (ret = cursor->next(cursor)) == 0 &&
	    (ret = cursor->get_key(cursor, &desc)) == 0)
		if ((*exact = strcmp(cursor->key.data, desc)) <= 0)
			break;
err:	return (ret);
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

	CURSOR_API_CALL_CONF(cursor, session, close, NULL, config, cfg);

	cst = (WT_CURSOR_STAT *)cursor;
	WT_TRET(__wt_config_gets(session, cfg, "clear", &cval));
	if (ret == 0 && cval.val != 0 && cst->clear_func)
		cst->clear_func(cst->stats_first);
	__wt_buf_free(session, &cst->pvalue);
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
	WT_BTREE *btree;
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
		__curstat_search_near,
		__wt_cursor_notsup,
		__wt_cursor_notsup,
		__wt_cursor_notsup,
		__curstat_close,
		{ NULL, NULL },		/* TAILQ_ENTRY q */
		0,			/* recno key */
		{ 0 },                  /* recno raw buffer */
		{ NULL, 0, 0, NULL, 0 },/* WT_BUF key */
		{ NULL, 0, 0, NULL, 0 },/* WT_BUF value */
		0,			/* int saved_err */
		0			/* uint32_t flags */
	};
	WT_CURSOR_STAT *cst;
	WT_CONFIG_ITEM cval;
	WT_CURSOR *cursor;
	WT_STATS *stats_first, *stats_last;
	void (*clear_func)(WT_STATS *);
	int raw, ret;

	btree = NULL;
	cst = NULL;
	ret = 0;

	if (!WT_PREFIX_SKIP(uri, "statistics:"))
		return (EINVAL);
	if (WT_PREFIX_MATCH(uri, "file:")) {
		WT_ERR(__wt_session_get_btree(session,
		    uri, uri, NULL, NULL, 0));
		btree = session->btree;
		WT_RET(__wt_btree_stat_init(session));
		stats_first = (WT_STATS *)session->btree->stats;
		stats_last = (WT_STATS *)&session->btree->stats->__end;
		clear_func = __wt_stat_clear_btree_stats;
	} else {
		__wt_conn_stat_init(session);
		stats_first = (WT_STATS *)S2C(session)->stats;
		stats_last = (WT_STATS *)&S2C(session)->stats->__end;
		clear_func = __wt_stat_clear_connection_stats;
	}

	WT_ERR(__wt_config_gets(session, cfg, "raw", &cval));
	raw = (cval.val != 0);

	WT_RET(__wt_calloc_def(session, 1, &cst));
	cst->stats_first = stats_first;
	cst->stats_last = stats_last;
	cst->clear_func = clear_func;

	cursor = &cst->iface;
	*cursor = iface;
	cursor->session = &session->iface;

	cursor->get_key = __curstat_get_key;
	cursor->get_value = __curstat_get_value;
	cursor->set_value = __curstat_set_value;

	cst->btree = btree;
	if (raw)
		F_SET(cursor, WT_CURSTD_RAW);

	STATIC_ASSERT(offsetof(WT_CURSOR_STAT, iface) == 0);
	__wt_cursor_init(cursor, 1, cfg);

	/*
	 * We return the statistics field's description string as the key, and
	 * a uint64_t value plus a printable representation of the value as the
	 * value columns.
	 */
	cursor->key_format = "S";
	cursor->value_format = "Sq";

	*cursorp = cursor;

	if (0) {
err:		__wt_free(session, cst);
	}

	return (ret);
}
