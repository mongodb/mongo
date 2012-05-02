/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_session_add_btree --
 *	Add a btree handle to the session's cache.
 */
int
__wt_session_add_btree(
    WT_SESSION_IMPL *session, WT_BTREE_SESSION **btree_sessionp)
{
	WT_BTREE_SESSION *btree_session;

	WT_RET(__wt_calloc_def(session, 1, &btree_session));
	btree_session->btree = session->btree;

	TAILQ_INSERT_HEAD(&session->btrees, btree_session, q);

	if (btree_sessionp != NULL)
		*btree_sessionp = btree_session;

	return (0);
}

/*
 * __wt_session_lock_btree --
 *	Lock a btree handle.
 */
int
__wt_session_lock_btree(
    WT_SESSION_IMPL *session, const char *cfg[], uint32_t flags)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	uint32_t open_flags;

	btree = session->btree;

	if (LF_ISSET(WT_BTREE_EXCLUSIVE)) {
		/*
		 * Try to get an exclusive handle lock and fail immediately if
		 * it unavailable.  We don't expect exclusive operations on
		 * trees to be mixed with ordinary cursor access, but if there
		 * is a use case in the future, we could make blocking here
		 * configurable.
		 */
		WT_RET(__wt_try_writelock(session, btree->rwlock));

		/*
		 * Reopen the handle for this operation to set any special
		 * flags.  For example, set WT_BTREE_BULK so the handle is
		 * closed correctly.
		 */
		open_flags = LF_ISSET(WT_BTREE_BULK |
		    WT_BTREE_SALVAGE | WT_BTREE_UPGRADE | WT_BTREE_VERIFY);
		if (open_flags != 0)
			ret = __wt_conn_btree_reopen(session, cfg, open_flags);
		F_SET(btree, WT_BTREE_EXCLUSIVE);
	} else if (!LF_ISSET(WT_BTREE_NO_LOCK))
		__wt_readlock(session, btree->rwlock);

	if (LF_ISSET(WT_BTREE_LOCK_ONLY) &&
	    WT_META_TRACKING(session))
		WT_TRET(__wt_meta_track_handle_lock(session));

	return (ret);
}

/*
 * __wt_session_release_btree --
 *	Unlock a btree handle.
 */
int
__wt_session_release_btree(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_DECL_RET;

	btree = session->btree;

	/*
	 * If we had exclusive access, reopen the tree without special flags so
	 * that other threads can use it (note the reopen call sets the flags).
	 */
	if (F_ISSET(btree, WT_BTREE_BULK |
	    WT_BTREE_SALVAGE | WT_BTREE_UPGRADE | WT_BTREE_VERIFY)) {
		WT_ASSERT(session, F_ISSET(btree, WT_BTREE_EXCLUSIVE));
		ret = __wt_conn_btree_reopen(session, NULL, 0);
	}

	if (F_ISSET(btree, WT_BTREE_EXCLUSIVE))
		F_CLR(btree, WT_BTREE_EXCLUSIVE);

	__wt_rwunlock(session, btree->rwlock);

	return (ret);
}

/*
 * __session_find_btree --
 *	Find an open btree handle for the named table.
 */
static int
__session_find_btree(WT_SESSION_IMPL *session,
    const char *uri, size_t urilen, const char *cfg[], uint32_t flags,
    WT_BTREE_SESSION **btree_sessionp)
{
	WT_BTREE *btree;
	WT_BTREE_SESSION *btree_session;

	TAILQ_FOREACH(btree_session, &session->btrees, q) {
		btree = btree_session->btree;
		if (strncmp(uri, btree->name, urilen) == 0 &&
		    btree->name[urilen] == '\0') {
			if (btree_sessionp != NULL)
				*btree_sessionp = btree_session;
			session->btree = btree;
			return (__wt_session_lock_btree(session, cfg, flags));
		}
	}

	return (WT_NOTFOUND);
}

/*
 * __wt_session_get_btree --
 *	Get a btree handle for the given name, set session->btree.
 */
