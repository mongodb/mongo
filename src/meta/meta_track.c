/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * WT_META_TRACK -- A tracked metadata operation: a non-transactional log,
 * maintained to make it easy to unroll simple metadata and filesystem
 * operations.
 */
typedef struct __wt_meta_track {
	enum {
		WT_ST_EMPTY = 0,	/* Unused slot */
		WT_ST_CHECKPOINT,	/* Complete a checkpoint */
		WT_ST_DROP_COMMIT,	/* Drop post commit */
		WT_ST_FILEOP,		/* File operation */
		WT_ST_LOCK,		/* Lock a handle */
		WT_ST_REMOVE,		/* Remove a metadata entry */
		WT_ST_SET		/* Reset a metadata entry */
	} op;
	char *a, *b;			/* Strings */
	WT_DATA_HANDLE *dhandle;	/* Locked handle */
	int created;			/* Handle on newly created file */
} WT_META_TRACK;

/*
 * __meta_track_next --
 *	Extend the list of operations we're tracking, as necessary, and
 *	optionally return the next slot.
 */
static int
__meta_track_next(WT_SESSION_IMPL *session, WT_META_TRACK **trkp)
{
	size_t offset, sub_off;

	if (session->meta_track_next == NULL)
		session->meta_track_next = session->meta_track;

	offset = WT_PTRDIFF(session->meta_track_next, session->meta_track);
	sub_off = WT_PTRDIFF(session->meta_track_sub, session->meta_track);
	if (offset == session->meta_track_alloc) {
		WT_RET(__wt_realloc(session, &session->meta_track_alloc,
		    WT_MAX(2 * session->meta_track_alloc,
		    20 * sizeof(WT_META_TRACK)), &session->meta_track));

		/* Maintain positions in the new chunk of memory. */
		session->meta_track_next =
		    (uint8_t *)session->meta_track + offset;
		if (session->meta_track_sub != NULL)
			session->meta_track_sub =
			    (uint8_t *)session->meta_track + sub_off;
	}

	WT_ASSERT(session, session->meta_track_next != NULL);

	if (trkp != NULL) {
		*trkp = session->meta_track_next;
		session->meta_track_next = *trkp + 1;
	}

	return (0);
}

/*
 * __meta_track_clear --
 *	Clear the structure.
 */
static void
__meta_track_clear(WT_SESSION_IMPL *session, WT_META_TRACK *trk)
{
	__wt_free(session, trk->a);
	__wt_free(session, trk->b);
	memset(trk, 0, sizeof(WT_META_TRACK));
}

/*
 * __meta_track_err --
 *	Drop the last operation off the end of the list, something went wrong
 * during initialization.
 */
static void
__meta_track_err(WT_SESSION_IMPL *session)
{
	WT_META_TRACK *trk;

	trk = session->meta_track_next;
	--trk;
	__meta_track_clear(session, trk);

	session->meta_track_next = trk;
}

/*
 * __wt_meta_track_discard --
 *	Cleanup metadata tracking when closing a session.
 */
void
__wt_meta_track_discard(WT_SESSION_IMPL *session)
{
	__wt_free(session, session->meta_track);
	session->meta_track_next = NULL;
	session->meta_track_alloc = 0;
}

/*
 * __wt_meta_track_on --
 *	Turn on metadata operation tracking.
 */
int
__wt_meta_track_on(WT_SESSION_IMPL *session)
{
	if (session->meta_track_nest++ == 0)
		WT_RET(__meta_track_next(session, NULL));

	return (0);
}

/*
 * __meta_track_apply --
 *	Apply the changes in a metadata tracking record.
 */
