/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __session_checkpoint(WT_SESSION *, const char *);
static int __session_snapshot(WT_SESSION *, const char *);
static int __session_rollback_transaction(WT_SESSION *, const char *);

/*
 * __wt_session_notsup --
 *	Unsupported session method.
 */
int
__wt_session_notsup(WT_SESSION *wt_session)
{
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;

	WT_RET_MSG(session, ENOTSUP, "Unsupported session method");
}

/*
 * __wt_session_reset_cursors --
 *	Reset all open cursors.
 */
int
__wt_session_reset_cursors(WT_SESSION_IMPL *session, bool free_buffers)
{
	WT_CURSOR *cursor;
	WT_DECL_RET;

	TAILQ_FOREACH(cursor, &session->cursors, q) {
		/* Stop when there are no positioned cursors. */
		if (session->ncursors == 0)
			break;
		if (!F_ISSET(cursor, WT_CURSTD_JOINED))
			WT_TRET(cursor->reset(cursor));
		/* Optionally, free the cursor buffers */
		if (free_buffers) {
			__wt_buf_free(session, &cursor->key);
			__wt_buf_free(session, &cursor->value);
		}
	}

	WT_ASSERT(session, session->ncursors == 0);
	return (ret);
}

/*
 * __wt_session_copy_values --
 *	Copy values into all positioned cursors, so that they don't keep
 *	transaction IDs pinned.
 */
int
__wt_session_copy_values(WT_SESSION_IMPL *session)
{
	WT_CURSOR *cursor;
	WT_DECL_RET;

	TAILQ_FOREACH(cursor, &session->cursors, q)
		if (F_ISSET(cursor, WT_CURSTD_VALUE_INT)) {
			F_CLR(cursor, WT_CURSTD_VALUE_INT);
			WT_RET(__wt_buf_set(session, &cursor->value,
			    cursor->value.data, cursor->value.size));
			F_SET(cursor, WT_CURSTD_VALUE_EXT);
		}

	return (ret);
}

/*
 * __wt_session_release_resources --
 *	Release common session resources.
 */
int
__wt_session_release_resources(WT_SESSION_IMPL *session)
{
	WT_DECL_RET;

	/* Block manager cleanup */
	if (session->block_manager_cleanup != NULL)
		WT_TRET(session->block_manager_cleanup(session));

	/* Reconciliation cleanup */
	if (session->reconcile_cleanup != NULL)
		WT_TRET(session->reconcile_cleanup(session));

	/*
	 * Discard scratch buffers, error memory; last, just in case a cleanup
	 * routine uses scratch buffers.
	 */
	__wt_scr_discard(session);
	__wt_buf_free(session, &session->err);

	return (ret);
}

/*
 * __session_clear --
 *	Clear a session structure.
 */
static void
__session_clear(WT_SESSION_IMPL *session)
{
	/*
	 * There's no serialization support around the review of the hazard
	 * array, which means threads checking for hazard pointers first check
	 * the active field (which may be 0) and then use the hazard pointer
	 * (which cannot be NULL).
	 *
	 * Additionally, the session structure can include information that
	 * persists past the session's end-of-life, stored as part of page
	 * splits.
	 *
	 * For these reasons, be careful when clearing the session structure.
	 */
	memset(session, 0, WT_SESSION_CLEAR_SIZE(session));
	session->hazard_size = 0;
	session->nhazard = 0;
	WT_INIT_LSN(&session->bg_sync_lsn);
}

/*
 * __session_close --
 *	WT_SESSION->close method.
 */
