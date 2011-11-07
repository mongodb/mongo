/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
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

	btree = session->btree;
	WT_ASSERT(session, btree != NULL);

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
		 * Check if the handle needs to be reopened.  If the handle was
		 * just opened, cfg is NULL, so there is no need to reopen in
		 * that case.
		 *
		 * We do need to pick up the flags anyway, for example to set
		 * WT_BTREE_BULK so the handle is closed correctly.
		 */
		if (cfg != NULL && LF_ISSET(
		    WT_BTREE_BULK | WT_BTREE_SALVAGE | WT_BTREE_VERIFY))
			return (__wt_btree_reopen(session, cfg, flags));
		else
			F_SET(btree, flags);
	} else if (!LF_ISSET(WT_BTREE_NO_LOCK))
		__wt_readlock(session, btree->rwlock);

	return (0);
}

/*
 * __wt_session_unlock_btree --
 *	Unlock a btree handle.
 */
int
__wt_session_release_btree(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	int ret;

	btree = session->btree;
	WT_ASSERT(session, btree != NULL);
	ret = 0;

	/*
	 * If we had exclusive access, reopen the tree without special flags so
	 * that other threads can use it.
	 */
	if (F_ISSET(btree,
	    WT_BTREE_BULK | WT_BTREE_SALVAGE | WT_BTREE_VERIFY)) {
		WT_ASSERT(session, F_ISSET(btree, WT_BTREE_EXCLUSIVE));
		ret = __wt_btree_reopen(session, NULL, 0);
	} else if (F_ISSET(btree, WT_BTREE_EXCLUSIVE))
		F_CLR(btree, WT_BTREE_EXCLUSIVE);

	__wt_rwunlock(session, session->btree->rwlock);

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
	if (!WT_PREFIX_SKIP(filename, "file:")) {
		__wt_errx(session, "Expected a 'file:' URI: %s", fileuri);
		return (EINVAL);
	}

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
	WT_RET(__wt_btree_open(session, name, filename, treeconf, cfg, flags));
	WT_RET(__wt_session_lock_btree(session, NULL, flags));
	WT_RET(__wt_session_add_btree(session, NULL));

	return (0);
}

/*
 * __wt_session_remove_btree --
 *	Remove the btree handle from the session, closing if necessary.
 */
int
__wt_session_remove_btree(
    WT_SESSION_IMPL *session, WT_BTREE_SESSION *btree_session)
{
	TAILQ_REMOVE(&session->btrees, btree_session, q);
	session->btree = btree_session->btree;
	__wt_free(session, btree_session);

	return (__wt_btree_close(session));
}
