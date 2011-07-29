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
	WT_SESSION_IMPL *session;
	va_list ap;

	cst = (WT_CURSOR_STAT *)cursor;
	CURSOR_API_CALL(cursor, session, get_key, NULL);

	if (!F_ISSET(cursor, WT_CURSTD_KEY_SET))
		return ((cursor->saved_err != 0) ? cursor->saved_err : EINVAL);

	va_start(ap, cursor);
	*va_arg(ap, const char **) = cst->stat_name;
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
	WT_SESSION_IMPL *session;
	va_list ap;

	cst = (WT_CURSOR_STAT *)cursor;
	CURSOR_API_CALL(cursor, session, get_value, NULL);

	if (!F_ISSET(cursor, WT_CURSTD_VALUE_SET))
		return ((cursor->saved_err != 0) ? cursor->saved_err : EINVAL);

	va_start(ap, cursor);
	*va_arg(ap, uint64_t *) = cst->stat_value;
	*va_arg(ap, const char **) = cst->stat_desc;
	va_end(ap);

	API_END(session);
	return (0);
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
	if (cst->stats == NULL)
		cst->stats = cst->stats_first;
	if ((s = cst->stats) == NULL || s->desc == NULL) {
		ret = WT_NOTFOUND;
		F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
	} else {
		ret = 0;
		cst->stat_name = s->desc;
		F_SET(cursor, WT_CURSTD_KEY_SET);
		cst->stat_value = s->v;
		WT_RET(__curstat_print_value(session, s->v, &cursor->value));
		cst->stat_desc = cursor->value.data;
		F_SET(cursor, WT_CURSTD_VALUE_SET);

		/* Move to the next item. */
		++cst->stats;
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
	WT_SESSION_IMPL *session;
	int ret;

	ret = 0;

	CURSOR_API_CALL_CONF(cursor, session, close, NULL, config, cfg);
	WT_UNUSED(cfg);

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
	int printable, raw, ret;
	const char *cfg[] = API_CONF_DEFAULTS(session, open_cursor, config);

	cst = NULL;
	ret = 0;

	if (!WT_PREFIX_SKIP(uri, "statistics:"))
		return (EINVAL);
	if (WT_PREFIX_MATCH(uri, "file:")) {
		WT_ERR(__wt_session_get_btree(session, uri));
		WT_RET(__wt_btree_stat_init(session));
		stats_first = (WT_STATS *)session->btree->stats;
	} else {
		__wt_conn_stat_init(session);
		stats_first = (WT_STATS *)S2C(session)->stats;
	}

	WT_ERR(__wt_config_gets(session, cfg, "printable", &cval));
	printable = (cval.val != 0);
	WT_ERR(__wt_config_gets(session, cfg, "raw", &cval));
	raw = (cval.val != 0);

	WT_RET(__wt_calloc_def(session, 1, &cst));
	cst->stats_first = stats_first;
	cursor = &cst->iface;
	*cursor = iface;
	cursor->session = &session->iface;

	/*
	 * There are only two modes, printable or raw -- if it's not set, we
	 * default to raw.
	 */
	if (printable)
		F_SET(cursor, WT_CURSTD_PRINT);
	else /* if (raw) */
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
	cursor->value_format = "qS";

	*cursorp = cursor;

	if (0) {
err:		__wt_free(session, cst);
	}

	return (ret);
}