static int
__session_close(WT_SESSION *wt_session, const char *config)
{
	WT_CONNECTION_IMPL *conn;
	WT_CURSOR *cursor;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	conn = (WT_CONNECTION_IMPL *)wt_session->connection;
	session = (WT_SESSION_IMPL *)wt_session;

	SESSION_API_CALL(session, close, config, cfg);
	WT_UNUSED(cfg);

	/* Rollback any active transaction. */
	if (F_ISSET(&session->txn, WT_TXN_RUNNING))
		WT_TRET(__session_rollback_transaction(wt_session, NULL));

	/*
	 * Also release any pinned transaction ID from a non-transactional
	 * operation.
	 */
	if (conn->txn_global.states != NULL)
		__wt_txn_release_snapshot(session);

	/* Close all open cursors. */
	while ((cursor = TAILQ_FIRST(&session->cursors)) != NULL) {
		/*
		 * Notify the user that we are closing the cursor handle
		 * via the registered close callback.
		 */
		if (session->event_handler->handle_close != NULL &&
		    !WT_STREQ(cursor->internal_uri, WT_LAS_URI))
			WT_TRET(session->event_handler->handle_close(
			    session->event_handler, wt_session, cursor));
		WT_TRET(cursor->close(cursor));
	}

	WT_ASSERT(session, session->ncursors == 0);

	/* Discard cached handles. */
	__wt_session_close_cache(session);

	/* Close all tables. */
	WT_TRET(__wt_schema_close_tables(session));

	/* Confirm we're not holding any hazard pointers. */
	__wt_hazard_close(session);

	/* Discard metadata tracking. */
	__wt_meta_track_discard(session);

	/* Free transaction information. */
	__wt_txn_destroy(session);

	/* Release common session resources. */
	WT_TRET(__wt_session_release_resources(session));

	/* Destroy the thread's mutex. */
	WT_TRET(__wt_cond_destroy(session, &session->cond));

	/* The API lock protects opening and closing of sessions. */
	__wt_spin_lock(session, &conn->api_lock);

	/* Decrement the count of open sessions. */
	WT_STAT_FAST_CONN_DECR(session, session_open);

	/*
	 * Sessions are re-used, clear the structure: the clear sets the active
	 * field to 0, which will exclude the hazard array from review by the
	 * eviction thread. Because some session fields are accessed by other
	 * threads, the structure must be cleared carefully.
	 *
	 * We don't need to publish here, because regardless of the active field
	 * being non-zero, the hazard pointer is always valid.
	 */
	__session_clear(session);
	session = conn->default_session;

	/*
	 * Decrement the count of active sessions if that's possible: a session
	 * being closed may or may not be at the end of the array, step toward
	 * the beginning of the array until we reach an active session.
	 */
	while (conn->sessions[conn->session_cnt - 1].active == 0)
		if (--conn->session_cnt == 0)
			break;

	__wt_spin_unlock(session, &conn->api_lock);

	/* We no longer have a session, don't try to update it. */
	session = NULL;

err:	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __session_reconfigure --
 *	WT_SESSION->reconfigure method.
 */
static int
__session_reconfigure(WT_SESSION *wt_session, const char *config)
{
	WT_CONFIG_ITEM cval;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;
	SESSION_API_CALL(session, reconfigure, config, cfg);

	if (F_ISSET(&session->txn, WT_TXN_RUNNING))
		WT_ERR_MSG(session, EINVAL, "transaction in progress");

	WT_TRET(__wt_session_reset_cursors(session, false));

	WT_ERR(__wt_config_gets_def(session, cfg, "isolation", 0, &cval));
	if (cval.len != 0)
		session->isolation = session->txn.isolation =
		    WT_STRING_MATCH("snapshot", cval.str, cval.len) ?
		    WT_ISO_SNAPSHOT :
		    WT_STRING_MATCH("read-uncommitted", cval.str, cval.len) ?
		    WT_ISO_READ_UNCOMMITTED : WT_ISO_READ_COMMITTED;

err:	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __session_open_cursor_int --
 *	Internal version of WT_SESSION::open_cursor, with second cursor arg.
 */
static int
__session_open_cursor_int(WT_SESSION_IMPL *session, const char *uri,
    WT_CURSOR *owner, WT_CURSOR *other, const char *cfg[], WT_CURSOR **cursorp)
{
	WT_COLGROUP *colgroup;
	WT_DATA_SOURCE *dsrc;
	WT_DECL_RET;

	*cursorp = NULL;

	/*
	 * Open specific cursor types we know about, or call the generic data
	 * source open function.
	 *
	 * Unwind a set of string comparisons into a switch statement hoping
	 * the compiler can make it fast, but list the common choices first
	 * instead of sorting so if/else patterns are still fast.
	 */
	switch (uri[0]) {
	/*
	 * Common cursor types.
	 */
	case 't':
		if (WT_PREFIX_MATCH(uri, "table:"))
			WT_RET(__wt_curtable_open(
			    session, uri, owner, cfg, cursorp));
		break;
	case 'c':
		if (WT_PREFIX_MATCH(uri, "colgroup:")) {
			/*
			 * Column groups are a special case: open a cursor on
			 * the underlying data source.
			 */
			WT_RET(__wt_schema_get_colgroup(
			    session, uri, false, NULL, &colgroup));
			WT_RET(__wt_open_cursor(
			    session, colgroup->source, owner, cfg, cursorp));
		} else if (WT_PREFIX_MATCH(uri, "config:"))
			WT_RET(__wt_curconfig_open(
			    session, uri, cfg, cursorp));
		break;
	case 'i':
		if (WT_PREFIX_MATCH(uri, "index:"))
			WT_RET(__wt_curindex_open(
			    session, uri, owner, cfg, cursorp));
		break;
	case 'j':
		if (WT_PREFIX_MATCH(uri, "join:"))
			WT_RET(__wt_curjoin_open(
			    session, uri, owner, cfg, cursorp));
		break;
	case 'l':
		if (WT_PREFIX_MATCH(uri, "lsm:"))
			WT_RET(__wt_clsm_open(
			    session, uri, owner, cfg, cursorp));
		else if (WT_PREFIX_MATCH(uri, "log:"))
			WT_RET(__wt_curlog_open(session, uri, cfg, cursorp));
		break;

	/*
	 * Less common cursor types.
	 */
	case 'f':
		if (WT_PREFIX_MATCH(uri, "file:"))
			WT_RET(__wt_curfile_open(
			    session, uri, owner, cfg, cursorp));
		break;
	case 'm':
		if (WT_PREFIX_MATCH(uri, WT_METADATA_URI))
			WT_RET(__wt_curmetadata_open(
			    session, uri, owner, cfg, cursorp));
		break;
	case 'b':
		if (WT_PREFIX_MATCH(uri, "backup:"))
			WT_RET(__wt_curbackup_open(
			    session, uri, cfg, cursorp));
		break;
	case 's':
		if (WT_PREFIX_MATCH(uri, "statistics:"))
			WT_RET(__wt_curstat_open(session, uri, other, cfg,
			    cursorp));
		break;
	default:
		break;
	}

	if (*cursorp == NULL &&
	    (dsrc = __wt_schema_get_source(session, uri)) != NULL)
		WT_RET(dsrc->open_cursor == NULL ?
		    __wt_object_unsupported(session, uri) :
		    __wt_curds_open(session, uri, owner, cfg, dsrc, cursorp));

	if (*cursorp == NULL)
		return (__wt_bad_object_type(session, uri));

	/*
	 * When opening simple tables, the table code calls this function on the
	 * underlying data source, in which case the application's URI has been
	 * copied.
	 */
	if ((*cursorp)->uri == NULL &&
	    (ret = __wt_strdup(session, uri, &(*cursorp)->uri)) != 0) {
		WT_TRET((*cursorp)->close(*cursorp));
		*cursorp = NULL;
	}

	return (ret);
}

/*
 * __wt_open_cursor --
 *	Internal version of WT_SESSION::open_cursor.
 */
int
__wt_open_cursor(WT_SESSION_IMPL *session,
    const char *uri, WT_CURSOR *owner, const char *cfg[], WT_CURSOR **cursorp)
{
	return (__session_open_cursor_int(session, uri, owner, NULL, cfg,
	    cursorp));
}

/*
 * __session_open_cursor --
 *	WT_SESSION->open_cursor method.
 */
static int
__session_open_cursor(WT_SESSION *wt_session,
    const char *uri, WT_CURSOR *to_dup, const char *config, WT_CURSOR **cursorp)
{
	WT_CURSOR *cursor;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	bool statjoin;

	cursor = *cursorp = NULL;

	session = (WT_SESSION_IMPL *)wt_session;
	SESSION_API_CALL(session, open_cursor, config, cfg);

	statjoin = (to_dup != NULL && uri != NULL &&
	    WT_STREQ(uri, "statistics:join"));
	if ((to_dup == NULL && uri == NULL) ||
	    (to_dup != NULL && uri != NULL && !statjoin))
		WT_ERR_MSG(session, EINVAL,
		    "should be passed either a URI or a cursor to duplicate, "
		    "but not both");

	if (to_dup != NULL && !statjoin) {
		uri = to_dup->uri;
		if (!WT_PREFIX_MATCH(uri, "colgroup:") &&
		    !WT_PREFIX_MATCH(uri, "index:") &&
		    !WT_PREFIX_MATCH(uri, "file:") &&
		    !WT_PREFIX_MATCH(uri, "lsm:") &&
		    !WT_PREFIX_MATCH(uri, WT_METADATA_URI) &&
		    !WT_PREFIX_MATCH(uri, "table:") &&
		    __wt_schema_get_source(session, uri) == NULL)
			WT_ERR(__wt_bad_object_type(session, uri));
	}

	WT_ERR(__session_open_cursor_int(session, uri, NULL,
	    statjoin ? to_dup : NULL, cfg, &cursor));
	if (to_dup != NULL && !statjoin)
		WT_ERR(__wt_cursor_dup_position(to_dup, cursor));

	*cursorp = cursor;

	if (0) {
err:		if (cursor != NULL)
			WT_TRET(cursor->close(cursor));
	}

	/*
	 * Opening a cursor on a non-existent data source will set ret to
	 * either of ENOENT or WT_NOTFOUND at this point.  However,
	 * applications may reasonably do this inside a transaction to check
	 * for the existence of a table or index.
	 *
	 * Prefer WT_NOTFOUND here: that does not force running transactions to
	 * roll back.  It will be mapped back to ENOENT.
	 */
	if (ret == ENOENT)
		ret = WT_NOTFOUND;

	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __wt_session_create --
 *	Internal version of WT_SESSION::create.
 */
int
__wt_session_create(
    WT_SESSION_IMPL *session, const char *uri, const char *config)
{
	WT_DECL_RET;

	WT_WITH_SCHEMA_LOCK(session, ret,
	    WT_WITH_TABLE_LOCK(session, ret,
		ret = __wt_schema_create(session, uri, config)));
	return (ret);
}

/*
 * __session_create --
 *	WT_SESSION->create method.
 */
static int
__session_create(WT_SESSION *wt_session, const char *uri, const char *config)
{
	WT_CONFIG_ITEM cval;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;
	SESSION_API_CALL(session, create, config, cfg);
	WT_UNUSED(cfg);

	/* Disallow objects in the WiredTiger name space. */
	WT_ERR(__wt_str_name_check(session, uri));

	/*
	 * Type configuration only applies to tables, column groups and indexes.
	 * We don't want applications to attempt to layer LSM on top of their
	 * extended data-sources, and the fact we allow LSM as a valid URI is an
	 * invitation to that mistake: nip it in the bud.
	 */
	if (!WT_PREFIX_MATCH(uri, "colgroup:") &&
	    !WT_PREFIX_MATCH(uri, "index:") &&
	    !WT_PREFIX_MATCH(uri, "table:")) {
		/*
		 * We can't disallow type entirely, a configuration string might
		 * innocently include it, for example, a dump/load pair.  If the
		 * underlying type is "file", it's OK ("file" is the underlying
		 * type for every type); if the URI type prefix and the type are
		 * the same, let it go.
		 */
		if ((ret =
		    __wt_config_getones(session, config, "type", &cval)) == 0 &&
		    !WT_STRING_MATCH("file", cval.str, cval.len) &&
		    (strncmp(uri, cval.str, cval.len) != 0 ||
		    uri[cval.len] != ':'))
			WT_ERR_MSG(session, EINVAL,
			    "%s: unsupported type configuration", uri);
		WT_ERR_NOTFOUND_OK(ret);
	}

	ret = __wt_session_create(session, uri, config);

err:	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __session_create_readonly --
 *	WT_SESSION->create method; readonly version.
 */
static int
__session_create_readonly(
    WT_SESSION *wt_session, const char *uri, const char *config)
{
	WT_UNUSED(uri);
	WT_UNUSED(config);

	return (__wt_session_notsup(wt_session));
}

/*
 * __session_log_flush --
 *	WT_SESSION->log_flush method.
 */
static int
__session_log_flush(WT_SESSION *wt_session, const char *config)
{
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	uint32_t flags;

	session = (WT_SESSION_IMPL *)wt_session;
	SESSION_API_CALL(session, log_flush, config, cfg);
	WT_STAT_FAST_CONN_INCR(session, log_flush);

	conn = S2C(session);
	flags = 0;
	/*
	 * If logging is not enabled there is nothing to do.
	 */
	if (!FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED))
		WT_ERR_MSG(session, EINVAL, "logging not enabled");

	WT_ERR(__wt_config_gets_def(session, cfg, "sync", 0, &cval));
	if (WT_STRING_MATCH("background", cval.str, cval.len))
		flags = WT_LOG_BACKGROUND;
	else if (WT_STRING_MATCH("off", cval.str, cval.len))
		flags = WT_LOG_FLUSH;
	else if (WT_STRING_MATCH("on", cval.str, cval.len))
		flags = WT_LOG_FSYNC;
	ret = __wt_log_flush(session, flags);

err:	API_END_RET(session, ret);
}

/*
 * __session_log_flush_readonly --
 *	WT_SESSION->log_flush method; readonly version.
 */
static int
__session_log_flush_readonly(WT_SESSION *wt_session, const char *config)
{
	WT_UNUSED(config);

	return (__wt_session_notsup(wt_session));
}

/*
 * __session_log_printf --
 *	WT_SESSION->log_printf method.
 */
static int
__session_log_printf(WT_SESSION *wt_session, const char *fmt, ...)
    WT_GCC_FUNC_ATTRIBUTE((format (printf, 2, 3)))
{
	WT_SESSION_IMPL *session;
	WT_DECL_RET;
	va_list ap;

	session = (WT_SESSION_IMPL *)wt_session;
	SESSION_API_CALL_NOCONF(session, log_printf);

	va_start(ap, fmt);
	ret = __wt_log_vprintf(session, fmt, ap);
	va_end(ap);

err:	API_END_RET(session, ret);
}

/*
 * __session_log_printf_readonly --
 *	WT_SESSION->log_printf method; readonly version.
 */
static int
__session_log_printf_readonly(WT_SESSION *wt_session, const char *fmt, ...)
    WT_GCC_FUNC_ATTRIBUTE((format (printf, 2, 3)))
{
	WT_UNUSED(fmt);

	return (__wt_session_notsup(wt_session));
}

/*
 * __session_rebalance --
 *	WT_SESSION->rebalance method.
 */
static int
__session_rebalance(WT_SESSION *wt_session, const char *uri, const char *config)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;

	SESSION_API_CALL(session, rebalance, config, cfg);

	/* Block out checkpoints to avoid spurious EBUSY errors. */
	WT_WITH_CHECKPOINT_LOCK(session, ret,
	    WT_WITH_SCHEMA_LOCK(session, ret,
		ret = __wt_schema_worker(session, uri, __wt_bt_rebalance,
		NULL, cfg, WT_DHANDLE_EXCLUSIVE | WT_BTREE_REBALANCE)));

err:	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __session_rebalance_readonly --
 *	WT_SESSION->rebalance method; readonly version.
 */
static int
__session_rebalance_readonly(
    WT_SESSION *wt_session, const char *uri, const char *config)
{
	WT_UNUSED(uri);
	WT_UNUSED(config);

	return (__wt_session_notsup(wt_session));
}

/*
 * __session_rename --
 *	WT_SESSION->rename method.
 */
static int
__session_rename(WT_SESSION *wt_session,
    const char *uri, const char *newuri, const char *config)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;
	SESSION_API_CALL(session, rename, config, cfg);

	/* Disallow objects in the WiredTiger name space. */
	WT_ERR(__wt_str_name_check(session, uri));
	WT_ERR(__wt_str_name_check(session, newuri));

	WT_WITH_CHECKPOINT_LOCK(session, ret,
	    WT_WITH_SCHEMA_LOCK(session, ret,
		WT_WITH_TABLE_LOCK(session, ret,
		    ret = __wt_schema_rename(session, uri, newuri, cfg))));

err:	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __session_rename_readonly --
 *	WT_SESSION->rename method; readonly version.
 */
static int
__session_rename_readonly(WT_SESSION *wt_session,
    const char *uri, const char *newuri, const char *config)
{
	WT_UNUSED(uri);
	WT_UNUSED(newuri);
	WT_UNUSED(config);

	return (__wt_session_notsup(wt_session));
}

/*
 * __session_reset --
 *	WT_SESSION->reset method.
 */
static int
__session_reset(WT_SESSION *wt_session)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;

	SESSION_API_CALL_NOCONF(session, reset);

	if (F_ISSET(&session->txn, WT_TXN_RUNNING))
		WT_ERR_MSG(session, EINVAL, "transaction in progress");

	WT_TRET(__wt_session_reset_cursors(session, true));

	/* Release common session resources. */
	WT_TRET(__wt_session_release_resources(session));

err:	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __wt_session_drop --
 *	Internal version of WT_SESSION::drop.
 */
int
__wt_session_drop(WT_SESSION_IMPL *session, const char *uri, const char *cfg[])
{
	WT_DECL_RET;
	WT_CONFIG_ITEM cval;
	bool checkpoint_wait, lock_wait;

	WT_RET(__wt_config_gets_def(session, cfg, "checkpoint_wait", 1, &cval));
	checkpoint_wait = cval.val != 0;
	WT_RET(__wt_config_gets_def(session, cfg, "lock_wait", 1, &cval));
	lock_wait = cval.val != 0 || F_ISSET(session, WT_SESSION_LOCK_NO_WAIT);

	if (!lock_wait)
		F_SET(session, WT_SESSION_LOCK_NO_WAIT);

	/*
	 * The checkpoint lock only is needed to avoid a spurious EBUSY error
	 * return.
	 */
	if (checkpoint_wait)
		WT_WITH_CHECKPOINT_LOCK(session, ret,
		    WT_WITH_SCHEMA_LOCK(session, ret,
			WT_WITH_TABLE_LOCK(session, ret,
			    ret = __wt_schema_drop(session, uri, cfg))));
	else
		WT_WITH_SCHEMA_LOCK(session, ret,
		    WT_WITH_TABLE_LOCK(session, ret,
			ret = __wt_schema_drop(session, uri, cfg)));

	if (!lock_wait)
		F_CLR(session, WT_SESSION_LOCK_NO_WAIT);

	return (ret);
}

/*
 * __session_drop --
 *	WT_SESSION->drop method.
 */
static int
__session_drop(WT_SESSION *wt_session, const char *uri, const char *config)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;
	SESSION_API_CALL(session, drop, config, cfg);

	/* Disallow objects in the WiredTiger name space. */
	WT_ERR(__wt_str_name_check(session, uri));

	ret = __wt_session_drop(session, uri, cfg);

err:	/* Note: drop operations cannot be unrolled (yet?). */
	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __session_drop_readonly --
 *	WT_SESSION->drop method; readonly version.
 */
static int
__session_drop_readonly(
    WT_SESSION *wt_session, const char *uri, const char *config)
{
	WT_UNUSED(uri);
	WT_UNUSED(config);

	return (__wt_session_notsup(wt_session));
}

/*
 * __session_join --
 *	WT_SESSION->join method.
 */
static int
__session_join(WT_SESSION *wt_session, WT_CURSOR *join_cursor,
    WT_CURSOR *ref_cursor, const char *config)
{
	WT_CONFIG_ITEM cval;
	WT_CURSOR *firstcg;
	WT_CURSOR_INDEX *cindex;
	WT_CURSOR_JOIN *cjoin;
	WT_CURSOR_TABLE *ctable;
	WT_DECL_RET;
	WT_INDEX *idx;
	WT_SESSION_IMPL *session;
	WT_TABLE *table;
	bool nested;
	uint64_t count;
	uint32_t bloom_bit_count, bloom_hash_count;
	uint8_t flags, range;

	session = (WT_SESSION_IMPL *)wt_session;
	SESSION_API_CALL(session, join, config, cfg);

	firstcg = NULL;
	table = NULL;
	nested = false;
	count = 0;

	if (!WT_PREFIX_MATCH(join_cursor->uri, "join:"))
		WT_ERR_MSG(session, EINVAL, "not a join cursor");

	if (WT_PREFIX_MATCH(ref_cursor->uri, "index:")) {
		cindex = (WT_CURSOR_INDEX *)ref_cursor;
		idx = cindex->index;
		table = cindex->table;
		firstcg = cindex->cg_cursors[0];
	} else if (WT_PREFIX_MATCH(ref_cursor->uri, "table:")) {
		idx = NULL;
		ctable = (WT_CURSOR_TABLE *)ref_cursor;
		table = ctable->table;
		firstcg = ctable->cg_cursors[0];
	} else if (WT_PREFIX_MATCH(ref_cursor->uri, "join:")) {
		idx = NULL;
		table = ((WT_CURSOR_JOIN *)ref_cursor)->table;
		nested = true;
	} else
		WT_ERR_MSG(session, EINVAL,
		    "ref_cursor must be an index, table or join cursor");

	if (firstcg != NULL && !F_ISSET(firstcg, WT_CURSTD_KEY_SET))
		WT_ERR_MSG(session, EINVAL,
		    "requires reference cursor be positioned");
	cjoin = (WT_CURSOR_JOIN *)join_cursor;
	if (cjoin->table != table)
		WT_ERR_MSG(session, EINVAL,
		    "table for join cursor does not match table for "
		    "ref_cursor");
	if (F_ISSET(ref_cursor, WT_CURSTD_JOINED))
		WT_ERR_MSG(session, EINVAL,
		    "cursor already used in a join");

	/* "ge" is the default */
	range = WT_CURJOIN_END_GT | WT_CURJOIN_END_EQ;
	flags = 0;
	WT_ERR(__wt_config_gets(session, cfg, "compare", &cval));
	if (cval.len != 0) {
		if (WT_STRING_MATCH("gt", cval.str, cval.len))
			range = WT_CURJOIN_END_GT;
		else if (WT_STRING_MATCH("lt", cval.str, cval.len))
			range = WT_CURJOIN_END_LT;
		else if (WT_STRING_MATCH("le", cval.str, cval.len))
			range = WT_CURJOIN_END_LE;
		else if (WT_STRING_MATCH("eq", cval.str, cval.len))
			range = WT_CURJOIN_END_EQ;
		else if (!WT_STRING_MATCH("ge", cval.str, cval.len))
			WT_ERR(EINVAL);
	}
	WT_ERR(__wt_config_gets(session, cfg, "count", &cval));
	if (cval.len != 0)
		count = (uint64_t)cval.val;

	WT_ERR(__wt_config_gets(session, cfg, "strategy", &cval));
	if (cval.len != 0) {
		if (WT_STRING_MATCH("bloom", cval.str, cval.len))
			LF_SET(WT_CURJOIN_ENTRY_BLOOM);
		else if (!WT_STRING_MATCH("default", cval.str, cval.len))
			WT_ERR(EINVAL);
	}
	WT_ERR(__wt_config_gets(session, cfg, "bloom_bit_count", &cval));
	if ((uint64_t)cval.val > UINT32_MAX)
		WT_ERR_MSG(session, EINVAL,
		    "bloom_bit_count: value too large");
	bloom_bit_count = (uint32_t)cval.val;
	WT_ERR(__wt_config_gets(session, cfg, "bloom_hash_count", &cval));
	if ((uint64_t)cval.val > UINT32_MAX)
		WT_ERR_MSG(session, EINVAL,
		    "bloom_hash_count: value too large");
	bloom_hash_count = (uint32_t)cval.val;
	if (LF_ISSET(WT_CURJOIN_ENTRY_BLOOM) && count == 0)
		WT_ERR_MSG(session, EINVAL,
		    "count must be nonzero when strategy=bloom");

	WT_ERR(__wt_config_gets(session, cfg, "operation", &cval));
	if (cval.len != 0 && WT_STRING_MATCH("or", cval.str, cval.len))
		LF_SET(WT_CURJOIN_ENTRY_DISJUNCTION);

	if (nested && (count != 0 || range != WT_CURJOIN_END_EQ ||
	    LF_ISSET(WT_CURJOIN_ENTRY_BLOOM)))
		WT_ERR_MSG(session, EINVAL,
		    "joining a nested join cursor is incompatible with "
		    "setting \"strategy\", \"compare\" or \"count\"");

	WT_ERR(__wt_curjoin_join(session, cjoin, idx, ref_cursor, flags,
	    range, count, bloom_bit_count, bloom_hash_count));
	/*
	 * There's an implied ownership ordering that isn't
	 * known when the cursors are created: the join cursor
	 * must be closed before any of the indices.  Enforce
	 * that here by reordering.
	 */
	if (TAILQ_FIRST(&session->cursors) != join_cursor) {
		TAILQ_REMOVE(&session->cursors, join_cursor, q);
		TAILQ_INSERT_HEAD(&session->cursors, join_cursor, q);
	}
	/* Disable the reference cursor for regular operations */
	F_SET(ref_cursor, WT_CURSTD_JOINED);

err:	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __session_salvage --
 *	WT_SESSION->salvage method.
 */
static int
__session_salvage(WT_SESSION *wt_session, const char *uri, const char *config)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;

	SESSION_API_CALL(session, salvage, config, cfg);

	if (F_ISSET(S2C(session), WT_CONN_IN_MEMORY))
		WT_ERR(ENOTSUP);

	/* Block out checkpoints to avoid spurious EBUSY errors. */
	WT_WITH_CHECKPOINT_LOCK(session, ret,
	    WT_WITH_SCHEMA_LOCK(session, ret,
		ret = __wt_schema_worker(session, uri, __wt_salvage,
		NULL, cfg, WT_DHANDLE_EXCLUSIVE | WT_BTREE_SALVAGE)));

err:	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __session_salvage_readonly --
 *	WT_SESSION->salvage method; readonly version.
 */
static int
__session_salvage_readonly(
    WT_SESSION *wt_session, const char *uri, const char *config)
{
	WT_UNUSED(uri);
	WT_UNUSED(config);

	return (__wt_session_notsup(wt_session));
}

/*
 * __wt_session_range_truncate --
 *	Session handling of a range truncate.
 */
int
__wt_session_range_truncate(WT_SESSION_IMPL *session,
    const char *uri, WT_CURSOR *start, WT_CURSOR *stop)
{
	WT_CURSOR *cursor;
	WT_DECL_RET;
	int cmp;
	bool local_start;

	local_start = false;
	if (uri != NULL) {
		WT_ASSERT(session, WT_PREFIX_MATCH(uri, "file:"));
		/*
		 * A URI file truncate becomes a range truncate where we
		 * set a start cursor at the beginning.  We already
		 * know the NULL stop goes to the end of the range.
		 */
		WT_ERR(__session_open_cursor(
		    (WT_SESSION *)session, uri, NULL, NULL, &start));
		local_start = true;
		ret = start->next(start);
		if (ret == WT_NOTFOUND) {
			/*
			 * If there are no elements, there is nothing
			 * to do.
			 */
			ret = 0;
			goto done;
		}
		WT_ERR(ret);
	}

	/*
	 * Cursor truncate is only supported for some objects, check for the
	 * supporting methods we need, range_truncate and compare.
	 */
	cursor = start == NULL ? stop : start;
	if (cursor->compare == NULL)
		WT_ERR(__wt_bad_object_type(session, cursor->uri));

	/*
	 * If both cursors set, check they're correctly ordered with respect to
	 * each other.  We have to test this before any search, the search can
	 * change the initial cursor position.
	 *
	 * Rather happily, the compare routine will also confirm the cursors
	 * reference the same object and the keys are set.
	 */
	if (start != NULL && stop != NULL) {
		WT_ERR(start->compare(start, stop, &cmp));
		if (cmp > 0)
			WT_ERR_MSG(session, EINVAL,
			    "the start cursor position is after the stop "
			    "cursor position");
	}

	/*
	 * Truncate does not require keys actually exist so that applications
	 * can discard parts of the object's name space without knowing exactly
	 * what records currently appear in the object.  For this reason, do a
	 * search-near, rather than a search.  Additionally, we have to correct
	 * after calling search-near, to position the start/stop cursors on the
	 * next record greater than/less than the original key.
	 */
	if (start != NULL) {
		WT_ERR(start->search_near(start, &cmp));
		if (cmp < 0 && (ret = start->next(start)) != 0) {
			WT_ERR_NOTFOUND_OK(ret);
			goto done;
		}
	}
	if (stop != NULL) {
		WT_ERR(stop->search_near(stop, &cmp));
		if (cmp > 0 && (ret = stop->prev(stop)) != 0) {
			WT_ERR_NOTFOUND_OK(ret);
			goto done;
		}
	}

	/*
	 * We always truncate in the forward direction because the underlying
	 * data structures can move through pages faster forward than backward.
	 * If we don't have a start cursor, create one and position it at the
	 * first record.
	 */
	if (start == NULL) {
		WT_ERR(__session_open_cursor(
		    (WT_SESSION *)session, stop->uri, NULL, NULL, &start));
		local_start = true;
		WT_ERR(start->next(start));
	}

	/*
	 * If the start/stop keys cross, we're done, the range must be empty.
	 */
	if (stop != NULL) {
		WT_ERR(start->compare(start, stop, &cmp));
		if (cmp > 0)
			goto done;
	}

	WT_ERR(__wt_schema_range_truncate(session, start, stop));

done:
err:	/*
	 * Close any locally-opened start cursor.
	 */
	if (local_start)
		WT_TRET(start->close(start));
	return (ret);
}

/*
 * __session_truncate --
 *	WT_SESSION->truncate method.
 */
static int
__session_truncate(WT_SESSION *wt_session,
    const char *uri, WT_CURSOR *start, WT_CURSOR *stop, const char *config)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;
	SESSION_TXN_API_CALL(session, truncate, config, cfg);
	WT_STAT_FAST_CONN_INCR(session, cursor_truncate);

	/*
	 * If the URI is specified, we don't need a start/stop, if start/stop
	 * is specified, we don't need a URI.  One exception is the log URI
	 * which may truncate (archive) log files for a backup cursor.
	 *
	 * If no URI is specified, and both cursors are specified, start/stop
	 * must reference the same object.
	 *
	 * Any specified cursor must have been initialized.
	 */
	if ((uri == NULL && start == NULL && stop == NULL) ||
	    (uri != NULL && !WT_PREFIX_MATCH(uri, "log:") &&
	    (start != NULL || stop != NULL)))
		WT_ERR_MSG(session, EINVAL,
		    "the truncate method should be passed either a URI or "
		    "start/stop cursors, but not both");

	if (uri != NULL) {
		/* Disallow objects in the WiredTiger name space. */
		WT_ERR(__wt_str_name_check(session, uri));

		if (WT_PREFIX_MATCH(uri, "log:")) {
			/*
			 * Verify the user only gave the URI prefix and not
			 * a specific target name after that.
			 */
			if (!WT_STREQ(uri, "log:"))
				WT_ERR_MSG(session, EINVAL,
				    "the truncate method should not specify any"
				    "target after the log: URI prefix");
			WT_ERR(__wt_log_truncate_files(session, start, cfg));
		} else if (WT_PREFIX_MATCH(uri, "file:"))
			WT_ERR(__wt_session_range_truncate(
			    session, uri, start, stop));
		else
			/* Wait for checkpoints to avoid EBUSY errors. */
			WT_WITH_CHECKPOINT_LOCK(session, ret,
			    WT_WITH_SCHEMA_LOCK(session, ret,
				ret = __wt_schema_truncate(session, uri, cfg)));
	} else
		WT_ERR(__wt_session_range_truncate(session, uri, start, stop));

err:	TXN_API_END_RETRY(session, ret, 0);

	/*
	 * Only map WT_NOTFOUND to ENOENT if a URI was specified.
	 */
	return (ret == WT_NOTFOUND && uri != NULL ? ENOENT : ret);
}

/*
 * __session_truncate_readonly --
 *	WT_SESSION->truncate method; readonly version.
 */
static int
__session_truncate_readonly(WT_SESSION *wt_session,
    const char *uri, WT_CURSOR *start, WT_CURSOR *stop, const char *config)
{
	WT_UNUSED(uri);
	WT_UNUSED(start);
	WT_UNUSED(stop);
	WT_UNUSED(config);

	return (__wt_session_notsup(wt_session));
}

/*
 * __session_upgrade --
 *	WT_SESSION->upgrade method.
 */
static int
__session_upgrade(WT_SESSION *wt_session, const char *uri, const char *config)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;

	SESSION_API_CALL(session, upgrade, config, cfg);
	/* Block out checkpoints to avoid spurious EBUSY errors. */
	WT_WITH_CHECKPOINT_LOCK(session, ret,
	    WT_WITH_SCHEMA_LOCK(session, ret,
		ret = __wt_schema_worker(session, uri, __wt_upgrade,
		NULL, cfg, WT_DHANDLE_EXCLUSIVE | WT_BTREE_UPGRADE)));

err:	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __session_upgrade_readonly --
 *	WT_SESSION->upgrade method; readonly version.
 */
static int
__session_upgrade_readonly(
    WT_SESSION *wt_session, const char *uri, const char *config)
{
	WT_UNUSED(uri);
	WT_UNUSED(config);

	return (__wt_session_notsup(wt_session));
}

/*
 * __session_verify --
 *	WT_SESSION->verify method.
 */
static int
__session_verify(WT_SESSION *wt_session, const char *uri, const char *config)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;

	SESSION_API_CALL(session, verify, config, cfg);

	if (F_ISSET(S2C(session), WT_CONN_IN_MEMORY))
		WT_ERR(ENOTSUP);

	/* Block out checkpoints to avoid spurious EBUSY errors. */
	WT_WITH_CHECKPOINT_LOCK(session, ret,
	    WT_WITH_SCHEMA_LOCK(session, ret,
		ret = __wt_schema_worker(session, uri, __wt_verify,
		NULL, cfg, WT_DHANDLE_EXCLUSIVE | WT_BTREE_VERIFY)));

err:	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __session_begin_transaction --
 *	WT_SESSION->begin_transaction method.
 */
static int
__session_begin_transaction(WT_SESSION *wt_session, const char *config)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;
	SESSION_API_CALL(session, begin_transaction, config, cfg);
	WT_STAT_FAST_CONN_INCR(session, txn_begin);

	if (F_ISSET(&session->txn, WT_TXN_RUNNING))
		WT_ERR_MSG(session, EINVAL, "Transaction already running");

	ret = __wt_txn_begin(session, cfg);

err:	API_END_RET(session, ret);
}

/*
 * __session_commit_transaction --
 *	WT_SESSION->commit_transaction method.
 */
static int
__session_commit_transaction(WT_SESSION *wt_session, const char *config)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	WT_TXN *txn;

	session = (WT_SESSION_IMPL *)wt_session;
	SESSION_API_CALL(session, commit_transaction, config, cfg);
	WT_STAT_FAST_CONN_INCR(session, txn_commit);

	txn = &session->txn;
	if (F_ISSET(txn, WT_TXN_ERROR) && txn->mod_count != 0)
		WT_ERR_MSG(session, EINVAL,
		    "failed transaction requires rollback");

	if (ret == 0)
		ret = __wt_txn_commit(session, cfg);
	else {
		WT_TRET(__wt_session_reset_cursors(session, false));
		WT_TRET(__wt_txn_rollback(session, cfg));
	}

err:	API_END_RET(session, ret);
}

/*
 * __session_rollback_transaction --
 *	WT_SESSION->rollback_transaction method.
 */
static int
__session_rollback_transaction(WT_SESSION *wt_session, const char *config)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;
	SESSION_API_CALL(session, rollback_transaction, config, cfg);
	WT_STAT_FAST_CONN_INCR(session, txn_rollback);

	WT_TRET(__wt_session_reset_cursors(session, false));

	WT_TRET(__wt_txn_rollback(session, cfg));

err:	API_END_RET(session, ret);
}

