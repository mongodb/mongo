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
__wt_db_stat_print(DB *db, FILE *fp, u_int32_t flags)
{
	WT_STATS *stats;

	DB_FLAG_CHK(db, "Db.stat_print", flags, WT_APIMASK_DB_STAT_PRINT);

	for (stats = db->stats; stats->desc != NULL; ++stats)
		fprintf(fp, "%lu\t%s\n", (u_long)stats->v, stats->desc);
	return (0);
}

/*
 * __wt_db_stat_clear --
 *	Clear DB handle statistics.
 */
int
__wt_db_stat_clear(DB *db, u_int32_t flags)
{
	DB_FLAG_CHK(db, "Db.stat_clear", flags, WT_APIMASK_DB_STAT_CLEAR);

	return (__wt_stat_clear_db(db->stats));
}
