/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
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
__wt_db_stat_print(WT_TOC *toc)
{
	wt_args_db_stat_print_unpack;
	IDB *idb;
	IENV *ienv;
	WT_STATS *stats;
	WT_STOC *stoc;

	idb = db->idb;
	ienv = toc->env->ienv;

	WT_DB_FCHK(db, "Db.stat_print", flags, WT_APIMASK_DB_STAT_PRINT);

	fprintf(stream, "Database statistics: %s\n", db->idb->dbname);

	/* Clear the database stats, then call Btree stat to fill them in. */
	WT_RET((__wt_stat_clear_db_dstats(db->dstats)));
	WT_RET((__wt_bt_stat(db)));

	for (stats = db->dstats; stats->desc != NULL; ++stats)
		fprintf(stream, "%llu\t%s\n", stats->v, stats->desc);

	/* Database handle statistics. */
	fprintf(stream, "%s\n", ienv->sep);
	fprintf(stream, "Database handle statistics: %s\n", db->idb->dbname);
	for (stats = db->hstats; stats->desc != NULL; ++stats)
		fprintf(stream, "%llu\t%s\n", stats->v, stats->desc);

	/* Underlying file handle statistics. */
	if (idb->fh != NULL) {
		fprintf(stream, "%s\n", ienv->sep);
		fprintf(stream,
		    "Underlying file I/O statistics: %s\n", db->idb->dbname);
		for (stats = idb->fh->stats; stats->desc != NULL; ++stats)
			fprintf(stream, "%llu\t%s\n", stats->v, stats->desc);
	}

	/* Underlying server thread statistics. */
	if (!F_ISSET(ienv, WT_SINGLE_THREADED) && idb->stoc != NULL) {
		fprintf(stream, "%s\n", ienv->sep);
		fprintf(stream,
		    "Database handle's server thread statistics\n");
		for (stats = idb->stoc->stats; stats->desc != NULL; ++stats)
			fprintf(stream, "%llu\t%s\n", stats->v, stats->desc);
	}

	return (0);
}

/*
 * __wt_db_stat_clear --
 *	Clear DB handle statistics.
 */
int
__wt_db_stat_clear(WT_TOC *toc)
{
	wt_args_db_stat_clear_unpack;
	IDB *idb;
	int ret;

	idb = db->idb;

	WT_DB_FCHK(db, "Db.stat_clear", flags, WT_APIMASK_DB_STAT_CLEAR);

	ret = __wt_stat_clear_db_hstats(db->hstats);
	WT_TRET((__wt_stat_clear_db_dstats(db->dstats)));
	if (idb->fh != NULL)
		WT_TRET((__wt_stat_clear_fh_stats(idb->fh->stats)));

	return (ret);
}
