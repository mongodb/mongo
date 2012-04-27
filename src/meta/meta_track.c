/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
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
		WT_ST_EMPTY=0,		/* Unused slot */
		WT_ST_FILEOP=1,		/* File operation */
		WT_ST_REMOVE=2,		/* Remove a metadata entry */
		WT_ST_SET=3		/* Reset a metadata entry */
	} op;
	const char *a, *b;		/* Strings */
} WT_META_TRACK;

/*
 * __meta_track_next --
 *	Return the next slot, and extend the list of operations we're tracking,
 * as necessary.
 */
static int
__meta_track_next(WT_SESSION_IMPL *session, WT_META_TRACK **trkp)
{
	WT_META_TRACK *trk;
	size_t bytes_allocated;
	u_int i;

	/*
	 * Slow, but we don't care -- it's a metadata operation, searching an
	 * array of maybe 20 items.
	 */
	for (trk = session->meta_track,
	    i = 0; i <  session->meta_track_entries; ++trk, ++i)
		if (trk->op == WT_ST_EMPTY) {
			if (trkp != NULL)
				*trkp = trk;
			return (0);
		}

	/*
	 * The __wt_realloc() function uses the "bytes allocated" value
	 * to figure out how much of the memory it needs to clear (see
	 * the function for an explanation of why the memory is cleared,
	 * it's a security thing).
	 */
	bytes_allocated =
	    session->meta_track_entries * sizeof(WT_META_TRACK);
	WT_RET(__wt_realloc(session, &bytes_allocated,
	    (session->meta_track_entries + 20) * sizeof(WT_META_TRACK),
	    &session->meta_track));
	if (trkp != NULL)
		*trkp = &((WT_META_TRACK *)
		    session->meta_track)[session->meta_track_entries];
	session->meta_track_entries += 20;
	return (0);
}

/*
 * __wt_meta_track_on --
 *	Turn on metadata operation tracking.
 */
int
__wt_meta_track_on(WT_SESSION_IMPL *session)
{
	return (__meta_track_next(session, NULL));
}

/*
 * __wt_meta_track_off --
 *	Turn off metadata operation tracking, unrolling on error.
 */
int
__wt_meta_track_off(WT_SESSION_IMPL *session, int unroll)
{
	WT_DECL_RET;
	WT_META_TRACK *trk, *trk_orig;
	int tret;

	if (session->meta_track == NULL || session->meta_track_entries == 0)
		return (0);

	trk_orig = session->meta_track;
	trk = &trk_orig[session->meta_track_entries - 1];

	/* Turn off tracking for unroll. */
	session->meta_track = NULL;
	session->meta_track_entries = 0;

	for (;; --trk) {
		if (unroll)
			switch (trk->op) {
			case WT_ST_EMPTY:	/* Unused slot */
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
					    "metadata unroll rename "
					    "%s to %s",
					    trk->b, trk->a);
					WT_TRET(tret);
				} else if (trk->a == NULL &&
				    ((tret = __wt_session_close_any_open_btree(
				    session, trk->b)) != 0 ||
				    (tret = __wt_remove(session,
				    trk->b + strlen("file:"))) != 0)) {
					__wt_err(session, tret,
					    "metadata unroll create %s",
					    trk->b);
					WT_TRET(tret);
				}
				/*
				 * We can't undo removes yet: that would imply
				 * some kind of temporary rename and remove in
				 * roll forward.
				 */
				break;
			case WT_ST_REMOVE:	/* Remove trk.a */
				if ((tret = __wt_metadata_remove(
				    session, trk->a)) != 0) {
					__wt_err(session, ret,
					    "metadata unroll remove: %s",
					    trk->a);
					WT_TRET(tret);
				}
				break;
			case WT_ST_SET:		/* Set trk.a to trk.b */
				if ((tret = __wt_metadata_update(
				    session, trk->a, trk->b)) != 0) {
					__wt_err(session, ret,
					    "metadata unroll update "
					    "%s to %s",
					    trk->a, trk->b);
					WT_TRET(tret);
				}
				break;
			WT_ILLEGAL_VALUE(session);
			}

		__wt_free(session, trk->a);
		__wt_free(session, trk->b);

		if (trk == trk_orig)
			break;
	}
	__wt_free(session, trk_orig);
	return (ret);
}

/*
 * __wt_meta_track_insert --
 *	Track an insert operation.
 */
int
__wt_meta_track_insert(WT_SESSION_IMPL *session, const char *key)
{
	WT_META_TRACK *trk;

	WT_RET(__meta_track_next(session, &trk));

	trk->op = WT_ST_REMOVE;
	WT_RET(__wt_strdup(session, key, &trk->a));

	return (0);
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
	WT_RET(__wt_strdup(session, key, &trk->a));

	/*
	 * If there was a previous value, keep it around -- if not, then this
	 * "update" is really an insert.
	 */
	if ((ret = __wt_metadata_read(session, key, &trk->b)) == WT_NOTFOUND) {
		trk->op = WT_ST_REMOVE;
		ret = 0;
	}
	return (ret);
}

/*
 * __wt_meta_track_fs_rename --
 *	Track a filesystem rename operation.
 */
int
__wt_meta_track_fileop(
    WT_SESSION_IMPL *session, const char *olduri, const char *newuri)
{
	WT_META_TRACK *trk;

	WT_RET(__meta_track_next(session, &trk));

	trk->op = WT_ST_FILEOP;
	if (olduri != NULL)
		WT_RET(__wt_strdup(session, olduri, &trk->a));
	if (newuri != NULL)
		WT_RET(__wt_strdup(session, newuri, &trk->b));
	return (0);
}
