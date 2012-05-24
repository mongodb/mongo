/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __session_rollback_transaction(WT_SESSION *, const char *);

/*
 * __session_close_cursors --
 *	Close all cursors open in a session.
 */
static int
__session_close_cursors(WT_SESSION_IMPL *session)
{
	WT_CURSOR *cursor;
	WT_DECL_RET;

	ret = 0;
	while ((cursor = TAILQ_FIRST(&session->cursors)) != NULL)
		WT_TRET(cursor->close(cursor));
	return (ret);
}

/*
 * __session_close --
 *	WT_SESSION->close method.
 */
static int
__session_close(WT_SESSION *wt_session, const char *config)
{
	WT_BTREE_SESSION *btree_session;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	conn = (WT_CONNECTION_IMPL *)wt_session->connection;
	session = (WT_SESSION_IMPL *)wt_session;

	SESSION_API_CALL(session, close, config, cfg);
	WT_UNUSED(cfg);

	if (F_ISSET(&session->txn, TXN_RUNNING))
		WT_TRET(__session_rollback_transaction(wt_session, NULL));

	WT_TRET(__session_close_cursors(session));

	while ((btree_session = TAILQ_FIRST(&session->btrees)) != NULL)
		WT_TRET(__wt_session_discard_btree(session, btree_session));

	WT_TRET(__wt_schema_close_tables(session));

	__wt_spin_lock(session, &conn->spinlock);

	/* Discard metadata tracking. */
	__wt_meta_track_discard(session);

	/* Discard scratch buffers. */
	__wt_scr_discard(session);

	/* Free transaction information. */
	__wt_txn_destroy(session);

	/* Confirm we're not holding any hazard references. */
	__wt_hazard_empty(session);

	/* Free the reconciliation information. */
	__wt_rec_destroy(session);

	/* Free the eviction exclusive-lock information. */
	__wt_free(session, session->excl);

	/* Destroy the thread's mutex. */
	if (session->cond != NULL)
		(void)__wt_cond_destroy(session, session->cond);

	/*
	 * Sessions are re-used, clear the structure: this code sets the active
	 * field to 0, which will exclude the hazard array from review by the
	 * eviction thread.   Note: there's no serialization support around the
	 * review of the hazard array, which means threads checking for hazard
	 * references first check the active field (which may be 0) and then use
	 * the hazard pointer (which cannot be NULL).  For this reason, clear
	 * the session structure carefully.
	 */
	WT_SESSION_CLEAR(session);
	session = conn->default_session;

	/*
	 * Decrement the count of active sessions if that's possible: a session
	 * being closed may or may not be at the end of the array, step toward
	 * the beginning of the array until we reach an active session.
	 */
	while (conn->sessions[conn->session_cnt - 1].active == 0)
		if (--conn->session_cnt == 0)
			break;

	__wt_spin_unlock(session, &conn->spinlock);

err:	API_END_NOTFOUND_MAP(session, ret);
}

/*
 * __session_open_cursor --
 *	WT_SESSION->open_cursor method.
 */
static int
__session_open_cursor(WT_SESSION *wt_session,
    const char *uri, WT_CURSOR *to_dup, const char *config, WT_CURSOR **cursorp)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;
	SESSION_API_CALL(session, open_cursor, config, cfg);

	if (uri != NULL && to_dup != NULL)
		WT_ERR_MSG(session, EINVAL,
		    "should be passed either a URI or a cursor, but not both");

	if (to_dup != NULL)
		ret = __wt_cursor_dup(session, to_dup, config, cursorp);
	else if (WT_PREFIX_MATCH(uri, "colgroup:"))
		ret = __wt_curfile_open(session, uri, NULL, cfg, cursorp);
	else if (WT_PREFIX_MATCH(uri, "config:"))
		ret = __wt_curconfig_open(session, uri, cfg, cursorp);
	else if (WT_PREFIX_MATCH(uri, "file:"))
		ret = __wt_curfile_open(session, uri, NULL, cfg, cursorp);
	else if (WT_PREFIX_MATCH(uri, "index:"))
		ret = __wt_curindex_open(session, uri, cfg, cursorp);
	else if (WT_PREFIX_MATCH(uri, "statistics:"))
		ret = __wt_curstat_open(session, uri, cfg, cursorp);
	else if (WT_PREFIX_MATCH(uri, "table:"))
		ret = __wt_curtable_open(session, uri, cfg, cursorp);
	else {
		__wt_err(session, EINVAL, "Unknown cursor type '%s'", uri);
		ret = EINVAL;
	}

