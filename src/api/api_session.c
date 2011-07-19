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

	__wt_lock(session, conn->mtx);
	/* Unpin the current per-WT_SESSION_IMPL buffer. */
	if (session->sb != NULL)
		__wt_sb_decrement(session, session->sb);

	/* Discard scratch buffers. */
	__wt_scr_free(session);

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

	/* Make the session array entry available for re-use. */
	session->iface.connection = NULL;
	WT_MEMORY_FLUSH;

	session = &conn->default_session;
	__wt_unlock(session, conn->mtx);
	API_END(session);

	return (0);
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
	/* Config parsing is done by each implementation. */
	WT_UNUSED(cfg);

	if (WT_PREFIX_MATCH(uri, "config:"))
		ret = __wt_curconfig_open(session, uri, config, cursorp);
	else if (WT_PREFIX_MATCH(uri, "file:"))
		ret = __wt_curfile_open(session, uri, config, cursorp);
	else if (WT_PREFIX_MATCH(uri, "index:"))
		ret = __wt_curindex_open(session, uri, config, cursorp);
	else if (WT_PREFIX_MATCH(uri, "stat:"))
		ret = __wt_curstat_open(session, uri, config, cursorp);
	else if (WT_PREFIX_MATCH(uri, "table:"))
		ret = __wt_curtable_open(session, uri, config, cursorp);
	else {
		__wt_errx(session, "Unknown cursor type '%s'", uri);
		ret = EINVAL;
	}

	API_END(session);
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
	WT_TRET(__wt_schema_create(session, name, config));
	API_END(session);
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

	session = (WT_SESSION_IMPL *)wt_session;

	SESSION_API_CALL(session, drop, config, cfg);

	WT_RET(__wt_config_gets(session, cfg, "force", &cval));
	force = (cval.val != 0);

	ret = __wt_schema_drop(session, name, config);
	API_END(session);

	return (force ? 0 : ret);
}

/*
 * __session_salvage --
 *	WT_SESSION->salvage method.
 */
static int
__session_salvage(WT_SESSION *wt_session, const char *name, const char *config)
{
	WT_SESSION_IMPL *session;
	const char *treeconf;
	int ret;

	session = (WT_SESSION_IMPL *)wt_session;
	ret = 0;

	SESSION_API_CALL(session, salvage, config, cfg);
	WT_UNUSED(cfg);

	if (!WT_PREFIX_SKIP(name, "file:")) {
		__wt_errx(session, "Unknown object type: %s", name);
		return (EINVAL);
	}

	/*
	 * Open a btree handle.
	 *
	 * Tell the eviction thread to ignore this handle, we'll manage our own
	 * pages.  Also tell open that we're going to salvage this handle, so
	 * it skips loading metadata such as the free list, which could be
	 * corrupted.
	 */
	WT_ERR(__wt_btconf_read(session, name, &treeconf));
	WT_ERR(__wt_btree_open(
	    session, name, treeconf, WT_BTREE_NO_EVICTION | WT_BTREE_SALVAGE));

	WT_TRET(__wt_salvage(session, config));

	/* Close the file and discard the WT_BTREE structure. */
	WT_TRET(__wt_btree_close(session));
err:	API_END(session);

	return (ret);
}

/*
 * __session_sync --
 *	WT_SESSION->sync method.
 */
