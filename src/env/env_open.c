/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_connection_open --
 *	Open a Env handle.
 */
int
__wt_connection_open(CONNECTION *conn, const char *home, mode_t mode)
{
	SESSION *session;
	int ret;

	WT_UNUSED(home);
	WT_UNUSED(mode);

	/* Default session. */
	conn->default_session.iface.connection = &conn->iface;

	session = &conn->default_session;
	ret = 0;

	/* SESSION and hazard arrays. */
	WT_RET(__wt_calloc(session,
	    conn->session_size, sizeof(SESSION *), &conn->sessions));
	WT_RET(__wt_calloc(session,
	    conn->session_size, sizeof(SESSION), &conn->toc_array));
	WT_RET(__wt_calloc(session,
	   conn->session_size * conn->hazard_size, sizeof(WT_HAZARD),
	   &conn->hazard));

	/* Create the cache. */
	WT_RET(__wt_cache_create(conn));

	/* Transition to the open state. */
	__wt_methods_connection_open_transition(conn);

	/* Start worker threads. */
	F_SET(conn, WT_WORKQ_RUN | WT_SERVER_RUN);
	WT_MEMORY_FLUSH;

	WT_ERR(__wt_thread_create(
	    &conn->cache_evict_tid, __wt_cache_evict_server, conn));
	WT_ERR(__wt_thread_create(
	    &conn->cache_read_tid, __wt_cache_read_server, conn));
	WT_ERR(__wt_thread_create(&conn->workq_tid, __wt_workq_srvr, conn));

	return (0);

err:	(void)__wt_connection_close(conn);
	return (ret);
}

/*
 * __wt_connection_close --
 *	Close an Env handle.
 */
int
__wt_connection_close(CONNECTION *conn)
{
	BTREE *btree;
	WT_FH *fh;
	int ret, secondary_err;

	WT_CONN_FCHK_RET(conn, "Env.close", conn->flags, WT_APIMASK_CONN, ret);

	ret = secondary_err = 0;

	/* Complain if BTREE handles weren't closed. */
	while ((btree = TAILQ_FIRST(&conn->dbqh)) != NULL) {
		__wt_errx(&conn->default_session,
		    "Env handle has open Db handles: %s", btree->name);
		WT_TRET(btree->close(btree, &conn->default_session, 0));
		secondary_err = WT_ERROR;
	}

	/* Complain if files weren't closed. */
	while ((fh = TAILQ_FIRST(&conn->fhqh)) != NULL) {
		__wt_errx(&conn->default_session,
		    "Env handle has open file handles: %s", fh->name);
		WT_TRET(__wt_close(&conn->default_session, fh));
		secondary_err = WT_ERROR;
	}

	/* Shut down the server threads. */
	F_CLR(conn, WT_SERVER_RUN);
	WT_MEMORY_FLUSH;

	/*
	 * Force the cache server threads to run and wait for them to exit.
	 * Wait for the cache eviction server first, it potentially schedules
	 * work for the read thread.
	 */
	__wt_workq_evict_server(conn, 1);
	__wt_thread_join(conn->cache_evict_tid);
	__wt_workq_read_server(conn, 1);
	__wt_thread_join(conn->cache_read_tid);

	/*
	 * Close down and wait for the workQ thread; this only happens after
	 * all other server threads have exited, as they may be waiting on a
	 * request from the workQ, or vice-versa.
	 */
	F_CLR(conn, WT_WORKQ_RUN);
	WT_MEMORY_FLUSH;
	__wt_thread_join(conn->workq_tid);

	/* Discard the cache. */
	WT_TRET(__wt_cache_destroy(conn));

	/* Destroy the handle. */
	WT_TRET(__wt_connection_destroy(conn));

	if (ret == 0)
		ret = secondary_err;

	return (ret == 0 ? secondary_err : ret);
}
