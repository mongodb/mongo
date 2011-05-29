/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_connection_stat_print --
 *	Print WT_CONNECTION_IMPL handle statistics to a stream.
 *
 * XXX this will become a statistics cursor.
 */
int
__wt_connection_stat_print(WT_CONNECTION_IMPL *conn, FILE *stream)
{
	WT_BTREE *btree;
	WT_SESSION_IMPL *session;

	session = &conn->default_session;

	fprintf(stream, "Database statistics:\n");
	__wt_stat_print_conn_stats(conn->stats, stream);
	fprintf(stream, "%s\n", conn->sep);

	fprintf(stream, "Database cache statistics:\n");
	__wt_cache_stats_update(conn);
	__wt_stat_print_cache_stats(conn->cache->stats, stream);
	fprintf(stream, "%s\n", conn->sep);

	TAILQ_FOREACH(btree, &conn->dbqh, q) {
		session->btree = btree;
		WT_RET(__wt_btree_stat_print(session, stream));
	}
	return (0);
}

/*
 * __wt_connection_stat_clear --
 *	Clear WT_CONNECTION_IMPL handle statistics.
 */
int
__wt_connection_stat_clear(WT_CONNECTION_IMPL *conn)
{
	WT_BTREE *btree;
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
		fprintf(stream, "%" PRIu64 "B\t%s (%" PRIu64 " bytes)\n",
		    s->v / WT_BILLION, s->desc, s->v);
	else if (s->v >= WT_MILLION)
		fprintf(stream, "%" PRIu64 "M\t%s (%" PRIu64 " bytes)\n",
		    s->v / WT_MILLION, s->desc, s->v);
	else
		fprintf(stream, "%" PRIu64 "\t%s\n", s->v, s->desc);
}
