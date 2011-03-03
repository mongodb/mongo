/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_connection_stat_print --
 *	Print CONNECTION handle statistics to a stream.
 */
int
__wt_connection_stat_print(CONNECTION *conn, FILE *stream)
{
	BTREE *btree;

	fprintf(stream, "Environment handle statistics:\n");
	__wt_stat_print(conn, conn->stats, stream);

	fprintf(stream, "Environment cache statistics:\n");
	__wt_cache_stats(conn);
	__wt_stat_print(conn, conn->cache->stats, stream);
	fprintf(stream, "Environment method statistics:\n");
	__wt_stat_print(conn, conn->method_stats, stream);

	TAILQ_FOREACH(btree, &conn->dbqh, q)
		WT_RET(btree->stat_print(btree, stream, 0));
	return (0);
}

/*
 * __wt_connection_stat_clear --
 *	Clear CONNECTION handle statistics.
 */
int
__wt_connection_stat_clear(CONNECTION *conn)
{
	BTREE *btree;
	int ret;

	ret = 0;

	TAILQ_FOREACH(btree, &conn->dbqh, q)
		WT_TRET(__wt_btree_stat_clear(btree));

	__wt_stat_clear_connection_stats(conn->stats);
	__wt_stat_clear_cache_stats(conn->cache->stats);
	__wt_stat_clear_method_stats(conn->method_stats);

	return (ret);
}

/*
 * __wt_stat_print --
 *	Print out a statistics table.
 */
void
__wt_stat_print(CONNECTION *conn, WT_STATS *s, FILE *stream)
{
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
	fprintf(stream, "%s\n", conn->sep);
}
