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
__wt_env_stat_print(WT_STOC *stoc)
{
	wt_args_env_stat_print_unpack;
	DB *db;
	IENV *ienv;
	WT_STATS *stats;
	WT_STOC *tmp_stoc;
	int i;

	ienv = env->ienv;

	WT_ENV_FCHK(env, "Env.stat_print", flags, WT_APIMASK_ENV_STAT_PRINT);

	fprintf(stream, "Environment handle statistics:\n");
	for (stats = env->hstats; stats->desc != NULL; ++stats)
		fprintf(stream, "%llu\t%s\n", stats->v, stats->desc);

	fprintf(stream, "%s\n", ienv->sep);
	WT_STOC_FOREACH(ienv, tmp_stoc, i) {
		if (!tmp_stoc->running)
			continue;
		fprintf(stream, "Server #%d thread statistics\n", tmp_stoc->id);
		for (stats = tmp_stoc->stats; stats->desc != NULL; ++stats)
			fprintf(stream, "%llu\t%s\n", stats->v, stats->desc);
	}

	fprintf(stream, "%s\n", ienv->sep);
	TAILQ_FOREACH(db, &env->dbqh, q)
		WT_RET(db->stat_print(db, stoc->toc, stream, flags));
	return (0);
}

/*
 * __wt_env_stat_clear --
 *	Clear ENV handle statistics.
 */
int
__wt_env_stat_clear(WT_STOC *stoc)
{
	wt_args_env_stat_clear_unpack;
	DB *db;
	int ret;

	WT_ENV_FCHK(env, "Env.stat_clear", flags, WT_APIMASK_ENV_STAT_CLEAR);

	ret = 0;
	TAILQ_FOREACH(db, &env->dbqh, q)
		WT_TRET(db->stat_clear(db, stoc->toc, flags));
	WT_TRET(__wt_stat_clear_env_hstats(env->hstats));
	return (ret);
}
