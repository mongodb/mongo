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
__wt_env_stat_print(ENV *env, FILE *fp, u_int32_t flags)
{
	DB *db;
	WT_STATS *stats;

	ENV_FLAG_CHK(env, "Env.stat_print", flags, WT_APIMASK_ENV_STAT_PRINT);

	fprintf(fp, "Environment statistics:\n");
	for (stats = env->stats; stats->desc != NULL; ++stats)
		fprintf(fp, "%lu\t%s\n", (u_long)stats->v, stats->desc);

	TAILQ_FOREACH(db, &env->dbqh, q)
		__wt_db_stat_print(db, fp, flags);
	return (0);
}

/*
 * __wt_env_stat_clear --
 *	Clear ENV handle statistics.
 */
int
__wt_env_stat_clear(ENV *env, u_int32_t flags)
{
	DB *db;

	ENV_FLAG_CHK(env, "Env.stat_clear", flags, WT_APIMASK_ENV_STAT_CLEAR);

	TAILQ_FOREACH(db, &env->dbqh, q)
		__wt_db_stat_clear(db, flags);
	return (__wt_stat_clear_env(env->stats));
}
