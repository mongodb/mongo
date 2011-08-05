/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_connection_open --
 *	Open a connection.
 */
int
__wt_connection_open(WT_CONNECTION_IMPL *conn, const char *home, mode_t mode)
{
	WT_SESSION_IMPL *session;
	int ret;

	WT_UNUSED(home);
	WT_UNUSED(mode);

	/* Default session. */
	conn->default_session.iface.connection = &conn->iface;

	session = &conn->default_session;
	ret = 0;

	/* WT_SESSION_IMPL and hazard arrays. */
	WT_RET(__wt_calloc(session,
	    conn->session_size, sizeof(WT_SESSION_IMPL *), &conn->sessions));
	WT_RET(__wt_calloc(session,
	    conn->session_size, sizeof(WT_SESSION_IMPL),
	    &conn->session_array));
	WT_RET(__wt_calloc(session,
	   conn->session_size * conn->hazard_size, sizeof(WT_HAZARD),
	   &conn->hazard));

	/* Create the cache. */
	WT_RET(__wt_cache_create(conn));

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
 *	Close a connection handle.
 */
int
__wt_connection_close(WT_CONNECTION_IMPL *conn)
{
	WT_BTREE *btree;
	WT_SESSION_IMPL *session;
	WT_DLH *dlh;
	WT_FH *fh;
	int ret, secondary_err;

	session = &conn->default_session;
	ret = secondary_err = 0;

	/* Complain if WT_BTREE handles weren't closed. */
	while ((btree = TAILQ_FIRST(&conn->dbqh)) != NULL) {
		__wt_errx(session,
		    "Connection has open btree handles: %s", btree->name);
		session->btree = btree;
		WT_TRET(__wt_btree_close(session));
		secondary_err = WT_ERROR;
	}

	/* Complain if files weren't closed. */
	while ((fh = TAILQ_FIRST(&conn->fhqh)) != NULL && fh != conn->log_fh) {
		__wt_errx(session,
		    "connection has open file handles: %s", fh->name);
		WT_TRET(__wt_close(session, fh));
		secondary_err = WT_ERROR;
	}

	/* Shut down the server threads. */
	F_CLR(conn, WT_SERVER_RUN);
	WT_MEMORY_FLUSH;

	/* Force the cache server threads to run and wait for them to exit. */
	__wt_workq_evict_server_exit(conn);
	__wt_thread_join(conn->cache_evict_tid);
	__wt_workq_read_server_exit(conn);
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
	__wt_cache_destroy(conn);

	/* Close extensions. */
	while ((dlh = TAILQ_FIRST(&conn->dlhqh)) != NULL)
		WT_TRET(__wt_dlclose(session, dlh));

	if (conn->log_fh != NULL) {
		WT_TRET(__wt_close(session, conn->log_fh));
		conn->log_fh = NULL;
	}

	/* Destroy the handle. */
	WT_TRET(__wt_connection_destroy(conn));

	if (ret == 0)
		ret = secondary_err;

	return ((ret == 0) ? secondary_err : ret);
}