/*
 * __session_transaction_pinned_range --
 *	WT_SESSION->transaction_pinned_range method.
 */
static int
__session_transaction_pinned_range(WT_SESSION *wt_session, uint64_t *prange)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	WT_TXN_STATE *txn_state;
	uint64_t pinned;

	session = (WT_SESSION_IMPL *)wt_session;
	SESSION_API_CALL_NOCONF(session, pinned_range);

	txn_state = WT_SESSION_TXN_STATE(session);

	/* Assign pinned to the lesser of id or snap_min */
	if (txn_state->id != WT_TXN_NONE &&
	    WT_TXNID_LT(txn_state->id, txn_state->snap_min))
		pinned = txn_state->id;
	else
		pinned = txn_state->snap_min;

	if (pinned == WT_TXN_NONE)
		*prange = 0;
	else
		*prange = S2C(session)->txn_global.current - pinned;

err:	API_END_RET(session, ret);
}

/*
 * __session_transaction_sync --
 *	WT_SESSION->transaction_sync method.
 */
static int
__session_transaction_sync(WT_SESSION *wt_session, const char *config)
{
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;
	WT_SESSION_IMPL *session;
	WT_TXN *txn;
	struct timespec now, start;
	uint64_t timeout_ms, waited_ms;
	bool forever;

	session = (WT_SESSION_IMPL *)wt_session;
	SESSION_API_CALL(session, transaction_sync, config, cfg);
	WT_STAT_FAST_CONN_INCR(session, txn_sync);

	conn = S2C(session);
	txn = &session->txn;
	if (F_ISSET(txn, WT_TXN_RUNNING))
		WT_ERR_MSG(session, EINVAL, "transaction in progress");

	/*
	 * If logging is not enabled there is nothing to do.
	 */
	if (!FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED))
		WT_ERR_MSG(session, EINVAL, "logging not enabled");

	log = conn->log;
	timeout_ms = waited_ms = 0;
	forever = true;

	/*
	 * If there is no background sync LSN in this session, there
	 * is nothing to do.
	 */
	if (WT_IS_INIT_LSN(&session->bg_sync_lsn))
		goto err;

	/*
	 * If our LSN is smaller than the current sync LSN then our
	 * transaction is stable.  We're done.
	 */
	if (__wt_log_cmp(&session->bg_sync_lsn, &log->sync_lsn) <= 0)
		goto err;

	/*
	 * Our LSN is not yet stable.  Wait and check again depending on the
	 * timeout.
	 */
	WT_ERR(__wt_config_gets_def(
	    session, cfg, "timeout_ms", (int)UINT_MAX, &cval));
	if ((unsigned int)cval.val != UINT_MAX) {
		timeout_ms = (uint64_t)cval.val;
		forever = false;
	}

	if (timeout_ms == 0)
		WT_ERR(ETIMEDOUT);

	WT_ERR(__wt_epoch(session, &start));
	/*
	 * Keep checking the LSNs until we find it is stable or we reach
	 * our timeout.
	 */
	while (__wt_log_cmp(&session->bg_sync_lsn, &log->sync_lsn) > 0) {
		WT_ERR(__wt_cond_signal(session, conn->log_file_cond));
		WT_ERR(__wt_epoch(session, &now));
		waited_ms = WT_TIMEDIFF_MS(now, start);
		if (forever || waited_ms < timeout_ms)
			/*
			 * Note, we will wait an increasing amount of time
			 * each iteration, likely doubling.  Also note that
			 * the function timeout value is in usecs (we are
			 * computing the wait time in msecs and passing that
			 * in, unchanged, as the usecs to wait).
			 */
			WT_ERR(__wt_cond_wait(
			    session, log->log_sync_cond, waited_ms));
		else
			WT_ERR(ETIMEDOUT);
	}

