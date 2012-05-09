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
__wt_session_lock_btree(WT_SESSION_IMPL *session, uint32_t flags)
{
	WT_BTREE *btree;
	uint32_t special_flags;

	btree = session->btree;

	/*
	 * Reopen the handle for this operation to set any special flags.
	 * For example, set WT_BTREE_BULK so the handle is closed correctly.
	 */
	special_flags = LF_ISSET(WT_BTREE_SPECIAL_FLAGS);
	WT_ASSERT(session, special_flags == 0 || LF_ISSET(WT_BTREE_EXCLUSIVE));

	if (LF_ISSET(WT_BTREE_EXCLUSIVE)) {
		/*
		 * Try to get an exclusive handle lock and fail immediately if
		 * it unavailable.  We don't expect exclusive operations on
		 * trees to be mixed with ordinary cursor access, but if there
		 * is a use case in the future, we could make blocking here
		 * configurable.
		 *
		 * Special operations will trigger a reopen, which will get
		 * the necessary lock, so don't bother here.
		 */
		if (LF_ISSET(WT_BTREE_LOCK_ONLY) || special_flags == 0) {
			WT_RET(__wt_try_writelock(session, btree->rwlock));
			F_SET(btree, WT_BTREE_EXCLUSIVE);
		}
	} else
		__wt_readlock(session, btree->rwlock);

	/*
	 * At this point, we have the requested lock.  Now check that the
	 * handle is open with the requested flags.
	 */
	if (!LF_ISSET(WT_BTREE_LOCK_ONLY) &&
	    (!F_ISSET(btree, WT_BTREE_OPEN) || special_flags != 0)) {
		if (!LF_ISSET(WT_BTREE_EXCLUSIVE))
			__wt_rwunlock(session, btree->rwlock);

		/* Treat an unopened handle just like a non-existent handle. */
		return (WT_NOTFOUND);
	}

	return (0);
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
	 * If we had special flags set, close the handle so that future access
	 * can get a handle without special flags.
	 */
	if (F_ISSET(btree, WT_BTREE_SPECIAL_FLAGS)) {
		WT_ASSERT(session, F_ISSET(btree, WT_BTREE_EXCLUSIVE));
		ret = __wt_btree_close(session);
		F_CLR(btree, WT_BTREE_OPEN | WT_BTREE_SPECIAL_FLAGS);
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
    const char *uri, size_t urilen, const char *snapshot, size_t snaplen,
    uint32_t flags, WT_BTREE_SESSION **btree_sessionp)
{
	WT_BTREE *btree;
	WT_BTREE_SESSION *btree_session;

	TAILQ_FOREACH(btree_session, &session->btrees, q) {
		btree = btree_session->btree;
		if (strncmp(uri, btree->name, urilen) != 0 ||
		    btree->name[urilen] != '\0')
			continue;
		if (snapshot != btree->snapshot &&
		    (snapshot == NULL || btree->snapshot == NULL ||
		    strncmp(snapshot, btree->snapshot, snaplen) != 0 ||
		    btree->snapshot[snaplen] != '\0'))
			continue;
		if (btree_sessionp != NULL)
			*btree_sessionp = btree_session;
		session->btree = btree;
		return (__wt_session_lock_btree(session, flags));
	}

	*btree_sessionp = NULL;
	return (WT_NOTFOUND);
}

/*
 * __wt_session_get_btree --
 *	Get a btree handle for the given name, set session->btree.
 */
int
__wt_session_get_btree(WT_SESSION_IMPL *session,
    const char *uri, const char *cfg[], uint32_t flags)
{
	WT_BTREE_SESSION *btree_session;
	WT_CONFIG_ITEM cval;
	WT_DECL_RET;
	const char *filename, *name, *snapshot, *treeconf;
	size_t namelen, snaplen;
	int exist;

	treeconf = NULL;

	filename = uri;
	if (!WT_PREFIX_SKIP(filename, "file:"))
		WT_RET_MSG(
		    session, EINVAL, "Expected a 'file:' URI: %s", uri);

	name = uri;
	namelen = strlen(uri);

	/* Is this a snapshot operation? */
	if (!LF_ISSET(WT_BTREE_SNAPSHOT_OP) && cfg != NULL &&
	    __wt_config_gets(session, cfg, "snapshot", &cval) == 0 &&
	    cval.len != 0) {
		snapshot = cval.str;
		snaplen = cval.len;
	} else {
		snapshot = NULL;
		snaplen = 0;
	}

	if ((ret = __session_find_btree(session, name, namelen,
	    snapshot, snaplen, flags, &btree_session)) != WT_NOTFOUND) {
		if (ret == 0 && LF_ISSET(WT_BTREE_NO_LOCK))
			ret = __wt_session_release_btree(session);
		return (ret);
	}

	WT_RET(__wt_exist(session, filename, &exist));
	if (!exist)
		return (ENOENT);

	if (LF_ISSET(WT_BTREE_LOCK_ONLY) && btree_session != NULL)
		return (0);

	WT_RET(__wt_metadata_read(session, uri, &treeconf));
	if (btree_session == NULL) {
		WT_RET(__wt_conn_btree_get(
		    session, name, snapshot, treeconf, cfg, flags));
		WT_RET(__wt_session_add_btree(session, NULL));
	} else {
		WT_RET(__wt_conn_btree_open_lock(session, flags));
		ret = __wt_conn_btree_open(session, treeconf, cfg, flags);
		if (ret != 0 || LF_ISSET(WT_BTREE_NO_LOCK))
			__wt_rwunlock(session, session->btree->rwlock);
		WT_RET(ret);
	}

	return (0);
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
	WT_ERR(__wt_session_get_btree(session, btree->name, cfg, flags));

	WT_ASSERT(session, WT_META_TRACKING(session));
	WT_ERR(__wt_meta_track_handle_lock(session));

	/* Restore the original btree in the session. */
err:	session->btree = btree;
	__wt_scr_free(&buf);

	return (ret);
}

/*
 * __wt_session_discard_btree --
 *	Discard our reference to the btree.
 */
int
__wt_session_discard_btree(
    WT_SESSION_IMPL *session, WT_BTREE_SESSION *btree_session, int locked)
{
	TAILQ_REMOVE(&session->btrees, btree_session, q);
	session->btree = btree_session->btree;
	__wt_free(session, btree_session);

	return (__wt_conn_btree_close(session, locked));
}
