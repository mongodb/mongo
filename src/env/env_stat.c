/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_env_stat_print --
 *	Print ENV handle statistics to a stream.
 */
int
__wt_env_stat_print(ENV *env, FILE *stream)
{
	IDB *idb;
	IENV *ienv;

	ienv = env->ienv;

	fprintf(stream, "Environment handle statistics:\n");
	__wt_stat_print(env, ienv->stats, stream);

	fprintf(stream, "Environment cache statistics:\n");
	__wt_cache_stats(env);
	__wt_stat_print(env, ienv->cache->stats, stream);
	fprintf(stream, "Environment method statistics:\n");
	__wt_stat_print(env, ienv->method_stats, stream);

	TAILQ_FOREACH(idb, &ienv->dbqh, q)
		WT_RET(idb->db->stat_print(idb->db, stream, 0));
	return (0);
}

/*
 * __wt_env_stat_clear --
 *	Clear ENV handle statistics.
 */
int
__wt_env_stat_clear(ENV *env)
{
	IDB *idb;
	IENV *ienv;
	int ret;

	ienv = env->ienv;
	ret = 0;

	TAILQ_FOREACH(idb, &ienv->dbqh, q)
		WT_TRET(__wt_db_stat_clear(idb->db));

	__wt_stat_clear_env_stats(ienv->stats);
	__wt_stat_clear_cache_stats(ienv->cache->stats);
	__wt_stat_clear_method_stats(ienv->method_stats);

	return (ret);
}

/*
 * __wt_stat_print --
 *	Print out a statistics table.
 */
void
__wt_stat_print(ENV *env, WT_STATS *s, FILE *stream)
{
	IENV *ienv;

	ienv = env->ienv;

	for (; s->desc != NULL; ++s)
		if (s->v >= WT_BILLION)
			fprintf(stream, "%lluB\t%s (%llu bytes)\n",
			    (unsigned long long)s->v / WT_BILLION,
			    s->desc, (unsigned long long)s->v);
		else if (s->v >= WT_MILLION)
			fprintf(stream, "%lluM\t%s (%llu bytes)\n",
			    (unsigned long long)s->v / WT_MILLION,
			    s->desc, (unsigned long long)s->v);
		else
			fprintf(stream,
			    "%llu\t%s\n", (unsigned long long)s->v, s->desc);
	fprintf(stream, "%s\n", ienv->sep);
}
