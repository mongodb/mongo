/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_conn_stat_init --
 *	Initialize the per-connection statistics.
 */
void
__wt_conn_stat_init(WT_SESSION_IMPL *session, uint32_t flags)
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

	__wt_cache_stats_update(conn, flags);
}
