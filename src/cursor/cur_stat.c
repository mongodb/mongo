/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int __curstat_next(WT_CURSOR *cursor);

/*
 * __cursor_notsup --
 *	WT_CURSOR->XXX methods for unsupported statistics cursor actions.
 */
static int
__cursor_notsup(WT_CURSOR *cursor)
{
	WT_UNUSED(cursor);
	return (ENOTSUP);
}

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

	cst = (WT_CURSOR_STAT *)cursor;
	CURSOR_API_CALL(cursor, session, get_key, NULL);

	if (!F_ISSET(cursor, WT_CURSTD_KEY_SET))
		return ((cursor->saved_err != 0) ? cursor->saved_err : EINVAL);

	s = cst->stats;
	va_start(ap, cursor);
	if (F_ISSET(cursor, WT_CURSTD_RAW)) {
		item = va_arg(ap, WT_ITEM *);
		item->data = s->name;
		item->size = (uint32_t)strlen(s->name);
	} else
		*va_arg(ap, const char **) = s->name;
	va_end(ap);

	API_END(session);
	return (0);
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

	cst = (WT_CURSOR_STAT *)cursor;
	s = cst->stats;
	ret = 0;

	CURSOR_API_CALL(cursor, session, get_value, NULL);

	if (!F_ISSET(cursor, WT_CURSTD_VALUE_SET))
		return ((cursor->saved_err != 0) ? cursor->saved_err : EINVAL);

	va_start(ap, cursor);
	if (F_ISSET(cursor, WT_CURSTD_RAW)) {
		size = __wt_struct_size(session,
		    cursor->value_format, s->v, cst->pvalue.data, s->desc);
		WT_ERR(__wt_buf_initsize(session, &cursor->value, size));
		WT_ERR(__wt_struct_pack(session, cursor->value.mem, size,
		    cursor->value_format, s->v, cst->pvalue.data, s->desc));
		item = va_arg(ap, WT_ITEM *);
		item->data = cursor->value.data;
		item->size = cursor->value.size;
	} else {
		*va_arg(ap, uint64_t *) = cst->stats->v;
		*va_arg(ap, const char **) = cst->pvalue.data;
		*va_arg(ap, const char **) = cst->stats->desc;
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
	WT_UNUSED(cursor);
	return;
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
	WT_SESSION_IMPL *session;
	int ret;

	cst = (WT_CURSOR_STAT *)cursor;
	CURSOR_API_CALL(cursor, session, first, NULL);
	cst->stats = NULL;
	ret = __curstat_next(cursor);
	API_END(session);

	return (ret);
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

	cst = (WT_CURSOR_STAT *)cursor;
	CURSOR_API_CALL(cursor, session, next, NULL);
	/* Move to the next item (or first, if the cursor isn't positioned). */
	if (cst->stats == NULL)
		cst->stats = cst->stats_first;
	else
		++cst->stats;
	s = cst->stats;
	if (s->desc == NULL) {
		ret = WT_NOTFOUND;
		F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
	} else {
		WT_RET(__curstat_print_value(session, s->v, &cst->pvalue));
		ret = 0;
		F_SET(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
	}
	API_END(session);

	return (ret);
}

static int
__curstat_search_near(WT_CURSOR *cursor, int *exact)
{
	WT_UNUSED(cursor);
	WT_UNUSED(exact);

	return (ENOTSUP);
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

	cst = (WT_CURSOR_STAT *)cursor;
	CURSOR_API_CALL_CONF(cursor, session, close, NULL, config, cfg);
	WT_TRET(__wt_config_gets(session, cfg, "clear", &cval));
	if (ret == 0 && cval.val != 0 && cst->clear_func)
		cst->clear_func(cst->stats_first);
	__wt_buf_free(session, &cst->pvalue);
	WT_TRET(__wt_cursor_close(cursor, config));
	API_END(session);

	return (ret);
}

/*
 * __wt_curstat_open --
 *	WT_SESSION->open_cursor method for the statistics cursor type.
 */
int
__wt_curstat_open(WT_SESSION_IMPL *session,
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
		__cursor_notsup,
		__curstat_next,
		__cursor_notsup,
		NULL,
		__curstat_search_near,
		__cursor_notsup,
		__cursor_notsup,
		__cursor_notsup,
		__curstat_close,
		{ NULL, NULL },		/* TAILQ_ENTRY q */
		0,			/* recno key */
		{ NULL, 0, 0, NULL, 0 },/* WT_BUF key */
		{ NULL, 0, 0, NULL, 0 },/* WT_BUF value */
		0,			/* int saved_err */
		0			/* uint32_t flags */
	};
	WT_CURSOR_STAT *cst;
	WT_CONFIG_ITEM cval;
	WT_CURSOR *cursor;
	WT_STATS *stats_first;
	void (*clear_func)(WT_STATS *);
	const char *cfg[] = API_CONF_DEFAULTS(session, open_cursor, config);
	const char *filename;
	int raw, ret;

	cst = NULL;
	ret = 0;

	if (!WT_PREFIX_SKIP(uri, "statistics:"))
		return (EINVAL);
	filename = uri;
	if (WT_PREFIX_SKIP(filename, "file:")) {
		WT_ERR(__wt_session_get_btree(session, uri, filename, NULL));
		WT_RET(__wt_btree_stat_init(session));
		stats_first = (WT_STATS *)session->btree->stats;
		clear_func = __wt_stat_clear_btree_stats;
	} else {
		__wt_conn_stat_init(session);
		stats_first = (WT_STATS *)S2C(session)->stats;
		clear_func = __wt_stat_clear_conn_stats;
	}

	WT_ERR(__wt_config_gets(session, cfg, "raw", &cval));
	raw = (cval.val != 0);

	WT_RET(__wt_calloc_def(session, 1, &cst));
	cst->stats_first = stats_first;
	cst->clear_func = clear_func;
	cursor = &cst->iface;
	*cursor = iface;
	cursor->session = &session->iface;

	if (raw)
		F_SET(cursor, WT_CURSTD_RAW);

	cursor->get_key = __curstat_get_key;
	cursor->get_value = __curstat_get_value;
	cursor->set_key = __curstat_set_key;
	cursor->set_value = __curstat_set_value;

	STATIC_ASSERT(offsetof(WT_CURSOR_STAT, iface) == 0);
	__wt_cursor_init(cursor, 1, config);

	/*
	 * We return the name of the statistics field as the key, and
	 * the value plus a string representation as the value columns.
	 */
	cursor->key_format = "S";
	cursor->value_format = "qSS";

	*cursorp = cursor;

	if (0) {
err:		__wt_free(session, cst);
	}

	return (ret);
}
