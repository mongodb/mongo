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
	if (!LF_ISSET(WT_BTREE_EXCLUSIVE) || special_flags == 0) {
		F_CLR(btree, WT_BTREE_EXCLUSIVE);
		__wt_rwunlock(session, btree->rwlock);
	}

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
	if (F_ISSET(btree, WT_BTREE_DISCARD | WT_BTREE_SPECIAL_FLAGS)) {
		WT_ASSERT(session, F_ISSET(btree, WT_BTREE_EXCLUSIVE));

		WT_WITH_SCHEMA_LOCK(session,
		    ret = __wt_conn_btree_sync_and_close(session));
		F_CLR(btree, WT_BTREE_DISCARD);
	}

	if (F_ISSET(btree, WT_BTREE_EXCLUSIVE))
		F_CLR(btree, WT_BTREE_EXCLUSIVE);

	__wt_rwunlock(session, btree->rwlock);
	session->btree = NULL;

	return (ret);
}

/*
 * __wt_session_get_btree_ckpt --
 *	Check the configuration strings for a checkpoint name, get a btree
 * handle for the given name, set session->btree.
 */
int
__wt_session_get_btree_ckpt(WT_SESSION_IMPL *session,
    const char *uri, const char *cfg[], uint32_t flags)
{
	WT_CONFIG_ITEM cval;
	WT_DECL_RET;
	int last_ckpt;
	const char *checkpoint;

	last_ckpt = 0;
	checkpoint = NULL;

	/*
	 * This function exists to handle checkpoint configuration.  Callers
	 * that never open a checkpoint call the underlying function directly.
	 */
	WT_RET_NOTFOUND_OK(
	    __wt_config_gets_defno(session, cfg, "checkpoint", &cval));
	if (cval.len != 0) {
		/*
		 * The internal checkpoint name is special, find the last
		 * unnamed checkpoint of the object.
		 */
		if (WT_STRING_MATCH(WT_CHECKPOINT, cval.str, cval.len)) {
			last_ckpt = 1;
retry:			WT_RET(__wt_meta_checkpoint_last_name(
			    session, uri, &checkpoint));
		} else
			WT_RET(__wt_strndup(
			    session, cval.str, cval.len, &checkpoint));
	}

	ret = __wt_session_get_btree(session, uri, checkpoint, cfg, flags);

	__wt_free(session, checkpoint);

	/*
	 * There's a potential race: we get the name of the most recent unnamed
	 * checkpoint, but if it's discarded (or locked so it can be discarded)
	 * by the time we try to open it, we'll fail the open.  Retry in those
	 * cases, a new "last" checkpoint should surface, and we can't return an
	 * error, the application will be justifiably upset if we can't open the
	 * last checkpoint instance of an object.
	 *
	 * The check against WT_NOTFOUND is correct: if there was no checkpoint
	 * for the object (that is, the object has never been in a checkpoint),
	 * we returned immediately after the call to search for that name.
	 */
	if (last_ckpt && (ret == WT_NOTFOUND || ret == EBUSY))
		goto retry;
	return (ret);
}

/*
 * __wt_session_get_btree --
 *	Get a btree handle for the given name, set session->btree.
 */
int
__wt_session_get_btree(WT_SESSION_IMPL *session,
    const char *uri, const char *checkpoint, const char *cfg[], uint32_t flags)
{
	WT_BTREE *btree;
	WT_BTREE_SESSION *btree_session;
	WT_DECL_RET;
	int needlock;

	btree = NULL;

	TAILQ_FOREACH(btree_session, &session->btrees, q) {
		btree = btree_session->btree;
		if (strcmp(uri, btree->name) != 0)
			continue;
		if ((checkpoint == NULL && btree->checkpoint == NULL) ||
		    (checkpoint != NULL && btree->checkpoint != NULL &&
		    strcmp(checkpoint, btree->checkpoint) == 0))
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

		/*
		 * Try and lock the file; if we succeed, our "exclusive" state
		 * must match.
		 */
		if ((ret =
		    __wt_session_lock_btree(session, flags)) != WT_NOTFOUND) {
			WT_ASSERT(session, ret != 0 ||
			    LF_ISSET(WT_BTREE_EXCLUSIVE) ==
			    F_ISSET(session->btree, WT_BTREE_EXCLUSIVE));
			return (ret);
		}
		ret = 0;
	}

	/*
	 * If we don't already hold the schema lock, get it now so that we
	 * can find and/or open the handle.
	 */
	needlock = !F_ISSET(session, WT_SESSION_SCHEMA_LOCKED);
	if (needlock) {
		__wt_spin_lock(session, &S2C(session)->schema_lock);
		F_SET(session, WT_SESSION_SCHEMA_LOCKED);
	}
	ret = __wt_conn_btree_get(session, uri, checkpoint, cfg, flags);
	if (needlock) {
		F_CLR(session, WT_SESSION_SCHEMA_LOCKED);
		__wt_spin_unlock(session, &S2C(session)->schema_lock);
	}
	WT_RET(ret);

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
__wt_session_lock_checkpoint(WT_SESSION_IMPL *session, const char *checkpoint)
{
	WT_BTREE *btree;
	WT_DECL_RET;

	btree = session->btree;

	WT_ERR(__wt_session_get_btree(session, btree->name,
	    checkpoint, NULL, WT_BTREE_EXCLUSIVE | WT_BTREE_LOCK_ONLY));

	/*
	 * We lock checkpoint handles that we are overwriting, so the handle
	 * must be closed when we release it.
	 */
	F_SET(session->btree, WT_BTREE_DISCARD);

	WT_ASSERT(session, WT_META_TRACKING(session));
	WT_ERR(__wt_meta_track_handle_lock(session));

	/* Restore the original btree in the session. */
err:	session->btree = btree;

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
