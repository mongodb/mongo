/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

typedef enum {
	SNAPSHOT,			/* Create snapshot */
	SNAPSHOT_DROP,			/* Drop named snapshot */
	SNAPSHOT_DROP_ALL,		/* Drop all snapshots */
	SNAPSHOT_DROP_FROM,		/* Drop snapshots from name to end */
	SNAPSHOT_DROP_TO		/* Drop snapshots from start to name */
} snapshot_op;

static int __snapshot_worker(WT_SESSION_IMPL *, const char *, int, snapshot_op);

/*
 * __wt_btree_snapshot --
 *	Snapshot the tree.
 */
int
__wt_btree_snapshot(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONFIG_ITEM cval;
	char *name;
	int ret;

	name = NULL;

	/* This may be a named snapshot, check the configuration. */
	if ((ret = __wt_config_gets(
	    session, cfg, "snapshot", &cval)) != 0 && ret != WT_NOTFOUND)
		WT_RET(ret);
	if (cval.len != 0)
		WT_RET(__wt_strndup(session, cval.str, cval.len, &name));

	ret = __snapshot_worker(session, name, 0, SNAPSHOT);

	__wt_free(session, name);
	return (ret);
}

/*
 * __wt_btree_snapshot_close --
 *	Snapshot the tree when the handle is closed.
 */
int
__wt_btree_snapshot_close(WT_SESSION_IMPL *session)
{
	return (__snapshot_worker(session, NULL, 1, SNAPSHOT));
}

/*
 * __wt_btree_snapshot_drop --
 *	Snapshot the tree, dropping one or more snapshots.
 */
int
__wt_btree_snapshot_drop(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONFIG_ITEM cval;
	int ret;
	char *name;

	name = NULL;

	if ((ret = __wt_config_gets(
	    session, cfg, "snapall", &cval)) != 0 && ret != WT_NOTFOUND)
		WT_RET(ret);
	if (cval.val != 0) {
		ret = __snapshot_worker(session, name, 0, SNAPSHOT_DROP_ALL);
		goto done;
	}

	if ((ret = __wt_config_gets(
	    session, cfg, "snapfrom", &cval)) != 0 && ret != WT_NOTFOUND)
		WT_RET(ret);
	if (cval.len != 0) {
		WT_RET(__wt_strndup(session, cval.str, cval.len, &name));
		ret =
		    __snapshot_worker(session, name, 0, SNAPSHOT_DROP_FROM);
		goto done;
	}

	if ((ret = __wt_config_gets(
	    session, cfg, "snapto", &cval)) != 0 && ret != WT_NOTFOUND)
		WT_RET(ret);
	if (cval.len != 0) {
		WT_RET(__wt_strndup(session, cval.str, cval.len, &name));
		ret = __snapshot_worker(session, name, 0, SNAPSHOT_DROP_TO);
		goto done;
	}

	if ((ret = __wt_config_gets(
	    session, cfg, "snapshot", &cval)) != 0 && ret != WT_NOTFOUND)
		WT_RET(ret);
	if (cval.len != 0) {
		WT_RET(__wt_strndup(session, cval.str, cval.len, &name));
		ret = __snapshot_worker(session, name, 0, SNAPSHOT_DROP);
		goto done;
	}

done:	__wt_free(session, name);
	return (ret);
}

/*
 * __snapshot_worker --
 *	Snapshot the tree.
 */
