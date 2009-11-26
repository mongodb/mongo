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
__wt_db_stat_print(DB *db, FILE *stream, u_int32_t flags)
{
	ENV *env;
	IDB *idb;

	env = db->env;
	idb = db->idb;

	/* Clear the database stats, then call Btree stat to fill them in. */
	__wt_stat_clear_idb_dstats(idb->dstats);
	WT_RET(__wt_bt_stat(db));

	fprintf(stream, "Database statistics: %s\n", idb->dbname);
	__wt_stat_print(env, idb->dstats, stream);

	/* Database handle statistics. */
	fprintf(stream, "Database handle statistics: %s\n", idb->dbname);
	__wt_stat_print(env, idb->stats, stream);

	/* Underlying file handle statistics. */
	if (idb->fh != NULL) {
		fprintf(stream,
		    "Underlying file I/O statistics: %s\n", idb->dbname);
		__wt_stat_print(env, idb->fh->stats, stream);
	}

	return (0);
}

/*
 * __wt_db_stat_clear --
 *	Clear DB handle statistics.
 */
int
__wt_db_stat_clear(DB *db, u_int32_t flags)
{
	IDB *idb;

	idb = db->idb;

	__wt_stat_clear_idb_stats(idb->stats);
	__wt_stat_clear_idb_dstats(idb->dstats);
	if (idb->fh != NULL)
		__wt_stat_clear_fh_stats(idb->fh->stats);

	return (0);
}
