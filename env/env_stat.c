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
__wt_env_stat_print(wt_args_env_stat_print *argp)
{
	wt_args_env_stat_print_unpack;
	DB *db;
	WT_STATS *stats;
	int ret;

	ENV_FLAG_CHK(env, "Env.stat_print", flags, WT_APIMASK_ENV_STAT_PRINT);

	fprintf(stream, "Environment handle statistics:\n");
	for (stats = env->hstats; stats->desc != NULL; ++stats)
		fprintf(stream, "%lu\t%s\n", (u_long)stats->v, stats->desc);

	TAILQ_FOREACH(db, &env->dbqh, q)
		if ((ret = db->stat_print(db, stream, flags)) != 0)
			return (ret);
	return (0);
}

/*
 * __wt_env_stat_clear --
 *	Clear ENV handle statistics.
 */
int
__wt_env_stat_clear(wt_args_env_stat_clear *argp)
{
	wt_args_env_stat_clear_unpack;
	DB *db;
	int ret, tret;

	ENV_FLAG_CHK(env, "Env.stat_clear", flags, WT_APIMASK_ENV_STAT_CLEAR);

	ret = 0;
	TAILQ_FOREACH(db, &env->dbqh, q)
		if ((tret = db->stat_clear(db, flags)) != 0 && ret == 0)
			ret = tret;
	if ((tret = __wt_stat_clear_env_hstats(env->hstats)) != 0 && ret == 0)
		ret = tret;
	return (ret);
}
