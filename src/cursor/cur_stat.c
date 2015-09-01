/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

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
 * __curstat_free_config --
 *	Free the saved configuration string stack
 */
static void
__curstat_free_config(WT_SESSION_IMPL *session, WT_CURSOR_STAT *cst)
{
	size_t i;

	if (cst->cfg != NULL) {
		for (i = 0; cst->cfg[i] != NULL; ++i)
			__wt_free(session, cst->cfg[i]);
		__wt_free(session, cst->cfg);
	}
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
		    cst->stats_desc(WT_STAT_KEY_OFFSET(cst)),
		    cst->pv.data, cst->v));
		WT_ERR(__wt_buf_initsize(session, &cursor->value, size));
		WT_ERR(__wt_struct_pack(session, cursor->value.mem, size,
		    cursor->value_format,
		    cst->stats_desc(WT_STAT_KEY_OFFSET(cst)),
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
			*p = cst->stats_desc(WT_STAT_KEY_OFFSET(cst));
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

	/* Initialize on demand. */
	if (cst->notinitialized) {
		WT_ERR(__wt_curstat_init(
		    session, cursor->internal_uri, cst->cfg, cst));
		cst->notinitialized = 0;
	}

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
	cst->v = (uint64_t)cst->stats[WT_STAT_KEY_OFFSET(cst)];
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

	/* Initialize on demand. */
	if (cst->notinitialized) {
		WT_ERR(__wt_curstat_init(
		    session, cursor->internal_uri, cst->cfg, cst));
		cst->notinitialized = 0;
	}

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

	cst->v = (uint64_t)cst->stats[WT_STAT_KEY_OFFSET(cst)];
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

	cst->notinitialized = cst->notpositioned = 1;
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

	/* Initialize on demand. */
	if (cst->notinitialized) {
		WT_ERR(__wt_curstat_init(
		    session, cursor->internal_uri, cst->cfg, cst));
		cst->notinitialized = 0;
	}

	if (cst->key < WT_STAT_KEY_MIN(cst) || cst->key > WT_STAT_KEY_MAX(cst))
		WT_ERR(WT_NOTFOUND);

	cst->v = (uint64_t)cst->stats[WT_STAT_KEY_OFFSET(cst)];
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

	__curstat_free_config(session, cst);

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
	__wt_stat_connection_aggregate(conn->stats, &cst->u.conn_stats);
	if (F_ISSET(cst, WT_CONN_STAT_CLEAR))
		__wt_stat_connection_clear_all(conn->stats);

	cst->stats = (int64_t *)&cst->u.conn_stats;
	cst->stats_base = WT_CONNECTION_STATS_BASE;
	cst->stats_count = sizeof(WT_CONNECTION_STATS) / sizeof(int64_t);
	cst->stats_desc = __wt_stat_connection_desc;
}

/*
 * __curstat_file_init --
 *	Initialize the statistics for a file.
 */
static int
__curstat_file_init(WT_SESSION_IMPL *session,
    const char *uri, const char *cfg[], WT_CURSOR_STAT *cst)
{
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	const char *filename;

	/*
	 * If we are only getting the size of the file, we don't need to open
	 * the tree.
	 */
	if (F_ISSET(cst, WT_CONN_STAT_SIZE)) {
		filename = uri;
		if (!WT_PREFIX_SKIP(filename, "file:"))
			return (EINVAL);
		__wt_stat_dsrc_init_single(&cst->u.dsrc_stats);
		WT_RET(__wt_block_manager_size(
		    session, filename, &cst->u.dsrc_stats));
		__wt_curstat_dsrc_final(cst);
		return (0);
	}

	WT_RET(__wt_session_get_btree_ckpt(session, uri, cfg, 0));
	dhandle = session->dhandle;

	/*
	 * Fill in the data source statistics, and copy them to the cursor.
	 * Optionally clear the data source statistics.
	 */
	if ((ret = __wt_btree_stat_init(session, cst)) == 0) {
		__wt_stat_dsrc_init_single(&cst->u.dsrc_stats);
		__wt_stat_dsrc_aggregate(dhandle->stats, &cst->u.dsrc_stats);
		if (F_ISSET(cst, WT_CONN_STAT_CLEAR))
			__wt_stat_dsrc_clear_all(dhandle->stats);
		__wt_curstat_dsrc_final(cst);
	}

	/* Release the handle, we're done with it. */
	WT_TRET(__wt_session_release_btree(session));

	return (ret);
}

/*
 * __wt_curstat_dsrc_final --
 *	Finalize a data-source statistics cursor.
 */
void
__wt_curstat_dsrc_final(WT_CURSOR_STAT *cst)
{
	cst->stats = (int64_t *)&cst->u.dsrc_stats;
	cst->stats_base = WT_DSRC_STATS_BASE;
	cst->stats_count = sizeof(WT_DSRC_STATS) / sizeof(int64_t);
	cst->stats_desc = __wt_stat_dsrc_desc;
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
	    __wt_cursor_notsup,		/* equals */
	    __curstat_next,		/* next */
	    __curstat_prev,		/* prev */
	    __curstat_reset,		/* reset */
	    __curstat_search,		/* search */
	    __wt_cursor_notsup,		/* search-near */
	    __wt_cursor_notsup,		/* insert */
	    __wt_cursor_notsup,		/* update */
	    __wt_cursor_notsup,		/* remove */
	    __wt_cursor_notsup,		/* reconfigure */
	    __curstat_close);		/* close */
	WT_CONFIG_ITEM cval, sval;
	WT_CURSOR *cursor;
	WT_CURSOR_STAT *cst;
	WT_DECL_RET;
	size_t i;

	WT_STATIC_ASSERT(offsetof(WT_CURSOR_STAT, iface) == 0);

	conn = S2C(session);

	WT_RET(__wt_calloc_one(session, &cst));
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
		    session, &cval, "size", &sval)) == 0 && sval.val != 0) {
			if (F_ISSET(cst, WT_CONN_STAT_FAST | WT_CONN_STAT_ALL))
				WT_ERR_MSG(session, EINVAL,
				    "only one statistics configuration value "
				    "may be specified");
			F_SET(cst, WT_CONN_STAT_SIZE);
		}
		WT_ERR_NOTFOUND_OK(ret);
		if ((ret = __wt_config_subgets(
		    session, &cval, "clear", &sval)) == 0 && sval.val != 0) {
			if (F_ISSET(cst, WT_CONN_STAT_SIZE))
				WT_ERR_MSG(session, EINVAL,
				    "clear is incompatible with size "
				    "statistics");
			F_SET(cst, WT_CONN_STAT_CLEAR);
		}
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

	/*
	 * WT_CURSOR.reset on a statistics cursor refreshes the cursor, save
	 * the cursor's configuration for that.
	 */
	for (i = 0; cfg[i] != NULL; ++i)
		;
	WT_ERR(__wt_calloc_def(session, i + 1, &cst->cfg));
	for (i = 0; cfg[i] != NULL; ++i)
		WT_ERR(__wt_strdup(session, cfg[i], &cst->cfg[i]));

	/*
	 * Do the initial statistics snapshot: there won't be cursor operations
	 * to trigger initialization when aggregating statistics for upper-level
	 * objects like tables, we need to a valid set of statistics when before
	 * the open returns.
	 */
	WT_ERR(__wt_curstat_init(session, uri, cst->cfg, cst));
	cst->notinitialized = 0;

	/* The cursor isn't yet positioned. */
	cst->notpositioned = 1;

	/* __wt_cursor_init is last so we don't have to clean up on error. */
	WT_ERR(__wt_cursor_init(cursor, uri, NULL, cfg, cursorp));

	if (0) {
config_err:	WT_ERR_MSG(session, EINVAL,
		    "cursor's statistics configuration doesn't match the "
		    "database statistics configuration");
	}

	if (0) {
err:		__curstat_free_config(session, cst);
		__wt_free(session, cst);
	}

	return (ret);
}
