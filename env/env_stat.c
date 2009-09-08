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
	IENV *ienv;
	WT_STATS *stats;
	WT_STOC *stoc;
	u_int i;

	ienv = env->ienv;

	WT_ENV_FCHK(env, "Env.stat_print", flags, WT_APIMASK_ENV_STAT_PRINT);

	fprintf(stream, "Environment handle statistics:\n");
	for (stats = env->hstats; stats->desc != NULL; ++stats)
		fprintf(stream, "%llu\t%s\n", stats->v, stats->desc);

	fprintf(stream, "%s\n", ienv->sep);
	WT_STOC_FOREACH(ienv, stoc, i) {
		if (!stoc->running)
			continue;
		fprintf(stream, "Server #%d thread statistics\n", stoc->id);
		for (stats = stoc->stats; stats->desc != NULL; ++stats)
			fprintf(stream, "%llu\t%s\n", stats->v, stats->desc);
	}

	fprintf(stream, "%s\n", ienv->sep);
	TAILQ_FOREACH(db, &env->dbqh, q)
		WT_RET(db->stat_print(db, toc, stream, flags));
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
	int ret;

	WT_ENV_FCHK(env, "Env.stat_clear", flags, WT_APIMASK_ENV_STAT_CLEAR);

	ret = 0;
	TAILQ_FOREACH(db, &env->dbqh, q)
		WT_TRET(db->stat_clear(db, toc, flags));
	WT_TRET(__wt_stat_clear_env_hstats(env->hstats));
	return (ret);
}