err:	API_END_NOTFOUND_MAP(session, ret);
}

/*
 * __wt_session_create_strip --
 *	Discard any configuration information from a schema entry that is not
 * applicable to an session.create call, here for the wt dump command utility,
 * which only wants to dump the schema information needed for load.
 */
int
__wt_session_create_strip(
    WT_SESSION *session, const char *v1, const char *v2, const char **value_ret)
{
	WT_SESSION_IMPL *session_impl = (WT_SESSION_IMPL *)session;
	const char *cfg[] = { __wt_confdfl_session_create, v1, v2, NULL };

	return (__wt_config_collapse(session_impl, cfg, value_ret));
}

/*
 * __session_create --
 *	WT_SESSION->create method.
 */
static int
__session_create(WT_SESSION *wt_session, const char *uri, const char *config)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;
	SESSION_API_CALL(session, create, config, cfg);
	WT_UNUSED(cfg);

	/* Disallow objects in the WiredTiger name space. */
	WT_ERR(__wt_schema_name_check(session, uri));

	WT_ERR(__wt_schema_create(session, uri, config));

err:	API_END_NOTFOUND_MAP(session, ret);
}

/*
 * __session_rename --
 *	WT_SESSION->rename method.
 */
static int
__session_rename(WT_SESSION *wt_session,
    const char *uri, const char *newname, const char *config)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;
	SESSION_API_CALL(session, rename, config, cfg);
	ret = __wt_schema_rename(session, uri, newname, cfg);

err:	API_END_NOTFOUND_MAP(session, ret);
}

/*
 * __session_drop --
 *	WT_SESSION->drop method.
 */
static int
__session_drop(WT_SESSION *wt_session, const char *uri, const char *config)
{
	WT_CONFIG_ITEM cval;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;
	SESSION_API_CALL(session, drop, config, cfg);

	WT_ERR(__wt_meta_track_on(session));

	/* If dropping snapshots, that's a different code path. */
	WT_ERR(__wt_config_gets(session, cfg, "snapshot", &cval));
	ret = (cval.len != 0) ?
	    __wt_schema_worker(
		session, uri, cfg, __wt_snapshot_drop, WT_BTREE_SNAPSHOT_OP) :
	    __wt_schema_drop(session, uri, cfg);

err:    WT_TRET(__wt_meta_track_off(session, ret != 0));
	API_END_NOTFOUND_MAP(session, ret);
}

/*
 * __session_dumpfile --
 *	WT_SESSION->dumpfile method.
 */
static int
__session_dumpfile(WT_SESSION *wt_session, const char *uri, const char *config)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;
	SESSION_API_CALL(session, dumpfile, config, cfg);
	ret = __wt_schema_worker(session, uri, cfg,
	    __wt_dumpfile, WT_BTREE_EXCLUSIVE | WT_BTREE_VERIFY);

err:	API_END_NOTFOUND_MAP(session, ret);
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
	ret = __wt_schema_worker(session, uri, cfg,
	    __wt_salvage, WT_BTREE_EXCLUSIVE | WT_BTREE_SALVAGE);

err:	API_END_NOTFOUND_MAP(session, ret);
}

/*
 * __session_sync --
 *	WT_SESSION->sync method.
 */
