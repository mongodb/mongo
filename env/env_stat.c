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
__wt_env_stat_print(ENV *env, FILE *stream)
{
	IDB *idb;
	IENV *ienv;

	ienv = env->ienv;

	fprintf(stream, "Environment handle statistics:\n");
	__wt_stat_print(env, ienv->stats, stream);

	TAILQ_FOREACH(idb, &ienv->dbqh, q)
		WT_RET(__wt_db_stat_print(idb->db, stream));
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

	__wt_stat_clear_ienv_stats(ienv->stats);
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
			fprintf(stream, "%lluB\t%s (%llu)\n",
			    s->v / WT_BILLION, s->desc, s->v);
		else if (s->v >= WT_MILLION)
			fprintf(stream, "%lluM\t%s (%llu)\n",
			    s->v / WT_MILLION, s->desc, s->v);
		else
			fprintf(stream, "%llu\t%s\n", s->v, s->desc);
	fprintf(stream, "%s\n", ienv->sep);
}
