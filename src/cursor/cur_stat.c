/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

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
__curstat_print_value(WT_SESSION_IMPL *session, WT_BUF *buf)
{
	WT_BUF *tmp;
	uint64_t v;

	WT_RET(__wt_scr_alloc(session, 64, &tmp));

	v = *(uint64_t *)buf->data;
	if (v >= WT_BILLION)
		tmp->size = snprintf(tmp->mem, tmp->mem_size,
		    "%" PRIu64 "B (%" PRIu64 ")", v / WT_BILLION, v);
	else if (v >= WT_MILLION)
		tmp->size = snprintf(tmp->mem, tmp->mem_size,
		    "%" PRIu64 "M (%" PRIu64 ")", v / WT_MILLION, v);
	else
		tmp->size = snprintf(tmp->mem, tmp->mem_size, "%" PRIu64, v);

	__wt_buf_swap(buf, tmp);
	__wt_scr_release(&tmp);

	return (0);
}

/*
 * __curstat_get_key --
 *	WT_CURSOR->get_key for statistics cursors.
 */
static int
__curstat_get_key(WT_CURSOR *cursor, ...)
{
	WT_SESSION_IMPL *session;
	WT_ITEM *key;
	va_list ap;

	CURSOR_API_CALL(cursor, session, get_key, NULL);

	if (!F_ISSET(cursor, WT_CURSTD_KEY_SET))
		return ((cursor->saved_err != 0) ? cursor->saved_err : EINVAL);

	va_start(ap, cursor);
	key = va_arg(ap, WT_ITEM *);
	key->data = cursor->key.data;
	key->size = cursor->key.size;
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
	WT_SESSION_IMPL *session;
	WT_ITEM *value;
	va_list ap;

	CURSOR_API_CALL(cursor, session, get_value, NULL);

	if (!F_ISSET(cursor, WT_CURSTD_VALUE_SET))
		return ((cursor->saved_err != 0) ? cursor->saved_err : EINVAL);

	if (F_ISSET(cursor, WT_CURSTD_PRINT))
		WT_RET(__curstat_print_value(session, &cursor->value));
	va_start(ap, cursor);
	value = va_arg(ap, WT_ITEM *);
	value->data = cursor->value.data;
	value->size = cursor->value.size;
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
	CURSOR_API_CALL(cursor, session, first, cst->btree);
	ret = cst->btree == NULL ?
	    __wt_conn_stat_first(cst) : __wt_btree_stat_first(cst);
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
	int ret;

	cst = (WT_CURSOR_STAT *)cursor;
	CURSOR_API_CALL(cursor, session, next, cst->btree);
	ret = cst->btree == NULL ?
	    __wt_conn_stat_next(cst) : __wt_btree_stat_next(cst);
	API_END(session);

	return (ret);
}

/*
 * __curstat_search_near --
 *	WT_CURSOR->search_near method for the statistics cursor type.
 */
static int
__curstat_search_near(WT_CURSOR *cursor, int *lastcmp)
{
	WT_SESSION_IMPL *session;
	int ret;

	WT_UNUSED(lastcmp);

	CURSOR_API_CALL(cursor, session, search_near, NULL);
	ret = ENOTSUP;
	API_END(session);

	return (ret);
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
	int printable, raw, ret;
	const char *cfg[] = API_CONF_DEFAULTS(session, open_cursor, config);

	cst = NULL;
	ret = 0;

	/*
	 * This code is called in two ways: (1) as a general statistics cursor,
	 * and (2) as a statistics cursor on a file.  We can tell which we are
	 * doing by looking at the WT_SESSION_IMPL's btree reference, it's not
	 * set in the general case.
	 */
	if (session->btree == NULL)
		if (!WT_PREFIX_SKIP(uri, "statistics:"))
			return (EINVAL);

	WT_ERR(__wt_config_gets(session, cfg, "printable", &cval));
	printable = (cval.val != 0);
	WT_ERR(__wt_config_gets(session, cfg, "raw", &cval));
	raw = (cval.val != 0);

	WT_RET(__wt_calloc_def(session, 1, &cst));
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

	cst->btree = session->btree;

	cursor->get_key = __curstat_get_key;
	cursor->get_value = __curstat_get_value;
	cursor->set_key = __curstat_set_key;
	cursor->set_value = __curstat_set_value;

	STATIC_ASSERT(offsetof(WT_CURSOR_STAT, iface) == 0);
	__wt_cursor_init(cursor, 1, config);

	*cursorp = cursor;

	if (0) {
err:		if (cst != NULL)
			__wt_free(session, cst);
	}

	return (ret);
}