static int
__session_sync(WT_SESSION *wt_session, const char *uri, const char *config)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;

	SESSION_API_CALL(session, sync, config, cfg);
	WT_ERR(__wt_meta_track_on(session));

	ret = __wt_schema_worker(
	    session, uri, cfg, __wt_snapshot, WT_BTREE_SNAPSHOT_OP);

err:    WT_TRET(__wt_meta_track_off(session, ret != 0));
	API_END_NOTFOUND_MAP(session, ret);
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

	SESSION_API_CALL(session, truncate, config, cfg);
	/*
	 * If the URI is specified, we don't need a start/stop, if start/stop
	 * is specified, we don't need a URI.
	 *
	 * If no URI is specified, and both cursors are specified, start/stop
	 * must reference the same object.
	 *
	 * Any specified cursor must have been initialized.
	 */
	if ((uri == NULL && start == NULL && stop == NULL) ||
	    (uri != NULL && (start != NULL || stop != NULL)))
		WT_ERR_MSG(session, EINVAL,
		    "the truncate method should be passed either a URI or "
		    "start/stop cursors, but not both");
	if (start != NULL && stop != NULL && strcmp(start->uri, stop->uri) != 0)
		WT_ERR_MSG(session, EINVAL,
		    "truncate method cursors must reference the same object");
	if ((start != NULL && !F_ISSET(start, WT_CURSTD_KEY_SET)) ||
	    (stop != NULL && !F_ISSET(stop, WT_CURSTD_KEY_SET)))
		WT_ERR_MSG(session, EINVAL,
		    "the truncate method cursors must have their keys set");

	if (uri == NULL) {
		/*
		 * From a starting/stopping cursor to the begin/end of the
		 * object is easy, walk the object.
		 */
		if (start == NULL)
			for (;;) {
				WT_ERR(stop->remove(stop));
				if ((ret = stop->prev(stop)) != 0) {
					if (ret == WT_NOTFOUND)
						ret = 0;
					break;
				}
			}
		else
			for (;;) {
				WT_ERR(start->remove(start));
				if (stop != NULL &&
				    start->equals(start, stop))
					break;
				if ((ret = start->next(start)) != 0) {
					if (ret == WT_NOTFOUND)
						ret = 0;
					break;
				}
			}
	} else
		ret = __wt_schema_truncate(session, uri, cfg);

err:	API_END_NOTFOUND_MAP(session, ret);
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
	ret = __wt_schema_worker(session, uri, cfg,
	    __wt_upgrade, WT_BTREE_EXCLUSIVE | WT_BTREE_UPGRADE);

err:	API_END_NOTFOUND_MAP(session, ret);
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
	ret = __wt_schema_worker(session, uri, cfg,
	    __wt_verify, WT_BTREE_EXCLUSIVE | WT_BTREE_VERIFY);

err:	API_END_NOTFOUND_MAP(session, ret);
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
	if (!F_ISSET(S2C(session), WT_CONN_TRANSACTIONAL))
		WT_ERR_MSG(session, EINVAL,
		    "Database not configured for transactions");
	if (TAILQ_FIRST(&session->cursors) != NULL)
		WT_ERR_MSG(session, EINVAL, "Not permitted with open cursors");

	ret = __wt_txn_begin(session, cfg);

err:	API_END(session);
	return (ret);
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

	session = (WT_SESSION_IMPL *)wt_session;

	SESSION_API_CALL(session, commit_transaction, config, cfg);
	WT_TRET(__session_close_cursors(session));
	if (ret == 0)
		ret = __wt_txn_commit(session, cfg);
	else
		(void)__wt_txn_rollback(session, cfg);

err:	API_END(session);
	return (ret);
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
	WT_TRET(__session_close_cursors(session));
	WT_TRET(__wt_txn_rollback(session, cfg));

err:	API_END(session);
	return (ret);
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

	session = (WT_SESSION_IMPL *)wt_session;
	SESSION_API_CALL(session, checkpoint, config, cfg);
	WT_TRET(__wt_txn_checkpoint(session, cfg));