static int
__meta_track_apply(WT_SESSION_IMPL *session, WT_META_TRACK *trk, int unroll)
{
	WT_BM *bm;
	WT_BTREE *btree;
	WT_DECL_RET;
	int tret;

	/*
	 * Unlock handles and complete checkpoints regardless of whether we are
	 * unrolling.
	 */
	if (!unroll && trk->op != WT_ST_CHECKPOINT &&
	    trk->op != WT_ST_DROP_COMMIT && trk->op != WT_ST_LOCK)
		goto free;

	switch (trk->op) {
	case WT_ST_EMPTY:	/* Unused slot */
		break;
	case WT_ST_CHECKPOINT:	/* Checkpoint, see above */
		if (!unroll) {
			btree = trk->dhandle->handle;
			bm = btree->bm;
			WT_WITH_DHANDLE(session, trk->dhandle,
			    WT_TRET(bm->checkpoint_resolve(bm, session)));
		}
		break;
	case WT_ST_DROP_COMMIT:
		if ((tret = __wt_remove_if_exists(session, trk->a)) != 0) {
			__wt_err(session, tret,
			    "metadata remove dropped file %s",
			    trk->a);
			WT_TRET(tret);
		}
		break;
	case WT_ST_LOCK:	/* Handle lock, see above */
		if (unroll && trk->created)
			F_SET(trk->dhandle, WT_DHANDLE_DISCARD);
		WT_WITH_DHANDLE(session, trk->dhandle,
		    WT_TRET(__wt_session_release_btree(session)));
		break;
	case WT_ST_FILEOP:	/* File operation */
		/*
		 * For renames, both a and b are set.
		 * For creates, a is NULL.
		 * For removes, b is NULL.
		 */
		if (trk->a != NULL && trk->b != NULL &&
		    (tret = __wt_rename(session,
		    trk->b + strlen("file:"),
		    trk->a + strlen("file:"))) != 0) {
			__wt_err(session, tret,
			    "metadata unroll rename %s to %s",
			    trk->b, trk->a);
			WT_TRET(tret);
		} else if (trk->a == NULL) {
			if ((tret = __wt_remove(session,
			    trk->b + strlen("file:"))) != 0) {
				__wt_err(session, tret,
				    "metadata unroll create %s",
				    trk->b);
				WT_TRET(tret);
			}
		}
		/*
		 * We can't undo removes yet: that would imply
		 * some kind of temporary rename and remove in
		 * roll forward.
		 */
		break;
	case WT_ST_REMOVE:	/* Remove trk.a */
		if ((tret = __wt_metadata_remove(session, trk->a)) != 0) {
			__wt_err(session, tret,
			    "metadata unroll remove: %s",
			    trk->a);
			WT_TRET(tret);
		}
		break;
	case WT_ST_SET:		/* Set trk.a to trk.b */
		if ((tret = __wt_metadata_update(
		    session, trk->a, trk->b)) != 0) {
			__wt_err(session, tret,
			    "metadata unroll update %s to %s",
			    trk->a, trk->b);
			WT_TRET(tret);
		}
		break;
	WT_ILLEGAL_VALUE(session);
	}

free:	__meta_track_clear(session, trk);

	return (ret);
}

/*
 * __wt_meta_track_find_handle --
 *	Check if we have already seen a handle.
 */
int
__wt_meta_track_find_handle(
    WT_SESSION_IMPL *session, const char *name, const char *checkpoint)
{
	WT_META_TRACK *trk, *trk_orig;

	WT_ASSERT(session,
	    WT_META_TRACKING(session) && session->meta_track_nest > 0);

	trk_orig = session->meta_track;
	trk = session->meta_track_next;

	while (--trk >= trk_orig) {
		if (trk->op != WT_ST_LOCK)
			continue;
		if (strcmp(trk->dhandle->name, name) == 0 &&
		    ((trk->dhandle->checkpoint == NULL && checkpoint == NULL) ||
		    (trk->dhandle->checkpoint != NULL &&
		    strcmp(trk->dhandle->checkpoint, checkpoint) == 0)))
			return (0);
	}

	return (WT_NOTFOUND);
}

/*
 * __wt_meta_track_off --
 *	Turn off metadata operation tracking, unrolling on error.
 */
int
__wt_meta_track_off(WT_SESSION_IMPL *session, int need_sync, int unroll)
{
	WT_DECL_RET;
	WT_META_TRACK *trk, *trk_orig;

	WT_ASSERT(session,
	    WT_META_TRACKING(session) && session->meta_track_nest > 0);

	trk_orig = session->meta_track;
	trk = session->meta_track_next;

	/* If it was a nested transaction, there is nothing to do. */
	if (--session->meta_track_nest != 0)
		return (0);

	/* Turn off tracking for unroll. */
	session->meta_track_next = session->meta_track_sub = NULL;

	/*
	 * If there were no operations logged, return now and avoid unnecessary
	 * metadata checkpoints.  For example, this happens if attempting to
	 * create a data source that already exists (or drop one that doesn't).
	 */
	if (trk == trk_orig)
		return (0);

	while (--trk >= trk_orig)
		WT_TRET(__meta_track_apply(session, trk, unroll));

	/*
	 * Unroll operations don't need to flush the metadata.
	 *
	 * Also, if we don't have the metadata handle (e.g, we're in the
	 * process of creating the metadata), we can't sync it.
	 */
	if (unroll || ret != 0 || !need_sync || session->meta_dhandle == NULL)
		return (ret);

	/* If we're logging, make sure the metadata update was flushed. */
	if (FLD_ISSET(S2C(session)->log_flags, WT_CONN_LOG_ENABLED)) {
		if (!FLD_ISSET(S2C(session)->txn_logsync,
		    WT_LOG_DSYNC | WT_LOG_FSYNC))
			WT_WITH_DHANDLE(session, session->meta_dhandle,
			    ret = __wt_txn_checkpoint_log(session,
			    0, WT_TXN_LOG_CKPT_SYNC, NULL));
	} else {
		WT_WITH_DHANDLE(session, session->meta_dhandle,
		    ret = __wt_checkpoint(session, NULL));
		WT_RET(ret);
		WT_WITH_DHANDLE(session, session->meta_dhandle,
		    ret = __wt_checkpoint_sync(session, NULL));
	}

	return (ret);
}

