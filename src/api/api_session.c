/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __session_close --
 *	WT_SESSION->close method.
 */
static int
__session_close(WT_SESSION *wt_session, const char *config)
{
	WT_BTREE_SESSION *btree_session;
	WT_CONNECTION_IMPL *conn;
	WT_SESSION_IMPL *session, **tp;
	WT_CURSOR *cursor;
	int ret;

	conn = (WT_CONNECTION_IMPL *)wt_session->connection;
	session = (WT_SESSION_IMPL *)wt_session;
	ret = 0;

	SESSION_API_CALL(session, close, config, cfg);
	WT_UNUSED(cfg);

	while ((cursor = TAILQ_FIRST(&session->cursors)) != NULL)
		WT_TRET(cursor->close(cursor, config));

	while ((btree_session = TAILQ_FIRST(&session->btrees)) != NULL)
		WT_TRET(__wt_session_remove_btree(session, btree_session));

	WT_TRET(__wt_schema_close_tables(session));

	__wt_lock(session, conn->mtx);
	/* Unpin the current per-WT_SESSION_IMPL buffer. */
	if (session->sb != NULL)
		__wt_sb_decrement(session, session->sb);

	/* Discard scratch buffers. */
	__wt_scr_discard(session);

	/* Confirm we're not holding any hazard references. */
	__wt_hazard_empty(session);

	/* Unlock and destroy the thread's mutex. */
	if (session->mtx != NULL) {
		__wt_unlock(session, session->mtx);
		(void)__wt_mtx_destroy(session, session->mtx);
	}

	/*
	 * Replace the session reference we're closing with the last entry in
	 * the table, then clear the last entry.  As far as the walk of the
	 * workQ is concerned, it's OK if the session appears twice, or if it
	 * doesn't appear at all, so these lines can race all they want.
	 */
	for (tp = conn->sessions; *tp != session; ++tp)
		;
	--conn->session_cnt;
	*tp = conn->sessions[conn->session_cnt];
	conn->sessions[conn->session_cnt] = NULL;

	/*
	 * Publish, making the session array entry available for re-use.  There
	 * must be a barrier here to ensure the cleanup above completes before
	 * the entry is re-used.
	 */
	WT_PUBLISH(session->iface.connection, NULL);

	session = &conn->default_session;
	__wt_unlock(session, conn->mtx);
err:	API_END(session);

	return (ret);
}

/*
 * __session_open_cursor --
 *	WT_SESSION->open_cursor method.
 */
static int
__session_open_cursor(WT_SESSION *wt_session,
    const char *uri, WT_CURSOR *to_dup, const char *config, WT_CURSOR **cursorp)
{
	WT_SESSION_IMPL *session;
	int ret;

	WT_UNUSED(to_dup);

	session = (WT_SESSION_IMPL *)wt_session;
	SESSION_API_CALL(session, open_cursor, config, cfg);

	if (WT_PREFIX_MATCH(uri, "colgroup:"))
		ret = __wt_curfile_open(session, uri, cfg, cursorp);
	else if (WT_PREFIX_MATCH(uri, "config:"))
		ret = __wt_curconfig_open(session, uri, cfg, cursorp);
	else if (WT_PREFIX_MATCH(uri, "file:"))
		ret = __wt_curfile_open(session, uri, cfg, cursorp);
	else if (WT_PREFIX_MATCH(uri, "index:"))
		ret = __wt_curindex_open(session, uri, cfg, cursorp);
	else if (WT_PREFIX_MATCH(uri, "statistics:"))
		ret = __wt_curstat_open(session, uri, cfg, cursorp);
	else if (WT_PREFIX_MATCH(uri, "table:"))
		ret = __wt_curtable_open(session, uri, cfg, cursorp);
	else {
		__wt_errx(session, "Unknown cursor type '%s'", uri);
		ret = EINVAL;
	}

err:	API_END(session);
	return (ret);
}

/*
 * __session_create --
 *	WT_SESSION->create method.
 */
static int
__session_create(WT_SESSION *wt_session, const char *name, const char *config)
{
	WT_SESSION_IMPL *session;
	int ret;

	session = (WT_SESSION_IMPL *)wt_session;
	ret = 0;
	SESSION_API_CALL(session, create, config, cfg);
	WT_UNUSED(cfg);
	WT_ERR(__wt_schema_create(session, name, config));
err:	API_END(session);
	return (ret);
}

/*
 * __session_rename --
 *	WT_SESSION->rename method.
 */
static int
__session_rename(WT_SESSION *wt_session,
    const char *oldname, const char *newname, const char *config)
{
	WT_UNUSED(wt_session);
	WT_UNUSED(oldname);
	WT_UNUSED(newname);
	WT_UNUSED(config);

	return (ENOTSUP);
}

/*
 * __session_drop --
 *	WT_SESSION->drop method.
 */
