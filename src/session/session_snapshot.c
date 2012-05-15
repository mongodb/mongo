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
 * __wt_snapshot --
 *	Snapshot the tree.
 */
int
__wt_snapshot(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONFIG_ITEM cval;
	WT_DECL_RET;
	char *name;

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
 * __wt_snapshot_close --
 *	Snapshot the tree when the handle is closed.
 */
int
__wt_snapshot_close(WT_SESSION_IMPL *session)
{
	return (__snapshot_worker(session, NULL, 1, SNAPSHOT));
}

/*
 * __wt_snapshot_drop --
 *	Snapshot the tree, dropping one or more snapshots.
 */
int
__wt_snapshot_drop(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONFIG_ITEM cval, sval;
	WT_DECL_RET;
	char *name;

	name = NULL;

	WT_RET(__wt_config_gets(session, cfg, "snapshot", &cval));
	if (cval.type != ITEM_STRUCT) {
		WT_RET(__wt_strndup(session, cval.str, cval.len, &name));
		ret = __snapshot_worker(session, name, 0, SNAPSHOT_DROP);
	} else if (__wt_config_subgets(session, &cval, "all", &sval) == 0 &&
	    sval.val != 0)
		ret = __snapshot_worker(session, name, 0, SNAPSHOT_DROP_ALL);
	else if (__wt_config_subgets(session, &cval, "from", &sval) == 0 &&
	    sval.len != 0) {
		WT_RET(__wt_strndup(session, sval.str, sval.len, &name));
		ret = __snapshot_worker(session, name, 0, SNAPSHOT_DROP_FROM);
	} else if (__wt_config_subgets(session, &cval, "to", &sval) == 0 &&
	    sval.len != 0) {
		WT_RET(__wt_strndup(session, sval.str, sval.len, &name));
		ret = __snapshot_worker(session, name, 0, SNAPSHOT_DROP_TO);
	} else
		WT_RET_MSG(session, EINVAL,
		    "Unexpected value for 'snapshot' key: %.*s",
		    (int)cval.len, cval.str);

	__wt_free(session, name);
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
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_SNAPSHOT *deleted, *snap, *snapbase;
	int force, matched;

	btree = session->btree;
	matched = 0;
	snap = snapbase = NULL;

	/* Snapshots are single-threaded. */
	__wt_writelock(session, btree->snaplock);

	/* Set the name to the default, if we aren't provided one. */
	if (op == SNAPSHOT && name == NULL) {
		force = 0;
		name = WT_INTERNAL_SNAPSHOT;
	} else
		force = 1;

	/*
	 * Get the list of snapshots for this file.  If there's no reference,
	 * this file is dead.  Discard it from the cache without bothering to
	 * write any dirty pages.
	 */
	if ((ret =
	    __wt_meta_snaplist_get(session, btree->name, &snapbase)) != 0) {
		if (ret == WT_NOTFOUND)
			ret = __wt_bt_cache_flush(
			    session, NULL, WT_SYNC_DISCARD_NOWRITE, 0);
		goto err;
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
				matched = 1;
			else if (strcmp(snap->name, WT_INTERNAL_SNAPSHOT) != 0)
				continue;
			F_SET(snap, WT_SNAP_DELETE);
		}
		if (!matched)
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
			if (strcmp(snap->name, name) == 0)
				matched = 1;
			if (matched ||
			    strcmp(snap->name, WT_INTERNAL_SNAPSHOT) == 0)
				F_SET(snap, WT_SNAP_DELETE);
		}
		if (!matched)
			goto nomatch;

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
			if (!matched ||
			    strcmp(snap->name, WT_INTERNAL_SNAPSHOT) == 0)
				F_SET(snap, WT_SNAP_DELETE);
			if (strcmp(snap->name, name) == 0)
				matched = 1;
		}
		if (!matched)
nomatch:		WT_ERR_MSG(session,
			    EINVAL, "no snapshot named %s was found", name);

		WT_ERR(__wt_strdup(session, WT_INTERNAL_SNAPSHOT, &snap->name));
		F_SET(snap, WT_SNAP_ADD);
		break;
	}

	/*
	 * Lock the snapshots that will be deleted.
	 *
	 * Snapshots are only locked when tracking is enabled, which covers
	 * sync and drop operations, but not close.  The reasoning is that
	 * there should be no access to a snapshot during close, because any
	 * thread accessing a snapshot will also have the current file handle
	 * open.
	 */
	if (WT_META_TRACKING(session))
		WT_SNAPSHOT_FOREACH(snapbase, deleted)
			if (F_ISSET(deleted, WT_SNAP_DELETE))
				WT_ERR(__wt_session_lock_snapshot(session,
				    deleted->name, WT_BTREE_EXCLUSIVE));

	WT_ERR(__wt_bt_cache_flush(
	    session, snapbase, discard ? WT_SYNC_DISCARD : WT_SYNC, force));

	/* If there was a snapshot, update the metadata. */
	if (snap->raw.data == NULL) {
		if (force)
			WT_ERR_MSG(session,
			    EINVAL, "cache flush failed to create a snapshot");
	} else
		WT_ERR(__wt_meta_snaplist_set(session, btree->name, snapbase));

err:	__wt_meta_snaplist_free(session, snapbase);
	__wt_rwunlock(session, btree->snaplock);

	return (ret);
}