/*
 * __wt_meta_track_sub_on --
 *	Start a group of operations that can be committed independent of the
 *	main transaction.
 */
int
__wt_meta_track_sub_on(WT_SESSION_IMPL *session)
{
	WT_ASSERT(session, session->meta_track_sub == NULL);
	session->meta_track_sub = session->meta_track_next;
	return (0);
}

/*
 * __wt_meta_track_sub_off --
 *	Commit a group of operations independent of the main transaction.
 */
int
__wt_meta_track_sub_off(WT_SESSION_IMPL *session)
{
	WT_DECL_RET;
	WT_META_TRACK *trk, *trk_orig;

	if (!WT_META_TRACKING(session) || session->meta_track_sub == NULL)
		return (0);

	trk_orig = session->meta_track_sub;
	trk = session->meta_track_next;

	/* Turn off tracking for unroll. */
	session->meta_track_next = session->meta_track_sub = NULL;

	while (--trk >= trk_orig)
		WT_TRET(__meta_track_apply(session, trk, 0));

	session->meta_track_next = trk_orig;
	return (ret);
}

/*
 * __wt_meta_track_checkpoint --
 *	Track a handle involved in a checkpoint.
 */
int
__wt_meta_track_checkpoint(WT_SESSION_IMPL *session)
{
	WT_META_TRACK *trk;

	WT_ASSERT(session, session->dhandle != NULL);

	WT_RET(__meta_track_next(session, &trk));

	trk->op = WT_ST_CHECKPOINT;
	trk->dhandle = session->dhandle;
	return (0);
}
/*
 * __wt_meta_track_insert --
 *	Track an insert operation.
 */
int
__wt_meta_track_insert(WT_SESSION_IMPL *session, const char *key)
{
	WT_DECL_RET;
	WT_META_TRACK *trk;

	WT_RET(__meta_track_next(session, &trk));

	trk->op = WT_ST_REMOVE;
	WT_ERR(__wt_strdup(session, key, &trk->a));
	return (0);

err:	__meta_track_err(session);
	return (ret);
}

/*
 * __wt_meta_track_update --
 *	Track a metadata update operation.
 */
int
__wt_meta_track_update(WT_SESSION_IMPL *session, const char *key)
{
	WT_DECL_RET;
	WT_META_TRACK *trk;

	WT_RET(__meta_track_next(session, &trk));

	trk->op = WT_ST_SET;
	WT_ERR(__wt_strdup(session, key, &trk->a));

	/*
	 * If there was a previous value, keep it around -- if not, then this
	 * "update" is really an insert.
	 */
	if ((ret =
	    __wt_metadata_search(session, key, &trk->b)) == WT_NOTFOUND) {
		trk->op = WT_ST_REMOVE;
		ret = 0;
	}
	WT_ERR(ret);
	return (0);

err:	__meta_track_err(session);
	return (ret);
}

/*
 * __wt_meta_track_fileop --
 *	Track a filesystem operation.
 */
int
__wt_meta_track_fileop(
    WT_SESSION_IMPL *session, const char *olduri, const char *newuri)
{
	WT_DECL_RET;
	WT_META_TRACK *trk;

	WT_RET(__meta_track_next(session, &trk));

	trk->op = WT_ST_FILEOP;
	WT_ERR(__wt_strdup(session, olduri, &trk->a));
	WT_ERR(__wt_strdup(session, newuri, &trk->b));
	return (0);

err:	__meta_track_err(session);
	return (ret);
}

/*
 * __wt_meta_track_drop --
 *	Track a file drop, where the remove is deferred until commit.
 */
int
__wt_meta_track_drop(
    WT_SESSION_IMPL *session, const char *filename)
{
	WT_DECL_RET;
	WT_META_TRACK *trk;

	WT_RET(__meta_track_next(session, &trk));

	trk->op = WT_ST_DROP_COMMIT;
	WT_ERR(__wt_strdup(session, filename, &trk->a));
	return (0);

err:	__meta_track_err(session);
	return (ret);
}

/*
 * __wt_meta_track_handle_lock --
 *	Track a locked handle.
 */
int
__wt_meta_track_handle_lock(WT_SESSION_IMPL *session, int created)
{
	WT_META_TRACK *trk;

	WT_ASSERT(session, session->dhandle != NULL);

	WT_RET(__meta_track_next(session, &trk));

	trk->op = WT_ST_LOCK;
	trk->dhandle = session->dhandle;
	trk->created = created;
	return (0);
}
