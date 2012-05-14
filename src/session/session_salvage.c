/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_salvage --
 *	Salvage a single file.
 */
int
__wt_salvage(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_SNAPSHOT snapbase[2];

	btree = session->btree;

	/*
	 * XXX
	 * The salvage process reads and discards previous snapshot blocks, so
	 * the underlying block manager has to ignore any previous snapshot
	 * entries when creating a new snapshot, in other words, we can't use
	 * the metadata snapshot list, it has all of those snapshots listed and
	 * we don't care about them.  Build a clean snapshot array and use it
	 * instead.
	 *
	 * Don't first clear the metadata snapshot list and call the snapshot
	 * get routine: a crash between clearing the metadata snapshot list and
	 * creating a new snapshot list would look like a create or open of a
	 * file without a snapshot from which to roll-forward, and the contents
	 * of the file would be discarded.
	 */
	memset(snapbase, 0, sizeof(snapbase));

	WT_RET(__wt_strdup(session, WT_INTERNAL_SNAPSHOT, &snapbase[0].name));
	F_SET(&snapbase[0], WT_SNAP_ADD);

	WT_ERR(__wt_bt_salvage(session, snapbase, cfg));

	/*
	 * If no snapshot was created, well, it's probably bad news, but there
	 * is nothing to do but clear any recorded snapshots for the file.  If
	 * a snapshot was created, life is good, replace any recorded snapshots
	 * with the new one.
	 */
	if (snapbase[0].raw.data == NULL)
		WT_ERR(__wt_meta_snapshot_clear(session, btree->name));
	else
		WT_ERR(__wt_meta_snaplist_set(session, btree->name, snapbase));

err:	__wt_free(session, snapbase[0].name);
	return (0);
}
