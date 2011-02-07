/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_db_stat_print --
 *	Print DB handle statistics to a stream.
 */
int
__wt_db_stat_print(WT_TOC *toc, FILE *stream)
{
	DB *db;
	ENV *env;
	BTREE *btree;

	db = toc->db;
	env = toc->env;
	btree = db->btree;

	fprintf(stream, "File statistics: %s\n", btree->name);
	__wt_stat_print(env, btree->stats, stream);

	/* Clear the file stats, then call Btree stat to fill them in. */
	__wt_stat_clear_btree_file_stats(btree->fstats);
	WT_STAT_SET(btree->fstats, FREELIST_ENTRIES, btree->freelist_entries);
	WT_RET(__wt_desc_stat(toc));

	/*
	 * Note we do not have a hazard reference for the root page, and that's
	 * safe -- root pages are pinned into memory when a file is opened, and
	 * never re-written until the file is closed.
	 */
	WT_RET(__wt_tree_walk(toc, NULL, 0, __wt_page_stat, NULL));

	fprintf(stream, "File statistics: %s\n", btree->name);
	__wt_stat_print(env, btree->fstats, stream);

	/* Underlying file handle statistics. */
	if (btree->fh != NULL) {
		fprintf(stream,
		    "Underlying file I/O statistics: %s\n", btree->name);
		__wt_stat_print(env, btree->fh->stats, stream);
	}

	return (0);
}

/*
 * __wt_db_stat_clear --
 *	Clear DB handle statistics.
 */
int
__wt_db_stat_clear(DB *db)
{
	BTREE *btree;

	btree = db->btree;

	__wt_stat_clear_btree_handle_stats(btree->stats);
	__wt_stat_clear_btree_file_stats(btree->fstats);
	if (btree->fh != NULL)
		__wt_stat_clear_fh_stats(btree->fh->stats);

	return (0);
}
