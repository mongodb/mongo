/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int  __curstat_next(WT_CURSOR *cursor);
static int  __curstat_prev(WT_CURSOR *cursor);

/*
 * The statistics identifier is an offset from a base to ensure the integer ID
 * values don't overlap (the idea is if they overlap it's easy for application
 * writers to confuse them).
 */
#define	WT_STAT_KEY_MAX(cst)	(((cst)->stats_base + (cst)->stats_count) - 1)
#define	WT_STAT_KEY_MIN(cst)	((cst)->stats_base)
#define	WT_STAT_KEY_OFFSET(cst)	((cst)->key - (cst)->stats_base)

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
	CURSOR_API_CALL(cursor, session, get_key, NULL);

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
	API_END_RET(session, ret);
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
	uint64_t *v;
	const char **p;

	cst = (WT_CURSOR_STAT *)cursor;
	va_start(ap, cursor);
	CURSOR_API_CALL(cursor, session, get_value, NULL);

	WT_CURSOR_NEEDVALUE(cursor);

	if (F_ISSET(cursor, WT_CURSTD_RAW)) {
		WT_ERR(__wt_struct_size(session, &size, cursor->value_format,
		    cst->stats_first[WT_STAT_KEY_OFFSET(cst)].desc,
		    cst->pv.data, cst->v));
		WT_ERR(__wt_buf_initsize(session, &cursor->value, size));
		WT_ERR(__wt_struct_pack(session, cursor->value.mem, size,
		    cursor->value_format,
		    cst->stats_first[WT_STAT_KEY_OFFSET(cst)].desc,
		    cst->pv.data, cst->v));

		item = va_arg(ap, WT_ITEM *);
		item->data = cursor->value.data;
		item->size = cursor->value.size;
	} else {
		/*
		 * Don't drop core if the statistics value isn't requested; NULL
		 * pointer support isn't documented, but it's a cheap test.
		 */
		if ((p = va_arg(ap, const char **)) != NULL)
			*p = cst->stats_first[WT_STAT_KEY_OFFSET(cst)].desc;
		if ((p = va_arg(ap, const char **)) != NULL)
			*p = cst->pv.data;
		if ((v = va_arg(ap, uint64_t *)) != NULL)
			*v = cst->v;
	}

err:	va_end(ap);
	API_END_RET(session, ret);
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
	CURSOR_API_CALL(cursor, session, set_key, NULL);
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
		F_SET(cursor, WT_CURSTD_KEY_EXT);

err:	API_END(session, ret);
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
	CURSOR_API_CALL(cursor, session, next, NULL);

	/* Move to the next item. */
	if (cst->notpositioned) {
		cst->notpositioned = 0;
		cst->key = WT_STAT_KEY_MIN(cst);
	} else if (cst->key < WT_STAT_KEY_MAX(cst))
		++cst->key;
	else {
		F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
		WT_ERR(WT_NOTFOUND);
	}
	cst->v = cst->stats_first[WT_STAT_KEY_OFFSET(cst)].v;
	WT_ERR(__curstat_print_value(session, cst->v, &cst->pv));
	F_SET(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);

err:	API_END_RET(session, ret);
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
	CURSOR_API_CALL(cursor, session, prev, NULL);

	/* Move to the previous item. */
	if (cst->notpositioned) {
		cst->notpositioned = 0;
		cst->key = WT_STAT_KEY_MAX(cst);
	} else if (cst->key > WT_STAT_KEY_MIN(cst))
		--cst->key;
	else {
		F_CLR(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);
		WT_ERR(WT_NOTFOUND);
	}

	cst->v = cst->stats_first[WT_STAT_KEY_OFFSET(cst)].v;
	WT_ERR(__curstat_print_value(session, cst->v, &cst->pv));
	F_SET(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);

err:	API_END_RET(session, ret);
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
	CURSOR_API_CALL(cursor, session, reset, NULL);

	cst->notpositioned = 1;
	F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

err:	API_END_RET(session, ret);
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
	CURSOR_API_CALL(cursor, session, search, NULL);

	WT_CURSOR_NEEDKEY(cursor);
	F_CLR(cursor, WT_CURSTD_VALUE_SET | WT_CURSTD_VALUE_SET);

	if (cst->key < WT_STAT_KEY_MIN(cst) || cst->key > WT_STAT_KEY_MAX(cst))
		WT_ERR(WT_NOTFOUND);

	cst->v = cst->stats_first[WT_STAT_KEY_OFFSET(cst)].v;
	WT_ERR(__curstat_print_value(session, cst->v, &cst->pv));
	F_SET(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);

err:	API_END_RET(session, ret);
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
	CURSOR_API_CALL(cursor, session, close, NULL);

	__wt_buf_free(session, &cst->pv);

	WT_ERR(__wt_cursor_close(cursor));

err:	API_END_RET(session, ret);
}

/*
 * __curstat_conn_init --
 *	Initialize the statistics for a connection.
 */