err:	API_END_RET(session, ret);
}

/*
 * __session_transaction_sync_readonly --
 *	WT_SESSION->transaction_sync method; readonly version.
 */
static int
__session_transaction_sync_readonly(WT_SESSION *wt_session, const char *config)
{
	WT_UNUSED(config);

	return (__wt_session_notsup(wt_session));
}

/*
 * __session_checkpoint --
 *	WT_SESSION->checkpoint method.
 */
static int
__session_checkpoint(WT_SESSION *wt_session, const char *config)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	WT_TXN *txn;

	session = (WT_SESSION_IMPL *)wt_session;

	WT_STAT_FAST_CONN_INCR(session, txn_checkpoint);
	SESSION_API_CALL(session, checkpoint, config, cfg);

	if (F_ISSET(S2C(session), WT_CONN_IN_MEMORY))
		WT_ERR(ENOTSUP);

	/*
	 * Checkpoints require a snapshot to write a transactionally consistent
	 * snapshot of the data.
	 *
	 * We can't use an application's transaction: if it has uncommitted
	 * changes, they will be written in the checkpoint and may appear after
	 * a crash.
	 *
	 * Use a real snapshot transaction: we don't want any chance of the
	 * snapshot being updated during the checkpoint.  Eviction is prevented
	 * from evicting anything newer than this because we track the oldest
	 * transaction ID in the system that is not visible to all readers.
	 */
	txn = &session->txn;
	if (F_ISSET(txn, WT_TXN_RUNNING))
		WT_ERR_MSG(session, EINVAL,
		    "Checkpoint not permitted in a transaction");

	ret = __wt_txn_checkpoint(session, cfg);

	/*
	 * Release common session resources (for example, checkpoint may acquire
	 * significant reconciliation structures/memory).
	 */
	WT_TRET(__wt_session_release_resources(session));