static int
__session_drop(
    WT_SESSION *wt_session, const char *name, const char *config)
{
	WT_SESSION_IMPL *session;
	WT_CONFIG_ITEM cval;
	int force, ret;

	force = 0;

	session = (WT_SESSION_IMPL *)wt_session;

	SESSION_API_CALL(session, drop, config, cfg);

	WT_ERR(__wt_config_gets(session, cfg, "force", &cval));
	force = (cval.val != 0);

	ret = __wt_schema_drop(session, name, config);
err:	API_END(session);

	return (force ? 0 : ret);
}

/*
 * __session_salvage --
 *	WT_SESSION->salvage method.
 */
static int
__session_salvage(WT_SESSION *wt_session, const char *uri, const char *config)
{
	WT_SESSION_IMPL *session;
	int ret;

	session = (WT_SESSION_IMPL *)wt_session;
	ret = 0;

	SESSION_API_CALL(session, salvage, config, cfg);
	if (!WT_PREFIX_MATCH(uri, "file:")) {
		__wt_errx(session, "Unknown object type: %s", uri);
		ret = EINVAL;
		goto err;
	}

	/*
	 * Open a btree handle.
	 *
	 * Tell the eviction thread to ignore this handle, we'll manage our own
	 * pages.  Also tell open that we're going to salvage this handle, so
	 * it skips loading metadata such as the free list, which could be
	 * corrupted.
	 */
	WT_ERR(__wt_session_get_btree(session, uri, uri, NULL, cfg,
	    WT_BTREE_EXCLUSIVE | WT_BTREE_NO_EVICTION | WT_BTREE_SALVAGE));

	WT_TRET(__wt_salvage(session, config));

	WT_TRET(__wt_session_release_btree(session));
err:	API_END(session);

	return (ret);
}

/*
 * __session_sync --
 *	WT_SESSION->sync method.
 */
static int
__session_sync(WT_SESSION *wt_session, const char *uri, const char *config)
{
	WT_BTREE_SESSION *btree_session;
	WT_SESSION_IMPL *session;
	const char *filename;
	int ret;

	session = (WT_SESSION_IMPL *)wt_session;

	SESSION_API_CALL(session, sync, config, cfg);
	WT_UNUSED(cfg);

	filename = uri;
	if (!WT_PREFIX_SKIP(filename, "file:")) {
		__wt_errx(session, "Unknown object type: %s", uri);
		ret = EINVAL;
		goto err;
	}

	ret = __wt_session_find_btree(session, filename,
	    strlen(filename), cfg, WT_BTREE_EXCLUSIVE, &btree_session);

	/* If the tree isn't open, there's nothing to do. */
	if (ret == 0) {
		session->btree = btree_session->btree;
		ret = __wt_btree_sync(session);

		/* Release the tree so other threads can use it. */
		WT_TRET(__wt_session_release_btree(session));
	}

err:	API_END(session);

	return (ret);
}

/*
 * __session_truncate --
 *	WT_SESSION->truncate method.
 */
static int
__session_truncate(WT_SESSION *wt_session,
    const char *name, WT_CURSOR *start, WT_CURSOR *end, const char *config)
{
	WT_UNUSED(wt_session);
	WT_UNUSED(name);
	WT_UNUSED(start);
	WT_UNUSED(end);
	WT_UNUSED(config);

	return (ENOTSUP);
}

/*
 * __session_verify --
 *	WT_SESSION->verify method.
 */
static int
__session_verify(WT_SESSION *wt_session, const char *uri, const char *config)
{
	WT_SESSION_IMPL *session;
	int ret;

	session = (WT_SESSION_IMPL *)wt_session;
	ret = 0;

	SESSION_API_CALL(session, verify, config, cfg);
	if (!WT_PREFIX_MATCH(uri, "file:")) {
		__wt_errx(session, "Unknown object type: %s", uri);
		ret = EINVAL;
		goto err;
	}

	/*
	 * Get a btree handle.
	 *
	 * Tell open that we're going to verify this handle, so it skips loading
	 * metadata such as the free list, which could be corrupted.
	 */
	WT_ERR(__wt_session_get_btree(session, uri, uri, NULL, cfg,
	    WT_BTREE_EXCLUSIVE | WT_BTREE_VERIFY));

	WT_TRET(__wt_verify(session, config));

	WT_TRET(__wt_session_release_btree(session));
err:	API_END(session);
	return (ret);
}

/*
 * __session_dumpfile --
 *	WT_SESSION->dumpfile method.
 */
static int
__session_dumpfile(WT_SESSION *wt_session, const char *uri, const char *config)
{
	WT_SESSION_IMPL *session;
	int ret;

	session = (WT_SESSION_IMPL *)wt_session;
	ret = 0;

	SESSION_API_CALL(session, dumpfile, config, cfg);
	if (!WT_PREFIX_MATCH(uri, "file:")) {
		__wt_errx(session, "Unknown object type: %s", uri);
		ret = EINVAL;
		goto err;
	}

	/*
	 * Get a btree handle.
	 *
	 * Tell the eviction thread to ignore this handle, we'll manage our own
	 * pages (it would be possible for us to let the eviction thread handle
	 * us, but there's no reason to do so, we can be more aggressive
	 * because we know what pages are no longer needed, regardless of LRU).
	 *
	 * Also tell open that we're going to verify this handle, so it skips
	 * loading metadata such as the free list, which could be corrupted.
	 */
	WT_ERR(__wt_session_get_btree(session, uri, uri, NULL, cfg,
	    WT_BTREE_EXCLUSIVE | WT_BTREE_NO_EVICTION | WT_BTREE_VERIFY));

	WT_TRET(__wt_dumpfile(session, config));

	WT_TRET(__wt_session_release_btree(session));
err:	API_END(session);
	return (ret);
}

