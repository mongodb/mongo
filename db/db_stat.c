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
	WT_STATS *stats;
	int ret;

	idb = db->idb;

	WT_DB_FCHK(db, "Db.stat_print", flags, WT_APIMASK_DB_STAT_PRINT);

	fprintf(stream, "%s\n", WT_GLOBAL(sep));
	fprintf(stream, "Database handle statistics: %s\n", db->idb->dbname);
	for (stats = db->hstats; stats->desc != NULL; ++stats)
		fprintf(stream, "%lu\t%s\n", (u_long)stats->v, stats->desc);
	if (idb->fh != NULL) {
		fprintf(stream, "%s\n", WT_GLOBAL(sep));
		fprintf(stream,
		    "Database handle I/O statistics: %s\n", db->idb->dbname);
		for (stats = idb->fh->stats; stats->desc != NULL; ++stats)
			fprintf(
			    stream, "%lu\t%s\n", (u_long)stats->v, stats->desc);
	}

	fprintf(stream, "%s\n", WT_GLOBAL(sep));
	fprintf(stream, "Database statistics: %s\n", db->idb->dbname);

	/* Clear the database stats, then call Btree stat to fill them in. */
	if ((ret = __wt_stat_clear_db_dstats(db->dstats)) != 0)
		return (ret);
	if ((ret = __wt_bt_stat(db)) != 0)
		return (ret);

	for (stats = db->dstats; stats->desc != NULL; ++stats)
		fprintf(stream, "%lu\t%s\n", (u_long)stats->v, stats->desc);
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
	int ret, tret;

	idb = db->idb;

	WT_DB_FCHK(db, "Db.stat_clear", flags, WT_APIMASK_DB_STAT_CLEAR);

	ret = __wt_stat_clear_db_hstats(db->hstats);
	if ((tret = __wt_stat_clear_db_dstats(db->dstats)) != 0 && ret == 0)
		ret = tret;
	if (idb->fh != NULL &&
	    (tret = __wt_stat_clear_fh_stats(idb->fh->stats))!= 0 && ret == 0)
		ret = tret;

	return (ret);
}
