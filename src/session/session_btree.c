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
	uint32_t open_flags;
	int ret;

	btree = session->btree;
	ret = 0;

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
	int ret;

	btree = session->btree;
	ret = 0;

	/*
	 * If we had exclusive access, close the handle so that other threads
	 * can use it (the next thread to access the handle will open it with
	 * any special flags as required.  The handle stays in our cache, so
	 * we don't want to go through __wt_conn_btree_close.
	 */
	if (F_ISSET(btree, WT_BTREE_BULK |
	    WT_BTREE_SALVAGE | WT_BTREE_UPGRADE | WT_BTREE_VERIFY)) {
		WT_ASSERT(session, F_ISSET(btree, WT_BTREE_EXCLUSIVE));
		ret = __wt_conn_btree_reopen(session, NULL, 0);
	}

	F_CLR(btree, WT_BTREE_EXCLUSIVE);
	__wt_rwunlock(session, btree->rwlock);

	return (ret);
}

/*
 * __wt_session_find_btree --
 *	Find an open btree handle for the named table.
 */
int
__wt_session_find_btree(WT_SESSION_IMPL *session,
    const char *filename, size_t namelen, const char *cfg[], uint32_t flags,
    WT_BTREE_SESSION **btree_sessionp)
{
	WT_BTREE *btree;
	WT_BTREE_SESSION *btree_session;

	TAILQ_FOREACH(btree_session, &session->btrees, q) {
		btree = btree_session->btree;
		if (strncmp(filename, btree->filename, namelen) == 0 &&
		    btree->filename[namelen] == '\0') {
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
    const char *name, const char *fileuri, const char *tconfig,
    const char *cfg[], uint32_t flags)
{
	WT_BTREE_SESSION *btree_session;
	const char *filename, *treeconf;
	int exist, ret;

	filename = fileuri;
	if (!WT_PREFIX_SKIP(filename, "file:"))
		WT_RET_MSG(
		    session, EINVAL, "Expected a 'file:' URI: %s", fileuri);

	if ((ret = __wt_session_find_btree(session,
	    filename, strlen(filename), cfg, flags, &btree_session)) == 0) {
		WT_ASSERT(session, btree_session->btree != NULL);
		session->btree = btree_session->btree;
		return (0);
	}
	if (ret != WT_NOTFOUND)
		return (ret);

	WT_RET(__wt_exist(session, filename, &exist));
	if (!exist)
		return (WT_NOTFOUND);

	/*
	 * A fixed configuration is passed in for special files, such
	 * as the schema table itself.
	 */
	if (tconfig != NULL)
		WT_RET(__wt_strdup(session, tconfig, &treeconf));
	else
		WT_RET(__wt_schema_table_read(session, fileuri, &treeconf));
	WT_RET(__wt_conn_btree_open(
	    session, name, filename, treeconf, cfg, flags));
	WT_RET(__wt_session_lock_btree(session, cfg, flags));
	WT_RET(__wt_session_add_btree(session, NULL));

	return (0);
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
	int ret;

	if ((ret = __wt_session_find_btree(session, name, strlen(name),
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
