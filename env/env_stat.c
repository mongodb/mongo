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
 * __wt_env_stat_print --
 *	Print ENV handle statistics to a stream.
 */
int
__wt_env_stat_print(WT_TOC *toc)
{
	wt_args_env_stat_print_unpack;
	DB *db;
	IDB *idb;
	IENV *ienv;
	WT_SRVR *srvr;
	WT_STATS *stats;

	ienv = env->ienv;

	WT_ENV_FCHK(env, "Env.stat_print", flags, WT_APIMASK_ENV_STAT_PRINT);

	fprintf(stream, "Environment handle statistics:\n");
	for (stats = ienv->stats; stats->desc != NULL; ++stats)
		fprintf(stream, "%llu\t%s\n", stats->v, stats->desc);

	fprintf(stream, "%s\n", ienv->sep);
	srvr = &ienv->psrvr;
	if (srvr->running) {
		fprintf(stream, "Primary server statistics\n");
		for (stats = srvr->stats; stats->desc != NULL; ++stats)
			fprintf(stream, "%llu\t%s\n", stats->v, stats->desc);
	}

	fprintf(stream, "%s\n", ienv->sep);
	TAILQ_FOREACH(idb, &ienv->dbqh, q) {
		db = idb->db;
		WT_RET(db->stat_print(db, toc, stream, flags));
	}
	return (0);
}

/*
 * __wt_env_stat_clear --
 *	Clear ENV handle statistics.
 */
int
__wt_env_stat_clear(WT_TOC *toc)
{
	wt_args_env_stat_clear_unpack;
	DB *db;
	IDB *idb;
	IENV *ienv;
	int ret;

	ienv = env->ienv;
	ret = 0;

	WT_ENV_FCHK(env, "Env.stat_clear", flags, WT_APIMASK_ENV_STAT_CLEAR);

	__wt_stat_clear_srvr_stats(ienv->psrvr.stats);

	TAILQ_FOREACH(idb, &ienv->dbqh, q) {
		db = idb->db;
		WT_TRET(db->stat_clear(db, toc, flags));
	}
	__wt_stat_clear_ienv_stats(ienv->stats);
	return (ret);
}