err:	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __session_checkpoint_readonly --
 *	WT_SESSION->checkpoint method; readonly version.
 */
static int
__session_checkpoint_readonly(WT_SESSION *wt_session, const char *config)
{
	WT_UNUSED(config);

	return (__wt_session_notsup(wt_session));
}

/*
 * __session_snapshot --
 *	WT_SESSION->snapshot method.
 */
static int
__session_snapshot(WT_SESSION *wt_session, const char *config)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	WT_TXN_GLOBAL *txn_global;
	bool has_create, has_drop;

	has_create = has_drop = false;
	session = (WT_SESSION_IMPL *)wt_session;
	txn_global = &S2C(session)->txn_global;

	SESSION_API_CALL(session, snapshot, config, cfg);

	WT_ERR(__wt_txn_named_snapshot_config(
	    session, cfg, &has_create, &has_drop));

	WT_ERR(__wt_writelock(session, txn_global->nsnap_rwlock));

	/* Drop any snapshots to be removed first. */
	if (has_drop)
		WT_ERR(__wt_txn_named_snapshot_drop(session, cfg));

	/* Start the named snapshot if requested. */
	if (has_create)
		WT_ERR(__wt_txn_named_snapshot_begin(session, cfg));

err:	WT_TRET(__wt_writeunlock(session, txn_global->nsnap_rwlock));

	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __wt_session_strerror --
 *	WT_SESSION->strerror method.
 */
