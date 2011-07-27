/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_btree_stat_print --
 *	Print DB handle statistics to a stream.
 */
int
__wt_btree_stat_print(WT_SESSION_IMPL *session, FILE *stream)
{
	WT_BTREE *btree;
	WT_CONNECTION_IMPL *conn;

	btree = session->btree;
	conn = S2C(session);

	fprintf(stream, "Btree statistics: %s\n", btree->name);
	__wt_stat_print_btree_stats(btree->stats, stream);
	fprintf(stream, "%s\n", conn->sep);

	/* Clear the file stats, then call Btree stat to fill them in. */
	__wt_stat_clear_btree_file_stats(btree->fstats);
	WT_STAT_SET(
	    btree->fstats, file_freelist_entries, btree->freelist_entries);

	/*
	 * XXX
	 * This doesn't belong here -- leave it alone until the stat
	 * infrastructure gets reworked.
	 */
	WT_STAT_SET(btree->fstats, file_magic, WT_BTREE_MAGIC);
	WT_STAT_SET(btree->fstats, file_major, WT_BTREE_MAJOR_VERSION);
	WT_STAT_SET(btree->fstats, file_minor, WT_BTREE_MINOR_VERSION);
	WT_STAT_SET(btree->fstats, file_allocsize, btree->allocsize);
	WT_STAT_SET(btree->fstats, file_intlmax, btree->intlmax);
	WT_STAT_SET(btree->fstats, file_intlmin, btree->intlmin);
	WT_STAT_SET(btree->fstats, file_leafmax, btree->leafmax);
	WT_STAT_SET(btree->fstats, file_leafmin, btree->leafmin);
	WT_STAT_SET(btree->fstats, file_fixed_len, btree->bitcnt);

	/*
	 * Note we do not have a hazard reference for the root page, and that's
	 * safe -- root pages are pinned into memory when a file is opened, and
	 * never re-written until the file is closed.
	 */
	WT_RET(__wt_tree_walk(session, NULL, __wt_page_stat, NULL));

	fprintf(stream, "Btree file statistics: %s\n", btree->name);
	__wt_stat_print_btree_file_stats(btree->fstats, stream);
	fprintf(stream, "%s\n", conn->sep);

	/* Underlying file handle statistics. */
	if (btree->fh != NULL) {
		fprintf(stream, "Btree I/O statistics: %s\n", btree->name);
		__wt_stat_print_file_stats(btree->fh->stats, stream);
		fprintf(stream, "%s\n", conn->sep);
	}

	return (0);
}

/*
 * __wt_btree_stat_clear --
 *	Clear DB handle statistics.
 */
int
__wt_btree_stat_clear(WT_BTREE *btree)
{
	__wt_stat_clear_btree_stats(btree->stats);
	__wt_stat_clear_btree_file_stats(btree->fstats);
	if (btree->fh != NULL)
		__wt_stat_clear_file_stats(btree->fh->stats);

	return (0);
}