static int
__snapshot_worker(
    WT_SESSION_IMPL *session, const char *name, int discard, snapshot_op op)
{
	WT_SNAPSHOT *match, *snapbase, *snap;
	WT_BTREE *btree;
	int force, ret;

	btree = session->btree;
	match = snapbase = NULL;
	ret = 0;

	/* Snapshots are single-threaded. */
	__wt_writelock(session, btree->snaplock);

	/* Set the name to the default, if we aren't provided one. */
	if (name == NULL) {
		force = 0;
		name = WT_INTERNAL_SNAPSHOT;
	} else {
		force = 1;
		/*
		 * If it's a named snapshot and there's nothing dirty, we won't
		 * write any pages: mark the root page dirty to ensure a write.
		 */
		WT_ERR(__wt_page_modify_init(session, btree->root_page));
		__wt_page_modify_set(btree->root_page);
	}

	/*
	 * Get the list of snapshots for this file.  If there's no reference,
	 * this file is dead.  Discard it from the cache without bothering to
	 * write any dirty pages.
	 */
	if ((ret =
	    __wt_session_snap_list_get(session, NULL, &snapbase)) != 0) {
		if (ret == WT_NOTFOUND) {
			ret =
			    __wt_cache_flush(session, WT_SYNC_DISCARD_NOWRITE);

			/*
			 * XXX
			 * I'm not sure this is the right place to do this,
			 * but it's the point in the btree engine where we
			 * can know the root page is gone.
			 */
			btree->root_page = NULL;
			__wt_rwunlock(session, btree->snaplock);
			return (ret);
		}
		WT_ERR(ret);
	}

	switch (op) {
	case SNAPSHOT:
		/*
		 * Create a new, possibly named, snapshot.  Review existing
		 * snapshots, deleting default snapshots and snapshots with
		 * matching names, add the new snapshot entry at the end of
		 * the list.
		 */
		WT_SNAPSHOT_FOREACH(snapbase, snap)
			if (strcmp(snap->name, name) == 0 ||
			    strcmp(snap->name, WT_INTERNAL_SNAPSHOT) == 0)
				F_SET(snap, WT_SNAP_DELETE);

		WT_ERR(__wt_strdup(session, name, &snap->name));
		F_SET(snap, WT_SNAP_ADD);
		break;
	case SNAPSHOT_DROP:
		/*
		 * Drop all snapshots with matching names.
		 * Drop all snapshots with the default name.
		 * Add a new snapshot with the default name.
		 */
		WT_SNAPSHOT_FOREACH(snapbase, snap) {
			/*
			 * There should be only one snapshot with a matching
			 * name, but it doesn't hurt to check the rest.
			 */
			if (strcmp(snap->name, name) == 0)
				match = snap;
			if (strcmp(snap->name, name) == 0 ||
			    strcmp(snap->name, WT_INTERNAL_SNAPSHOT) == 0)
				F_SET(snap, WT_SNAP_DELETE);
		}
		if (match == NULL)
			goto nomatch;

		WT_ERR(__wt_strdup(session, WT_INTERNAL_SNAPSHOT, &snap->name));
		F_SET(snap, WT_SNAP_ADD);
		break;
	case SNAPSHOT_DROP_ALL:
		/*
		 * Drop all snapshots.
		 * Add a new snapshot with the default name.
		 */
		WT_SNAPSHOT_FOREACH(snapbase, snap)
			F_SET(snap, WT_SNAP_DELETE);

		WT_ERR(__wt_strdup(session, WT_INTERNAL_SNAPSHOT, &snap->name));
		F_SET(snap, WT_SNAP_ADD);
		break;
	case SNAPSHOT_DROP_FROM:
		/*
		 * Drop all snapshots after, and including, the named snapshot.
		 * Drop all snapshots with the default name.
		 * Add a new snapshot with the default name.
		 */
		WT_SNAPSHOT_FOREACH(snapbase, snap) {
			if (strcmp(snap->name, name) == 0) {
				match = snap;
				break;
			}
			if (strcmp(snap->name, WT_INTERNAL_SNAPSHOT) == 0)
				F_SET(snap, WT_SNAP_DELETE);
		}
		if (match == NULL)
			goto nomatch;

		WT_SNAPSHOT_CONTINUE(snap)
			F_SET(snap, WT_SNAP_DELETE);

		WT_ERR(__wt_strdup(session, WT_INTERNAL_SNAPSHOT, &snap->name));
		F_SET(snap, WT_SNAP_ADD);
		break;
	case SNAPSHOT_DROP_TO:
		/*
		 * Drop all snapshots before, and including, the named snapshot.
		 * Drop all snapshots with the default name.
		 * Add a new snapshot with the default name.
		 */
		WT_SNAPSHOT_FOREACH(snapbase, snap) {
			if (strcmp(snap->name, name) == 0)
				match = snap;
			if (strcmp(snap->name, WT_INTERNAL_SNAPSHOT) == 0)
				F_SET(snap, WT_SNAP_DELETE);
		}
		if (match == NULL)
nomatch:		WT_ERR_MSG(session,
			    EINVAL, "no snapshot named %s was found", name);

		WT_SNAPSHOT_FOREACH(snapbase, snap) {
			F_SET(snap, WT_SNAP_DELETE);
			if (match == snap)
				break;
		}

		WT_ERR(__wt_strdup(session, WT_INTERNAL_SNAPSHOT, &snap->name));
		F_SET(snap, WT_SNAP_ADD);
		break;
	}

	btree->snap = snapbase;
	ret = __wt_cache_flush(session, discard ? WT_SYNC_DISCARD : WT_SYNC);
	btree->snap = NULL;
	WT_ERR(ret);

	/* If we're discarding the tree, the root page should be gone. */
	WT_ASSERT(session, !discard || btree->root_page == NULL);

	/* If there was a snapshot, update the schema table. */
	if (snap->raw.data == NULL) {
		if (force)
			WT_ERR_MSG(session,
			    EINVAL, "cache flush failed to create a snapshot");
	} else
		WT_ERR(__wt_session_snap_list_set(session, snapbase));

err:	__wt_session_snap_list_free(session, snapbase);

	__wt_rwunlock(session, btree->snaplock);

	return (ret);
}

/*
 * __wt_cache_flush --
 *	Write dirty pages from the cache, optionally discarding the file.
 */
int
__wt_cache_flush(WT_SESSION_IMPL *session, int op)
{
	int ret;

	/*
	 * Ask the eviction thread to flush any dirty pages, and optionally
	 * discard the file from the cache.
	 *
	 * Reconciliation is just another reader of the page, so it's probably
	 * possible to do this work in the current thread, rather than poking
	 * the eviction thread.  The trick is for the sync thread to acquire
	 * hazard references on each dirty page, which would block the eviction
	 * thread from attempting to evict the pages.
	 *
	 * I'm not doing it for a few reasons: (1) I don't want sync to update
	 * the page's read-generation number, and eviction already knows how to
	 * do that, (2) I don't want more than a single sync running at a time,
	 * and calling eviction makes it work that way without additional work,
	 * (3) sync and eviction cannot operate on the same page at the same
	 * time and I'd rather they didn't operate in the same file at the same
	 * time, and using eviction makes it work that way without additional
	 * synchronization, (4) eventually we'll have async write I/O, and this
	 * approach results in one less page writer in the system, (5) the code
	 * already works that way.   None of these problems can't be fixed, but
	 * I don't see a reason to change at this time, either.
	 */

	do {
		ret = __wt_sync_file_serial(session, op);
	} while (ret == WT_RESTART);

	return (ret);
}