const char *
__wt_session_strerror(WT_SESSION *wt_session, int error)
{
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;

	return (__wt_strerror(session, error, NULL, 0));
}

/*
 * __open_session --
 *	Allocate a session handle.
 */
static int
__open_session(WT_CONNECTION_IMPL *conn,
    WT_EVENT_HANDLER *event_handler, const char *config,
    WT_SESSION_IMPL **sessionp)
{
	static const WT_SESSION stds = {
		NULL,
		NULL,
		__session_close,
		__session_reconfigure,
		__wt_session_strerror,
		__session_open_cursor,
		__session_create,
		__wt_session_compact,
		__session_drop,
		__session_join,
		__session_log_flush,
		__session_log_printf,
		__session_rebalance,
		__session_rename,
		__session_reset,
		__session_salvage,
		__session_truncate,
		__session_upgrade,
		__session_verify,
		__session_begin_transaction,
		__session_commit_transaction,
		__session_rollback_transaction,
		__session_checkpoint,
		__session_snapshot,
		__session_transaction_pinned_range,
		__session_transaction_sync
	}, stds_readonly = {
		NULL,
		NULL,
		__session_close,
		__session_reconfigure,
		__wt_session_strerror,
		__session_open_cursor,
		__session_create_readonly,
		__wt_session_compact_readonly,
		__session_drop_readonly,
		__session_join,
		__session_log_flush_readonly,
		__session_log_printf_readonly,
		__session_rebalance_readonly,
		__session_rename_readonly,
		__session_reset,
		__session_salvage_readonly,
		__session_truncate_readonly,
		__session_upgrade_readonly,
		__session_verify,
		__session_begin_transaction,
		__session_commit_transaction,
		__session_rollback_transaction,
		__session_checkpoint_readonly,
		__session_snapshot,
		__session_transaction_pinned_range,
		__session_transaction_sync_readonly
	};
	WT_DECL_RET;
	WT_SESSION_IMPL *session, *session_ret;
	uint32_t i;

	*sessionp = NULL;

	session = conn->default_session;
	session_ret = NULL;

	__wt_spin_lock(session, &conn->api_lock);

	/*
	 * Make sure we don't try to open a new session after the application
	 * closes the connection.  This is particularly intended to catch
	 * cases where server threads open sessions.
	 */
	WT_ASSERT(session, F_ISSET(conn, WT_CONN_SERVER_RUN));

	/* Find the first inactive session slot. */
	for (session_ret = conn->sessions,
	    i = 0; i < conn->session_size; ++session_ret, ++i)
		if (!session_ret->active)
			break;
	if (i == conn->session_size)
		WT_ERR_MSG(session, ENOMEM,
		    "only configured to support %" PRIu32 " sessions"
		    " (including %" PRIu32 " additional internal sessions)",
		    conn->session_size, WT_EXTRA_INTERNAL_SESSIONS);

	/*
	 * If the active session count is increasing, update it.  We don't worry
	 * about correcting the session count on error, as long as we don't mark
	 * this session as active, we'll clean it up on close.
	 */
	if (i >= conn->session_cnt)	/* Defend against off-by-one errors. */
		conn->session_cnt = i + 1;

	session_ret->id = i;
	session_ret->iface =
	    F_ISSET(conn, WT_CONN_READONLY) ? stds_readonly : stds;
	session_ret->iface.connection = &conn->iface;

	WT_ERR(__wt_cond_alloc(session, "session", false, &session_ret->cond));

	if (WT_SESSION_FIRST_USE(session_ret))
		__wt_random_init(&session_ret->rnd);

	__wt_event_handler_set(session_ret,
	    event_handler == NULL ? session->event_handler : event_handler);

	TAILQ_INIT(&session_ret->cursors);
	TAILQ_INIT(&session_ret->dhandles);
	/*
	 * If we don't have one, allocate the dhandle hash array.
	 * Allocate the table hash array as well.
	 */
	if (session_ret->dhhash == NULL)
		WT_ERR(__wt_calloc(session_ret, WT_HASH_ARRAY_SIZE,
		    sizeof(struct __dhandles_hash), &session_ret->dhhash));
	if (session_ret->tablehash == NULL)
		WT_ERR(__wt_calloc(session_ret, WT_HASH_ARRAY_SIZE,
		    sizeof(struct __tables_hash), &session_ret->tablehash));
	for (i = 0; i < WT_HASH_ARRAY_SIZE; i++) {
		TAILQ_INIT(&session_ret->dhhash[i]);
		TAILQ_INIT(&session_ret->tablehash[i]);
	}

	/* Initialize transaction support: default to read-committed. */
	session_ret->isolation = WT_ISO_READ_COMMITTED;
	WT_ERR(__wt_txn_init(session_ret));

	/*
	 * The session's hazard pointer memory isn't discarded during normal
	 * session close because access to it isn't serialized.  Allocate the
	 * first time we open this session.
	 */
	if (WT_SESSION_FIRST_USE(session_ret))
		WT_ERR(__wt_calloc_def(
		    session, conn->hazard_max, &session_ret->hazard));

	/*
	 * Set an initial size for the hazard array. It will be grown as
	 * required up to hazard_max. The hazard_size is reset on close, since
	 * __wt_hazard_close ensures the array is cleared - so it is safe to
	 * reset the starting size on each open.
	 */
	session_ret->hazard_size = 0;

	/*
	 * Configuration: currently, the configuration for open_session is the
	 * same as session.reconfigure, so use that function.
	 */
	if (config != NULL)
		WT_ERR(
		    __session_reconfigure((WT_SESSION *)session_ret, config));

	session_ret->name = NULL;

	/*
	 * Publish: make the entry visible to server threads.  There must be a
	 * barrier for two reasons, to ensure structure fields are set before
	 * any other thread will consider the session, and to push the session
	 * count to ensure the eviction thread can't review too few slots.
	 */
	WT_PUBLISH(session_ret->active, 1);

	WT_STATIC_ASSERT(offsetof(WT_SESSION_IMPL, iface) == 0);
	*sessionp = session_ret;

	WT_STAT_FAST_CONN_INCR(session, session_open);

err:	__wt_spin_unlock(session, &conn->api_lock);
	return (ret);
}

