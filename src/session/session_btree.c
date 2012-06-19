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
	 * Special operation flags will cause the handle to be reopened.
	 * For example, a handle opened with WT_BTREE_BULK cannot use the same
	 * internal data structures as a handle opened for ordinary access.
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
		 * Special flags will cause the handle to be reopened, which
		 * will get the necessary lock, so don't bother here.
		 */
		if (LF_ISSET(WT_BTREE_LOCK_ONLY) || special_flags == 0) {
			WT_RET(__wt_try_writelock(session, btree->rwlock));
			F_SET(btree, WT_BTREE_EXCLUSIVE);
		}
	} else
		__wt_readlock(session, btree->rwlock);

	/*
	 * At this point, we have the requested lock -- if that is all that was
	 * required, we're done.  Otherwise, check that the handle is open and
	 * that no special flags are required.
	 */
	if (LF_ISSET(WT_BTREE_LOCK_ONLY) ||
	    (F_ISSET(btree, WT_BTREE_OPEN) && special_flags == 0))
		return (0);

	/*
	 * The handle needs to be opened.  If we locked the handle above,
	 * unlock it before returning.
	 */
	if (!LF_ISSET(WT_BTREE_EXCLUSIVE) || special_flags == 0)
		__wt_rwunlock(session, btree->rwlock);

	/* Treat an unopened handle just like a non-existent handle. */
	return (WT_NOTFOUND);
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

	/* If the tree is being created, it is already locked and tracked. */
	if (btree == session->created_btree)
		return (0);

	/*
	 * If we had special flags set, close the handle so that future access
	 * can get a handle without special flags.
	 */
	if (F_ISSET(btree, WT_BTREE_SPECIAL_FLAGS)) {
		WT_ASSERT(session, F_ISSET(btree, WT_BTREE_EXCLUSIVE));

		ret = __wt_conn_btree_sync_and_close(session);
	}

	if (F_ISSET(btree, WT_BTREE_EXCLUSIVE))
		F_CLR(btree, WT_BTREE_EXCLUSIVE);

	__wt_rwunlock(session, btree->rwlock);
	session->btree = NULL;

	return (ret);
}

/*
 * __wt_session_get_btree --
 *	Get a btree handle for the given name, set session->btree.
 */
int
__wt_session_get_btree(WT_SESSION_IMPL *session,
    const char *uri, const char *cfg[], uint32_t flags)
{
	WT_BTREE *btree;
	WT_BTREE_SESSION *btree_session;
	WT_CONFIG_ITEM cval;
	WT_DECL_RET;
	const char *ckpt;
	size_t ckptlen;

	btree = NULL;

	/*
	 * Optionally open a checkpoint.  This function is called from lots of
	 * places, for example, session.checkpoint: the only method currently
	 * with a "checkpoint" configuration string is session.open_cursor, so
	 * we don't need to check further than if that configuration string is
	 * set.
	 */
	if (cfg != NULL &&
	    __wt_config_gets(session, cfg, "checkpoint", &cval) == 0 &&
	    cval.len != 0) {
		ckpt = cval.str;
		ckptlen = cval.len;
	} else {
		ckpt = NULL;
		ckptlen = 0;
	}

	TAILQ_FOREACH(btree_session, &session->btrees, q) {
		btree = btree_session->btree;
		if (strcmp(uri, btree->name) != 0)
			continue;
		if ((ckpt == NULL && btree->checkpoint == NULL) ||
		    (ckpt != NULL && btree->checkpoint != NULL &&
		    (strncmp(ckpt, btree->checkpoint, ckptlen) == 0 &&
		    btree->checkpoint[ckptlen] == '\0')))
			break;
	}

	if (btree_session == NULL)
		session->btree = NULL;
	else {
		session->btree = btree;
		/*
		 * If the tree is being created, it is already locked and
		 * tracked.
		 */
		if (btree == session->created_btree)
			return (0);

		if ((ret =
		    __wt_session_lock_btree(session, flags)) != WT_NOTFOUND) {
			WT_ASSERT(session, ret != 0 ||
			    LF_ISSET(WT_BTREE_EXCLUSIVE) ==
			    F_ISSET(session->btree, WT_BTREE_EXCLUSIVE));
			return (ret);
		}
		ret = 0;
	}

	WT_RET(__wt_conn_btree_get(session, uri, ckpt, cfg, flags));

	if (btree_session == NULL)
		WT_RET(__wt_session_add_btree(session, NULL));

	WT_ASSERT(session, LF_ISSET(WT_BTREE_LOCK_ONLY) ||
	    F_ISSET(session->btree, WT_BTREE_OPEN));
	WT_ASSERT(session, LF_ISSET(WT_BTREE_EXCLUSIVE) ==
	    F_ISSET(session->btree, WT_BTREE_EXCLUSIVE));

	return (0);
}

/*
 * __wt_session_lock_checkpoint --
 *	Lock the btree handle for the given checkpoint name.
 */
int
__wt_session_lock_checkpoint(
    WT_SESSION_IMPL *session, const char *checkpoint, uint32_t flags)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_ITEM *buf;
	const char *cfg[] = { NULL, NULL };

	buf = NULL;
	btree = session->btree;

	WT_ERR(__wt_scr_alloc(session, 0, &buf));
	WT_ERR(__wt_buf_fmt(session, buf, "checkpoint=\"%s\"", checkpoint));
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
    WT_SESSION_IMPL *session, WT_BTREE_SESSION *btree_session)
{
	TAILQ_REMOVE(&session->btrees, btree_session, q);

	session->btree = btree_session->btree;
	__wt_overwrite_and_free(session, btree_session);

	return (__wt_conn_btree_close(session, 0));
}