static void
__curstat_conn_init(WT_SESSION_IMPL *session, WT_CURSOR_STAT *cst)
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

	/*
	 * Fill in the connection statistics, and copy them to the cursor.
	 * Optionally clear the connection statistics.
	 */
	__wt_conn_stat_init(session);
	cst->u.conn_stats = conn->stats;
	if (F_ISSET(cst, WT_CONN_STAT_CLEAR))
		__wt_stat_refresh_connection_stats(&conn->stats);

	cst->stats_first = cst->stats = (WT_STATS *)&cst->u.conn_stats;
	cst->stats_base = WT_CONNECTION_STATS_BASE;
	cst->stats_count = sizeof(WT_CONNECTION_STATS) / sizeof(WT_STATS);
}

/*
 * When returning the statistics for a file URI, we review open handles, and
 * aggregate checkpoint handle statistics with the file URI statistics.  To
 * make that work, we have to pass information to the function reviewing the
 * handles, this structure is what we pass.
 */
struct __checkpoint_args {
	const char *name;		/* Data source handle name */
	WT_DSRC_STATS *stats;		/* Stat structure being filled */
	int clear;			/* WT_STATISTICS_CLEAR */
};

/*
 * __curstat_checkpoint --
 *	Aggregate statistics from checkpoint handles.
 */
static int
__curstat_checkpoint(WT_SESSION_IMPL *session, const char *cfg[])
{
	struct __checkpoint_args *args;
	WT_DATA_HANDLE *dhandle;

	dhandle = session->dhandle;
	args = (struct __checkpoint_args *)cfg[0];

	/* Aggregate the flagged file's checkpoint handles. */
	if (dhandle->checkpoint != NULL &&
	    strcmp(dhandle->name, args->name) == 0) {
		__wt_stat_aggregate_dsrc_stats(&dhandle->stats, args->stats);
		if (args->clear)
			__wt_stat_refresh_dsrc_stats(&dhandle->stats);
	}

	return (0);
}

/*
 * __curstat_file_init --
 *	Initialize the statistics for a file.
 */
static int
__curstat_file_init(WT_SESSION_IMPL *session,
    const char *uri, const char *cfg[], WT_CURSOR_STAT *cst)
{
	struct __checkpoint_args args;
	WT_DATA_HANDLE *dhandle, *saved_dhandle;
	WT_DECL_RET;
	const char *cfg_arg[] = { NULL, NULL };

	WT_RET(__wt_session_get_btree_ckpt(session, uri, cfg, 0));
	dhandle = session->dhandle;

	/*
	 * Fill in the data source statistics, and copy them to the cursor.
	 * Optionally clear the data source statistics.
	 */
	if ((ret = __wt_btree_stat_init(session, cst)) == 0) {
		cst->u.dsrc_stats = dhandle->stats;
		if (F_ISSET(cst, WT_CONN_STAT_CLEAR))
			__wt_stat_refresh_dsrc_stats(&dhandle->stats);
		__wt_curstat_dsrc_final(cst);
	}

	/* Release the handle, we're done with it. */
	WT_TRET(__wt_session_release_btree(session));
	WT_RET(ret);

	/*
	 * If no checkpoint was specified, review the open handles and aggregate
	 * the statistics from any checkpoint handles matching this file.
	 */
	if (dhandle->checkpoint == NULL) {
		args.name = dhandle->name;
		args.stats = &cst->u.dsrc_stats;
		args.clear = F_ISSET(cst, WT_CONN_STAT_CLEAR);
		cfg_arg[0] = (char *)&args;

		/*
		 * We're likely holding the schema lock inside the statistics
		 * logging thread, not to mention calling __wt_conn_btree_apply
		 * from there as well.  Save/restore the handle.
		 */
		saved_dhandle = dhandle;
		WT_WITH_SCHEMA_LOCK(session,
		    ret = __wt_conn_btree_apply(
		    session, 1, __curstat_checkpoint, cfg_arg));
		session->dhandle = saved_dhandle;
	}

	return (ret);
}

/*
 * __wt_curstat_dsrc_final --
 *	Finalize a data-source statistics cursor.
 */
void
__wt_curstat_dsrc_final(WT_CURSOR_STAT *cst)
{

	cst->stats_first = cst->stats = (WT_STATS *)&cst->u.dsrc_stats;
	cst->stats_base = WT_DSRC_STATS_BASE;
	cst->stats_count = sizeof(WT_DSRC_STATS) / sizeof(WT_STATS);
}

/*
 * __wt_curstat_init --
 *	Initialize a statistics cursor.
 */