/*
 * __wt_open_session --
 *	Allocate a session handle.
 */
int
__wt_open_session(WT_CONNECTION_IMPL *conn,
    WT_EVENT_HANDLER *event_handler, const char *config,
    bool open_metadata, WT_SESSION_IMPL **sessionp)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	WT_SESSION *wt_session;

	*sessionp = NULL;

	/* Acquire a session. */
	WT_RET(__open_session(conn, event_handler, config, &session));

	/*
	 * Acquiring the metadata handle requires the schema lock; we've seen
	 * problems in the past where a session has acquired the schema lock
	 * unexpectedly, relatively late in the run, and deadlocked. Be
	 * defensive, get it now.  The metadata file may not exist when the
	 * connection first creates its default session or the shared cache
	 * pool creates its sessions, let our caller decline this work.
	 */
	if (open_metadata) {
		WT_ASSERT(session, !F_ISSET(session, WT_SESSION_LOCKED_SCHEMA));
		if ((ret = __wt_metadata_cursor(session, NULL)) != 0) {
			wt_session = &session->iface;
			WT_TRET(wt_session->close(wt_session, NULL));
			return (ret);
		}
	}

	*sessionp = session;
	return (0);
}

/*
 * __wt_open_internal_session --
 *	Allocate a session for WiredTiger's use.
 */
