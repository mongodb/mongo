/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __drop --
 *	Drop all snapshots with a specific name.
 */
static void
__drop(WT_SNAPSHOT *snapbase, const char *name, size_t len)
{
	WT_SNAPSHOT *snap;

	WT_SNAPSHOT_FOREACH(snapbase, snap)
		if (strlen(snap->name) == len &&
		    strncmp(snap->name, name, len) == 0)
			F_SET(snap, WT_SNAP_DELETE);
}

/*
 * __drop_from --
 *	Drop all snapshots after, and including, the named snapshot.
 */
static void
__drop_from(WT_SNAPSHOT *snapbase, const char *name, size_t len)
{
	WT_SNAPSHOT *snap;
	int matched;

	/*
	 * There's a special case -- if the name is "all", then we delete all
	 * of the snapshots.
	 */
	if (len == strlen("all") && strncmp(name, "all", len) == 0) {
		WT_SNAPSHOT_FOREACH(snapbase, snap)
			F_SET(snap, WT_SNAP_DELETE);
		return;
	}

	/*
	 * We use the first snapshot we can find, that is, if there are two
	 * snapshots with the same name in the list, we'll delete from the
	 * first match to the end.
	 */
	matched = 0;
	WT_SNAPSHOT_FOREACH(snapbase, snap) {
		if (!matched &&
		    (strlen(snap->name) != len ||
		    strncmp(snap->name, name, len) != 0))
			continue;

		matched = 1;
		F_SET(snap, WT_SNAP_DELETE);
	}
}

/*
 * __drop_to --
 *	Drop all snapshots before, and including, the named snapshot.
 */
static void
__drop_to(WT_SNAPSHOT *snapbase, const char *name, size_t len)
{
	WT_SNAPSHOT *mark, *snap;

	/*
	 * We use the last snapshot we can find, that is, if there are two
	 * snapshots with the same name in the list, we'll delete from the
	 * beginning to the second match, not the first.
	 */
	mark = NULL;
	WT_SNAPSHOT_FOREACH(snapbase, snap)
		if (strlen(snap->name) == len &&
		    strncmp(snap->name, name, len) == 0)
			mark = snap;

	if (mark == NULL)
		return;

	WT_SNAPSHOT_FOREACH(snapbase, snap) {
		F_SET(snap, WT_SNAP_DELETE);

		if (snap == mark)
			break;
	}
}

/*
 * __wt_snapshot --
 *	Snapshot a tree.
 */
int
__wt_snapshot(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_BTREE *btree;
	WT_CONFIG dropconf;
	WT_CONFIG_ITEM cval, k, v;
	WT_DECL_RET;
	WT_SNAPSHOT *deleted, *snap, *snapbase;
	const char *name;
	char *name_alloc;
	int force, tracked;

	btree = session->btree;
	force = tracked = 0;
	snap = snapbase = NULL;
	name_alloc = NULL;

	/* Snapshots are single-threaded. */
	__wt_writelock(session, btree->snaplock);

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

	/*
	 * This may be a named snapshot, check the configuration.  If it's a
	 * named snapshot, set force, we have to create the snapshot even if
	 * the tree is clean.
	 */
	cval.len = 0;
	if (cfg != NULL)
		WT_ERR(__wt_config_gets(session, cfg, "name", &cval));
	if (cval.len == 0)
		name = WT_INTERNAL_SNAPSHOT;
	else {
		force = 1;
		WT_ERR(__wt_strndup(session, cval.str, cval.len, &name_alloc));
		name = name_alloc;
	}

	/*
	 * We may be dropping snapshots, check the configuration.  If we're
	 * dropping snapshots, set force, we have to create the snapshot even
	 * if the tree is clean.
	 */
	if (cfg != NULL) {
		cval.len = 0;
		WT_ERR(__wt_config_gets(session, cfg, "drop", &cval));
		if (cval.len != 0) {
			WT_ERR(__wt_config_subinit(session, &dropconf, &cval));
			while ((ret =
			    __wt_config_next(&dropconf, &k, &v)) == 0) {
				force = 1;

				if (v.len == 0)
					__drop(snapbase, k.str, k.len);
				else if (k.len == strlen("from") &&
				    strncmp(k.str, "from", k.len) == 0)
					__drop_from(snapbase, v.str, v.len);
				else if (k.len == strlen("to") &&
				    strncmp(k.str, "to", k.len) == 0)
					__drop_to(snapbase, v.str, v.len);
				else
					WT_ERR_MSG(session, EINVAL,
					    "unexpected value for snapshot "
					    "key: %.*s",
					    (int)k.len, k.str);
			}
			WT_ERR_NOTFOUND_OK(ret);
		}
	}

	/* Discard snapshots named the same as the snapshot being created. */
	__drop(snapbase, name, strlen(name));

	/* Add a new snapshot entry at the end of the list. */
	WT_SNAPSHOT_FOREACH(snapbase, snap)
		;
	WT_ERR(__wt_strdup(session, name, &snap->name));
	F_SET(snap, WT_SNAP_ADD);

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

	/* Flush the file from the cache, creating the snapshot. */
	WT_ERR(__wt_bt_cache_flush(
	    session, snapbase, cfg == NULL ? WT_SYNC_DISCARD : WT_SYNC, force));

	/* If there was a snapshot, update the metadata and resolve it. */
	if (snap->raw.data == NULL) {
		if (force)
			WT_ERR_MSG(session,
			    EINVAL, "cache flush failed to create a snapshot");
	} else {
		WT_ERR(__wt_meta_snaplist_set(session, btree->name, snapbase));
		/*
		 * If tracking is enabled, defer making pages available until
		 * the end of the transaction.  The exception is if the handle
		 * is being discarded: in that case, it will be gone by the
		 * time we try to apply or unroll the meta tracking event.
		 */
		if (WT_META_TRACKING(session) && cfg != NULL) {
			WT_ERR(__wt_meta_track_checkpoint(session));
			tracked = 1;
		} else
			WT_ERR(__wt_bm_snapshot_resolve(session, snapbase));
	}

err:	__wt_meta_snaplist_free(session, snapbase);
	if (!tracked)
		__wt_rwunlock(session, btree->snaplock);

	__wt_free(session, name_alloc);

	return (ret);
}
