/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
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
	IDB *idb;

	db = toc->db;
	env = toc->env;
	idb = db->idb;

	fprintf(stream, "Database handle statistics: %s\n", idb->name);
	__wt_stat_print(env, idb->stats, stream);

	/* Clear the database stats, then call Btree stat to fill them in. */
	__wt_stat_clear_database_stats(idb->dstats);
	WT_STAT_SET(idb->dstats, TREE_LEVEL, idb->root_page.page->hdr->level);
	WT_RET(__wt_bt_stat_desc(toc));

	/*
	 * Note we do not have a hazard reference for the root page, and that's
	 * safe -- root pages are pinned into memory when a database is opened,
	 * and never re-written until the database is closed.
	 */
	WT_RET(__wt_bt_tree_walk(toc, NULL, 0, __wt_bt_stat_page, NULL));

	fprintf(stream, "Database statistics: %s\n", idb->name);
	__wt_stat_print(env, idb->dstats, stream);

	/* Underlying file handle statistics. */
	if (idb->fh != NULL) {
		fprintf(stream,
		    "Underlying file I/O statistics: %s\n", idb->name);
		__wt_stat_print(env, idb->fh->stats, stream);
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
	IDB *idb;

	idb = db->idb;

	__wt_stat_clear_db_stats(idb->stats);
	__wt_stat_clear_database_stats(idb->dstats);
	if (idb->fh != NULL)
		__wt_stat_clear_fh_stats(idb->fh->stats);

	return (0);
}