int
__wt_open_internal_session(WT_CONNECTION_IMPL *conn, const char *name,
    bool open_metadata, uint32_t session_flags, WT_SESSION_IMPL **sessionp)
{
	WT_DECL_RET;
	WT_SESSION *wt_session;
	WT_SESSION_IMPL *session;

	*sessionp = NULL;

	/* Acquire a session. */
	WT_RET(__wt_open_session(conn, NULL, NULL, open_metadata, &session));
	session->name = name;

	/*
	 * Public sessions are automatically closed during WT_CONNECTION->close.
	 * If the session handles for internal threads were to go on the public
	 * list, there would be complex ordering issues during close.  Set a
	 * flag to avoid this: internal sessions are not closed automatically.
	 */
	F_SET(session, session_flags | WT_SESSION_INTERNAL);

	/*
	 * Acquiring the lookaside table cursor requires various locks; we've
	 * seen problems in the past where deadlocks happened because sessions
	 * deadlocked getting the cursor late in the process.  Be defensive,
	 * get it now.
	 */
	if (F_ISSET(session, WT_SESSION_LOOKASIDE_CURSOR) &&
	    (ret = __wt_las_cursor_open(session, &session->las_cursor)) != 0) {
		wt_session = &session->iface;
		WT_TRET(wt_session->close(wt_session, NULL));
		return (ret);
	}

	*sessionp = session;
	return (0);
}