static int
__session_sync(WT_SESSION *wt_session, const char *name, const char *config)
{
	WT_BTREE_SESSION *btree_session;
	WT_SESSION_IMPL *session;
	int ret;

	session = (WT_SESSION_IMPL *)wt_session;

	SESSION_API_CALL(session, sync, config, cfg);
	WT_UNUSED(cfg);

	if (!WT_PREFIX_SKIP(name, "file:")) {
		__wt_errx(session, "Unknown object type: %s", name);
		return (EINVAL);
	}

	ret = __wt_session_get_btree(session,
	    name, strlen(name), &btree_session);

	/* If the tree isn't open, there's nothing to do. */
	if (ret == WT_NOTFOUND)
		return (0);
	else if (ret != 0)
		return (ret);

	session->btree = btree_session->btree;
	ret = __wt_bt_sync(session);
	API_END(session);

	return (0);
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
__session_verify(WT_SESSION *wt_session, const char *name, const char *config)
{
	WT_SESSION_IMPL *session;
	const char *treeconf;
	int ret;

	session = (WT_SESSION_IMPL *)wt_session;
	ret = 0;

	SESSION_API_CALL(session, verify, config, cfg);
	WT_UNUSED(cfg);

	if (!WT_PREFIX_SKIP(name, "file:")) {
		__wt_errx(session, "Unknown object type: %s", name);
		return (EINVAL);
	}

	/*
	 * Open a btree handle.
	 *
	 * Tell the eviction thread to ignore this handle, we'll manage our own
	 * pages (it would be possible for us to let the eviction thread handle
	 * us, but there's no reason to do so, we can be more aggressive
	 * because we know what pages are no longer needed, regardless of LRU).
	 *
	 * Also tell open that we're going to verify this handle, so it skips
	 * loading metadata such as the free list, which could be corrupted.
	 */
	WT_ERR(__wt_btconf_read(session, name, &treeconf));
	WT_ERR(__wt_btree_open(
	    session, name, treeconf, WT_BTREE_NO_EVICTION | WT_BTREE_VERIFY));

	WT_TRET(__wt_verify(session, config));

	/* Close the file and discard the WT_BTREE structure. */
	WT_TRET(__wt_btree_close(session));
err:	API_END(session);

	return (ret);
}

/*
 * __session_dumpfile --
 *	WT_SESSION->dumpfile method.
 */
static int
__session_dumpfile(WT_SESSION *wt_session, const char *name, const char *config)
{
	WT_SESSION_IMPL *session;
	const char *treeconf;
	int ret;

	session = (WT_SESSION_IMPL *)wt_session;
	ret = 0;

	SESSION_API_CALL(session, dumpfile, config, cfg);
	(void)cfg;

	if (!WT_PREFIX_SKIP(name, "file:")) {
		__wt_errx(session, "Unknown object type: %s", name);
		return (EINVAL);
	}

	/*
	 * Open a btree handle.
	 *
	 * Tell the eviction thread to ignore this handle, we'll manage our own
	 * pages (it would be possible for us to let the eviction thread handle
	 * us, but there's no reason to do so, we can be more aggressive
	 * because we know what pages are no longer needed, regardless of LRU).
	 *
	 * Also tell open that we're going to verify this handle, so it skips
	 * loading metadata such as the free list, which could be corrupted.
	 */
	WT_RET(__wt_btconf_read(session, name, &treeconf));
	WT_RET(__wt_btree_open(
	    session, name, treeconf, WT_BTREE_NO_EVICTION | WT_BTREE_VERIFY));

	WT_TRET(__wt_dumpfile(session, config));

	/* Close the file and discard the WT_BTREE structure. */
	WT_TRET(__wt_btree_close(session));
	API_END(session);

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
 *	Allocate a session handle.
 */
int
__wt_open_session(WT_CONNECTION_IMPL *conn,
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

	WT_UNUSED(config);
	session = &conn->default_session;
	session_ret = NULL;

	/* Check to see if there's an available session slot. */
	if (conn->session_cnt == conn->session_size - 1) {
		__wt_err(session, 0,
		    "WiredTiger only configured to support %d thread contexts",
		    conn->session_size);
		return (WT_ERROR);
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

	/* We can't use the new session: it hasn't been configured yet. */
	WT_RET(__wt_mtx_alloc(session, "session", 1, &session_ret->mtx));

	session_ret->iface = stds;
	session_ret->iface.connection = &conn->iface;
	WT_ASSERT(session, session->event_handler != NULL);
	session_ret->event_handler = session->event_handler;
	session_ret->hazard = conn->hazard + slot * conn->hazard_size;

	/* Make the entry visible to the workQ. */
	conn->sessions[conn->session_cnt++] = session_ret;
	WT_MEMORY_FLUSH;

	TAILQ_INIT(&session_ret->cursors);
	TAILQ_INIT(&session_ret->btrees);
	if (event_handler != NULL)
		session_ret->event_handler = event_handler;

	STATIC_ASSERT(offsetof(WT_CONNECTION_IMPL, iface) == 0);
	*sessionp = session_ret;

	return (0);
}