int
__wt_session_get_btree(WT_SESSION_IMPL *session,
    const char *uri, const char *tconfig, const char *cfg[], uint32_t flags)
{
	WT_BTREE_SESSION *btree_session;
	WT_CONFIG_ITEM cval;
	WT_DECL_RET;
	WT_ITEM *buf;
	const char *filename, *name, *treeconf;
	size_t namelen;
	int exist;

	buf = NULL;
	treeconf = NULL;

	filename = uri;
	if (!WT_PREFIX_SKIP(filename, "file:"))
		WT_RET_MSG(
		    session, EINVAL, "Expected a 'file:' URI: %s", uri);

	/* Is this a snapshot operation? */
	if (!LF_ISSET(WT_BTREE_SNAPSHOT_OP) && cfg != NULL &&
	    __wt_config_gets(session, cfg, "snapshot", &cval) == 0 &&
	    cval.len != 0) {
		WT_RET(__wt_scr_alloc(session, 0, &buf));
		WT_ERR(__wt_buf_fmt(session, buf, "%s:%.*s",
		    uri, (int)cval.len, cval.str));
		name = buf->data;
		namelen = buf->size;
	} else {
		name = uri;
		namelen = strlen(uri);
	}

	if ((ret = __session_find_btree(session,
	    name, namelen, cfg, flags, &btree_session)) == 0) {
		WT_ASSERT(session, btree_session->btree != NULL);
		session->btree = btree_session->btree;
		goto err;
	}
	if (ret != WT_NOTFOUND)
		goto err;

	WT_ERR(__wt_exist(session, filename, &exist));
	if (!exist) {
		ret = WT_NOTFOUND;
		goto err;
	}

	/*
	 * A fixed configuration is passed in for special files, such as the
	 * metadata file itself.
	 */
	if (tconfig != NULL)
		WT_ERR(__wt_strdup(session, tconfig, &treeconf));
	else
		WT_ERR(__wt_metadata_read(session, uri, &treeconf));
	WT_ERR(__wt_conn_btree_open(session, name, treeconf, cfg, flags));
	WT_ERR(__wt_session_lock_btree(session, cfg, flags));
	WT_ERR(__wt_session_add_btree(session, NULL));

	if (0) {
err:		__wt_free(session, treeconf);
	}
	__wt_scr_free(&buf);
	return (ret);
}

/*
 * __wt_session_lock_snapshot --
 *	Lock the btree handle for the given snapshot name.
 */
int
__wt_session_lock_snapshot(
    WT_SESSION_IMPL *session, const char *snapshot, uint32_t flags)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_ITEM *buf;
	const char *cfg[] = { NULL, NULL };

	buf = NULL;
	btree = session->btree;

	WT_ERR(__wt_scr_alloc(session, 0, &buf));
	WT_ERR(__wt_buf_fmt(session, buf, "snapshot=\"%s\"", snapshot));
	cfg[0] = buf->data;

	LF_SET(WT_BTREE_LOCK_ONLY);
	WT_ERR(__wt_session_get_btree(session, btree->name, NULL, cfg, flags));

	/* Restore the original btree in the session. */
err:	session->btree = btree;
	__wt_scr_free(&buf);

	return (ret);
}

/*
 * __wt_session_remove_btree --
 *	Discard our reference to the btree.
 */
int
__wt_session_remove_btree(
    WT_SESSION_IMPL *session, WT_BTREE_SESSION *btree_session, int locked)
{
	TAILQ_REMOVE(&session->btrees, btree_session, q);
	session->btree = btree_session->btree;
	__wt_free(session, btree_session);

	return (__wt_conn_btree_close(session, locked));
}

/*
 * __wt_session_close_any_open_btree --
 *	If open, close the btree handle.
 */
int
__wt_session_close_any_open_btree(WT_SESSION_IMPL *session, const char *name)
{
	WT_BTREE_SESSION *btree_session;
	WT_DECL_RET;

	if ((ret = __session_find_btree(session, name, strlen(name),
	    NULL, WT_BTREE_EXCLUSIVE, &btree_session)) == 0) {
		/*
		 * XXX
		 * We have an exclusive lock, which means there are no cursors
		 * open but some other thread may have the handle cached.
		 * Fixing this will mean adding additional synchronization to
		 * the cursor open path.
		 */
		WT_ASSERT(session, btree_session->btree->refcnt == 1);
		__wt_schema_detach_tree(session, btree_session->btree);
		ret = __wt_session_remove_btree(session, btree_session, 1);
		__wt_rwunlock(session, session->btree->rwlock);
	} else if (ret == WT_NOTFOUND)
		ret = 0;

	return (ret);
}
