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

	fprintf(stream, "Database statistics:\n");
	__wt_stat_print_conn_stats(conn->stats, stream);
	fprintf(stream, "%s\n", conn->sep);

	fprintf(stream, "Database cache statistics:\n");
	__wt_cache_stats_update(conn);
	__wt_stat_print_cache_stats(conn->cache->stats, stream);
	fprintf(stream, "%s\n", conn->sep);

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

	__wt_stat_clear_conn_stats(conn->stats);
	__wt_stat_clear_cache_stats(conn->cache->stats);

	return (ret);
}

/*
 * __wt_stat_print --
 *	Print out a statistics table value.
 */
void
__wt_stat_print(WT_STATS *s, FILE *stream)
{
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
}
