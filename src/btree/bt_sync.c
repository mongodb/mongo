/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_btree_sync --
 *	Sync the tree.
 */
int
__wt_btree_sync(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_BTREE *btree;
	WT_CONFIG_ITEM cval;
	char *name;
	int ret;

	btree = session->btree;

	/* This may be a named snapshot, check the configuration. */
	ret = __wt_config_gets(session, cfg, "snapshot", &cval);
	if (ret == WT_NOTFOUND || cval.len == 0)
		name = NULL;
	else {
		WT_RET(ret);

		WT_ERR(__wt_strndup(session, cval.str, cval.len, &name));

		/*
		 * If it's a named snapshot and there's nothing dirty, we won't
		 * write any pages: mark the root page dirty to ensure a write.
		 */
		WT_ERR(__wt_page_modify_init(session, btree->root_page));
		__wt_page_modify_set(btree->root_page);

	}

	ret = __wt_btree_snapshot(session, name, 0);

err:	__wt_free(session, name);
	return (ret);
}

/*
 * __wt_btree_snapshot --
 *	Snapshot the tree.
 */
int
__wt_btree_snapshot(WT_SESSION_IMPL *session, const char *name, int discard)
{
	WT_SNAPSHOT *snapbase, *snap;
	WT_BTREE *btree;
	int ret;

	btree = session->btree;
	snapbase = NULL;
	ret = 0;

	/* Snapshots are single-threaded. */
	__wt_writelock(session, btree->snaplock);

	/* Get the list of snapshots for this file. */
	WT_ERR(__wt_session_snap_list_get(session, NULL, &snapbase));

	/*
	 * Review the existing snapshots, deleting any with matching names, and
	 * add the new snapshot entry at the end of the list.
	 */
	if (name == NULL)
		name = WT_INTERNAL_SNAPSHOT;
	WT_SNAPSHOT_FOREACH(snapbase, snap)
		if (strcmp(name, snap->name) == 0)
			FLD_SET(snap->flags, WT_SNAP_DELETE);
	WT_ERR(__wt_strdup(session, name, &snap->name));
	FLD_SET(snap->flags, WT_SNAP_ADD);

	/*
	 * Ask the eviction thread to flush any dirty pages.
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
	btree->snap = snapbase;
	do {
		ret = __wt_sync_file_serial(session, discard);
	} while (ret == WT_RESTART);
	btree->snap = NULL;
	WT_ERR(ret);

	/* If we're discarding the tree, the root page should be gone. */
	WT_ASSERT(session, !discard || btree->root_page == NULL);

	/* If there was a snapshot, update the schema table. */
	if (snap->raw.data != NULL)
		WT_ERR(__wt_session_snap_list_set(session, snapbase));

err:	__wt_session_snap_list_free(session, snapbase);

	__wt_rwunlock(session, btree->snaplock);

	return (ret);
}
