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
 * __wt_api_env_stat_print --
 *	Print ENV handle statistics to a stream.
 */
int
__wt_api_env_stat_print(ENV *env, FILE *stream, u_int32_t flags)
{
	IDB *idb;
	IENV *ienv;
	WT_STATS *stats;

	ienv = env->ienv;

	WT_ENV_FCHK(env, "Env.stat_print", flags, WT_APIMASK_ENV_STAT_PRINT);

	fprintf(stream, "Environment handle statistics:\n");
	for (stats = ienv->stats; stats->desc != NULL; ++stats)
		WT_STAT_FIELD_PRINT(stream, stats);

	fprintf(stream, "%s\n", ienv->sep);
	TAILQ_FOREACH(idb, &ienv->dbqh, q)
		if (!F_ISSET(idb, WT_INVALID))
			WT_RET(idb->db->stat_print(idb->db, stream, flags));
	return (0);
}

/*
 * __wt_api_env_stat_clear --
 *	Clear ENV handle statistics.
 */
int
__wt_api_env_stat_clear(ENV *env, u_int32_t flags)
{
	DB *db;
	IDB *idb;
	IENV *ienv;
	int ret;

	ienv = env->ienv;
	ret = 0;

	WT_ENV_FCHK(env, "Env.stat_clear", flags, WT_APIMASK_ENV_STAT_CLEAR);

	TAILQ_FOREACH(idb, &ienv->dbqh, q)
		if (!F_ISSET(idb, WT_INVALID)) {
			db = idb->db;
			WT_TRET(db->stat_clear(db, flags));
		}

	__wt_stat_clear_ienv_stats(ienv->stats);
	return (ret);
}