err:	API_END_NOTFOUND_MAP(session, ret);
}

/*
 * __session_msg_printf --
 *	WT_SESSION->msg_printf method.
 */
static int
__session_msg_printf(WT_SESSION *wt_session, const char *fmt, ...)
{
	WT_DECL_RET;
	va_list ap;

	va_start(ap, fmt);
	ret = __wt_vmsg((WT_SESSION_IMPL *)wt_session, fmt, ap);
	va_end(ap);

	return (ret);
}

/*
 * __wt_open_session --
 *	Allocate a session handle.  The internal parameter is used for sessions
 *	opened by WiredTiger for its own use.
 */
int
__wt_open_session(WT_CONNECTION_IMPL *conn, int internal,
    WT_EVENT_HANDLER *event_handler, const char *config,
    WT_SESSION_IMPL **sessionp)
{
	static WT_SESSION stds = {
		NULL,
		__session_close,
		__session_open_cursor,
		__session_create,
		__session_drop,
		__session_rename,
		__session_salvage,
		__session_sync,
		__session_truncate,
		__session_upgrade,
		__session_verify,
		__session_begin_transaction,
		__session_commit_transaction,
		__session_rollback_transaction,
		__session_checkpoint,
		__session_dumpfile,
		__session_msg_printf
	};
	WT_DECL_RET;
	WT_SESSION_IMPL *session, *session_ret;
	uint32_t i;

	WT_UNUSED(config);

	session = conn->default_session;
	session_ret = NULL;

	__wt_spin_lock(session, &conn->spinlock);

	/* Find the first inactive session slot. */
	for (session_ret = conn->sessions,
	    i = 0; i < conn->session_size; ++session_ret, ++i)
		if (!session_ret->active)
			break;
	if (i == conn->session_size)
		WT_ERR_MSG(session, WT_ERROR,
		    "only configured to support %d thread contexts",
		    conn->session_size);

	/*
	 * If the active session count is increasing, update it.  We don't worry
	 * about correcting the session count on error, as long as we don't mark
	 * this session as active, we'll clean it up on close.
	 */
	if (i >= conn->session_cnt)	/* Defend against off-by-one errors. */
		conn->session_cnt = i + 1;

	session_ret->id = i;
	session_ret->iface = stds;
	session_ret->iface.connection = &conn->iface;

	WT_ERR(__wt_cond_alloc(session, "session", 1, &session_ret->cond));

	__wt_event_handler_set(session_ret,
	    event_handler == NULL ? session->event_handler : event_handler);

	TAILQ_INIT(&session_ret->cursors);
	TAILQ_INIT(&session_ret->btrees);

	/* Initialize transaction support. */
	WT_ERR(__wt_txn_init(session_ret));

	/*
	 * The session's hazard reference memory isn't discarded during normal
	 * session close because access to it isn't serialized.  Allocate the
	 * first time we open this session.
	 */
	if (session_ret->hazard == NULL)
		WT_ERR(__wt_calloc(session, conn->hazard_size,
		    sizeof(WT_HAZARD), &session_ret->hazard));

	/*
	 * Public sessions are automatically closed during WT_CONNECTION->close.
	 * If the session handles for internal threads were to go on the public
	 * list, there would be complex ordering issues during close.  Set a
	 * flag to avoid this: internal sessions are not closed automatically.
	 */
	if (internal)
		F_SET(session_ret, WT_SESSION_INTERNAL);

	/*
	 * Publish: make the entry visible to server threads.  There must be a
	 * barrier for two reasons, to ensure structure fields are set before
	 * any other thread will consider the session, and to push the session
	 * count to ensure the eviction thread can't review too few slots.
	 */
	WT_PUBLISH(session_ret->active, 1);

	STATIC_ASSERT(offsetof(WT_SESSION_IMPL, iface) == 0);
	*sessionp = session_ret;

err:	__wt_spin_unlock(session, &conn->spinlock);
	return (ret);
}