/*
 * __session_begin_transaction --
 *	WT_SESSION->begin_transaction method.
 */
static int
__session_begin_transaction(WT_SESSION *wt_session, const char *config)
{
	WT_UNUSED(wt_session);
	WT_UNUSED(config);

	return (ENOTSUP);
}

/*
 * __session_commit_transaction --
 *	WT_SESSION->commit_transaction method.
 */
static int
__session_commit_transaction(WT_SESSION *wt_session, const char *config)
{
	WT_UNUSED(wt_session);
	WT_UNUSED(config);

	return (ENOTSUP);
}

/*
 * __session_rollback_transaction --
 *	WT_SESSION->rollback_transaction method.
 */
static int
__session_rollback_transaction(WT_SESSION *wt_session, const char *config)
{
	WT_UNUSED(wt_session);
	WT_UNUSED(config);

	return (ENOTSUP);
}

/*
 * __session_checkpoint --
 *	WT_SESSION->checkpoint method.
 */
static int
__session_checkpoint(WT_SESSION *wt_session, const char *config)
{
	WT_UNUSED(wt_session);
	WT_UNUSED(config);

	return (ENOTSUP);
}

/*
 * __session_msg_printf --
 *	WT_SESSION->msg_printf method.
 */
static int
__session_msg_printf(WT_SESSION *wt_session, const char *fmt, ...)
{
	WT_SESSION_IMPL *session;
	va_list ap;

	session = (WT_SESSION_IMPL *)wt_session;

	va_start(ap, fmt);
	__wt_msgv(session, fmt, ap);
	va_end(ap);

	return (0);
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
		__session_verify,
		__session_begin_transaction,
		__session_commit_transaction,
		__session_rollback_transaction,
		__session_checkpoint,
		__session_dumpfile,
		__session_msg_printf
	};
	WT_SESSION_IMPL *session, *session_ret;
	uint32_t slot;
	int ret;

	WT_UNUSED(config);
	ret = 0;
	session = &conn->default_session;
	session_ret = NULL;

	__wt_lock(session, conn->mtx);

	/* Check to see if there's an available session slot. */
	if (conn->session_cnt == conn->session_size - 1) {
		__wt_errx(session,
		    "WiredTiger only configured to support %d thread contexts",
		    conn->session_size);
		ret = WT_ERROR;
		goto err;
	}

	/* Check for multiple sessions without multithread support. */
	if (!internal) {
		if (!F_ISSET(conn, WT_MULTITHREAD) &&
		    conn->app_session_cnt > 0) {
			__wt_errx(session,
			    "wiredtiger_open not configured with 'multithread':"
			    " only a single session is permitted");
			ret = WT_ERROR;
			goto err;
		}

		++conn->app_session_cnt;
	}

	/*
	 * The session reference list is compact, the session array is not.
	 * Find the first empty session slot.
	 */
	for (slot = 0, session_ret = conn->session_array;
	    session_ret->iface.connection != NULL;
	    ++session_ret, ++slot)
		;

	/* Session entries are re-used, clear the old contents. */
	WT_CLEAR(*session_ret);

	WT_ERR(__wt_mtx_alloc(session, "session", 1, &session_ret->mtx));
	session_ret->iface = stds;
	session_ret->iface.connection = &conn->iface;
	WT_ASSERT(session, session->event_handler != NULL);
	session_ret->event_handler = session->event_handler;
	session_ret->hazard = conn->hazard + slot * conn->hazard_size;

	TAILQ_INIT(&session_ret->cursors);
	TAILQ_INIT(&session_ret->btrees);
	if (event_handler != NULL)
		session_ret->event_handler = event_handler;

	/*
	 * Public sessions are automatically closed during WT_CONNECTION->close.
	 * If the session handles for internal threads were to go on the public
	 * list, there would be complex ordering issues during close.  Set a
	 * flag to avoid this: internal sessions are not closed automatically.
	 */
	if (internal)
		F_SET(session_ret, WT_SESSION_INTERNAL);

	/*
	 * Publish: make the entry visible to the workQ.  There must be a
	 * barrier to ensure the structure fields are set before any other
	 * thread can see the session.
	 */
	WT_PUBLISH(conn->sessions[conn->session_cnt++], session_ret);

	STATIC_ASSERT(offsetof(WT_CONNECTION_IMPL, iface) == 0);
	*sessionp = session_ret;

err:	__wt_unlock(session, conn->mtx);
	return (ret);
}
