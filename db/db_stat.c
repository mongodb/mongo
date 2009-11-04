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
 * __wt_api_db_stat_print --
 *	Print DB handle statistics to a stream.
 */
int
__wt_api_db_stat_print(DB *db, FILE *stream, u_int32_t flags)
{
	IDB *idb;
	IENV *ienv;
	WT_STATS *stats;

	idb = db->idb;
	ienv = db->env->ienv;

	WT_DB_FCHK(db, "Db.stat_print", flags, WT_APIMASK_DB_STAT_PRINT);

	fprintf(stream, "Database statistics: %s\n", idb->dbname);

	/* Clear the database stats, then call Btree stat to fill them in. */
	__wt_stat_clear_idb_dstats(idb->dstats);
	WT_RET(__wt_bt_stat(db));

	for (stats = idb->dstats; stats->desc != NULL; ++stats)
		WT_STAT_FIELD_PRINT(stream, stats);

	/* Database handle statistics. */
	fprintf(stream, "%s\n", ienv->sep);
	fprintf(stream, "Database handle statistics: %s\n", idb->dbname);
	for (stats = idb->stats; stats->desc != NULL; ++stats)
		WT_STAT_FIELD_PRINT(stream, stats);

	/* Underlying file handle statistics. */
	if (idb->fh != NULL) {
		fprintf(stream, "%s\n", ienv->sep);
		fprintf(stream,
		    "Underlying file I/O statistics: %s\n", idb->dbname);
		for (stats = idb->fh->stats; stats->desc != NULL; ++stats)
			WT_STAT_FIELD_PRINT(stream, stats);
	}

	return (0);
}

/*
 * __wt_api_db_stat_clear --
 *	Clear DB handle statistics.
 */
int
__wt_api_db_stat_clear(DB *db, u_int32_t flags)
{
	IDB *idb;

	idb = db->idb;

	WT_DB_FCHK(db, "Db.stat_clear", flags, WT_APIMASK_DB_STAT_CLEAR);

	__wt_stat_clear_idb_stats(idb->stats);
	__wt_stat_clear_idb_dstats(idb->dstats);
	if (idb->fh != NULL)
		__wt_stat_clear_fh_stats(idb->fh->stats);

	return (0);
}
