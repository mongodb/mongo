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
__wt_db_stat_print(wt_args_db_stat_print *argp)
{
	wt_args_db_stat_print_unpack;
	IDB *idb;
	WT_STATS *stats;

	idb = db->idb;

	DB_FLAG_CHK(db, "Db.stat_print", flags, WT_APIMASK_DB_STAT_PRINT);

	fprintf(stream, "%s\n", WT_GLOBAL(sep));
	fprintf(stream, "Db: %s\n", db->idb->dbname);
	for (stats = db->stats; stats->desc != NULL; ++stats)
		fprintf(stream, "%lu\t%s\n", (u_long)stats->v, stats->desc);
	if (idb->fh != NULL)
		for (stats = idb->fh->stats; stats->desc != NULL; ++stats)
			fprintf(
			    stream, "%lu\t%s\n", (u_long)stats->v, stats->desc);
	return (0);
}

/*
 * __wt_db_stat_clear --
 *	Clear DB handle statistics.
 */
int
__wt_db_stat_clear(wt_args_db_stat_clear *argp)
{
	wt_args_db_stat_clear_unpack;

	DB_FLAG_CHK(db, "Db.stat_clear", flags, WT_APIMASK_DB_STAT_CLEAR);

	return (__wt_stat_clear_db(db->stats));
}
