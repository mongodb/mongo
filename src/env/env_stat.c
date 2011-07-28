/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static void __wt_conn_stat_init(WT_SESSION_IMPL *);

/*
 * __wt_conn_stat_init --
 *	Initialize the Btree statistics.
 */
static void
__wt_conn_stat_init(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

	__wt_cache_stats_update(conn);
}

/*
 * __wt_conn_stat_first --
 *	Initialize a walk of a connection statistics cursor.
 */
int
__wt_conn_stat_first(WT_CURSOR_STAT *cst)
{
	cst->stats = NULL;
	cst->notfound = 0;
	return (__wt_conn_stat_next(cst));
}

/*
 * __wt_conn_stat_next --
 *	Return next entry in a connection statistics cursor.
 */
int
__wt_conn_stat_next(WT_CURSOR_STAT *cst)
{
	WT_CONNECTION_IMPL *conn;
	WT_CURSOR *cursor;
	WT_SESSION_IMPL *session;
	WT_STATS *s;

	cursor = &cst->iface;
	session = (WT_SESSION_IMPL *)cst->iface.session;
	conn = S2C(session);

	if (cst->notfound)
		return (WT_NOTFOUND);
	if (cst->stats == NULL) {
		__wt_conn_stat_init(session);
		cst->stats = (WT_STATS *)conn->stats;
	}
	s = cst->stats++;

	if (s->desc == NULL) {
		cst->notfound = 1;
		return (WT_NOTFOUND);
	}
	WT_RET(__wt_buf_set(session, &cursor->key, s->desc, strlen(s->desc)));
	F_SET(cursor, WT_CURSTD_KEY_SET);
	WT_RET(__wt_buf_set(session, &cursor->value, &s->v, sizeof(s->v)));
	F_SET(cursor, WT_CURSTD_VALUE_SET);

	return (0);
}