int
__wt_curstat_init(WT_SESSION_IMPL *session,
    const char *uri, const char *cfg[], WT_CURSOR_STAT *cst)
{
	const char *dsrc_uri;

	cst->notpositioned = 1;

	if (strcmp(uri, "statistics:") == 0) {
		__curstat_conn_init(session, cst);
		return (0);
	}

	dsrc_uri = uri + strlen("statistics:");

	if (WT_PREFIX_MATCH(dsrc_uri, "colgroup:"))
		return (
		    __wt_curstat_colgroup_init(session, dsrc_uri, cfg, cst));

	if (WT_PREFIX_MATCH(dsrc_uri, "file:"))
		return (__curstat_file_init(session, dsrc_uri, cfg, cst));

	if (WT_PREFIX_MATCH(dsrc_uri, "index:"))
		return (__wt_curstat_index_init(session, dsrc_uri, cfg, cst));

	if (WT_PREFIX_MATCH(dsrc_uri, "lsm:"))
		return (__wt_curstat_lsm_init(session, dsrc_uri, cst));

	if (WT_PREFIX_MATCH(dsrc_uri, "table:"))
		return (__wt_curstat_table_init(session, dsrc_uri, cfg, cst));

	return (__wt_bad_object_type(session, uri));
}

/*
 * __wt_curstat_open --
 *	WT_SESSION->open_cursor method for the statistics cursor type.
 */
int
__wt_curstat_open(WT_SESSION_IMPL *session,
    const char *uri, const char *cfg[], WT_CURSOR **cursorp)
{
	WT_CONNECTION_IMPL *conn;
	WT_CURSOR_STATIC_INIT(iface,
	    __curstat_get_key,		/* get-key */
	    __curstat_get_value,	/* get-value */
	    __curstat_set_key,		/* set-key */
	    __curstat_set_value,	/* set-value */
	    __wt_cursor_notsup,		/* compare */
	    __curstat_next,		/* next */
	    __curstat_prev,		/* prev */
	    __curstat_reset,		/* reset */
	    __curstat_search,		/* search */
	    __wt_cursor_notsup,		/* search-near */
	    __wt_cursor_notsup,		/* insert */
	    __wt_cursor_notsup,		/* update */
	    __wt_cursor_notsup,		/* remove */
	    __curstat_close);		/* close */
	WT_CONFIG_ITEM cval, sval;
	WT_CURSOR *cursor;
	WT_CURSOR_STAT *cst;
	WT_DECL_RET;

	WT_STATIC_ASSERT(offsetof(WT_CURSOR_STAT, iface) == 0);

	conn = S2C(session);

	WT_ERR(__wt_calloc_def(session, 1, &cst));
	cursor = &cst->iface;
	*cursor = iface;
	cursor->session = &session->iface;

	/*
	 * Statistics cursor configuration: must match (and defaults to), the
	 * database configuration.
	 */
	if (FLD_ISSET(conn->stat_flags, WT_CONN_STAT_NONE))
		goto config_err;
	if ((ret = __wt_config_gets(session, cfg, "statistics", &cval)) == 0) {
		if ((ret = __wt_config_subgets(
		    session, &cval, "all", &sval)) == 0 && sval.val != 0) {
			if (!FLD_ISSET(conn->stat_flags, WT_CONN_STAT_ALL))
				goto config_err;
			F_SET(cst, WT_CONN_STAT_ALL | WT_CONN_STAT_FAST);
		}
		WT_ERR_NOTFOUND_OK(ret);
		if ((ret = __wt_config_subgets(
		    session, &cval, "fast", &sval)) == 0 && sval.val != 0) {
			if (F_ISSET(cst, WT_CONN_STAT_ALL))
				WT_ERR_MSG(session, EINVAL,
				    "only one statistics configuration value "
				    "may be specified");
			F_SET(cst, WT_CONN_STAT_FAST);
		}
		WT_ERR_NOTFOUND_OK(ret);
		if ((ret = __wt_config_subgets(
		    session, &cval, "clear", &sval)) == 0 && sval.val != 0)
			F_SET(cst, WT_CONN_STAT_CLEAR);
		WT_ERR_NOTFOUND_OK(ret);

		/* If no configuration, use the connection's configuration. */
		if (cst->flags == 0) {
			if (FLD_ISSET(conn->stat_flags, WT_CONN_STAT_ALL))
				F_SET(cst, WT_CONN_STAT_ALL);
			if (FLD_ISSET(conn->stat_flags, WT_CONN_STAT_FAST))
				F_SET(cst, WT_CONN_STAT_FAST);
		}

		/* If the connection configures clear, so do we. */
		if (FLD_ISSET(conn->stat_flags, WT_CONN_STAT_CLEAR))
			F_SET(cst, WT_CONN_STAT_CLEAR);
	}

	/*
	 * We return the statistics field's offset as the key, and a string
	 * description, a string value,  and a uint64_t value as the value
	 * columns.
	 */
	cursor->key_format = "i";
	cursor->value_format = "SSq";
	WT_ERR(__wt_curstat_init(session, uri, cfg, cst));

	/* __wt_cursor_init is last so we don't have to clean up on error. */
	WT_ERR(__wt_cursor_init(cursor, uri, NULL, cfg, cursorp));

	if (0) {
config_err:	WT_ERR_MSG(session, EINVAL,
		    "cursor's statistics configuration doesn't match the "
		    "database statistics configuration");
	}

	if (0) {
err:		__wt_free(session, cst);
	}

	return (ret);
}
