/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_connection_open --
 *	Open a connection.
 */
int
__wt_connection_open(WT_CONNECTION_IMPL *conn, const char *cfg[])
{
	WT_SESSION_IMPL *evict_session, *session;

	/* Default session. */
	session = conn->default_session;
	WT_ASSERT(session, session->iface.connection == &conn->iface);

	/* WT_SESSION_IMPL array. */
	WT_RET(__wt_calloc(session,
	    conn->session_size, sizeof(WT_SESSION_IMPL), &conn->sessions));

	/* Create the cache. */
	WT_RET(__wt_cache_create(conn, cfg));

	/* Initialize transaction support. */
	WT_RET(__wt_txn_global_init(conn, cfg));

	/*
	 * Publish: there must be a barrier to ensure the connection structure
	 * fields are set before other threads read from the pointer.
	 */
	WT_WRITE_BARRIER();

	/*
	 * Open the default session.  We open this before starting service
	 * threads because those may allocate and use session resources that
	 * need to get cleaned up on close.
	 */
	WT_RET(__wt_open_session(conn, 1, NULL, NULL, &session));
	conn->default_session = session;

	/* Start worker threads. */
	F_SET(conn, WT_CONN_EVICTION_RUN | WT_CONN_SERVER_RUN);

	/*
	 * Start the eviction thread.
	 *
	 * It needs a session handle because it is reading/writing pages.
	 * Allocate a session here so the eviction thread never needs
	 * to acquire the connection spinlock, which can lead to deadlock.
	 */
	WT_RET(__wt_open_session(conn, 1, NULL, NULL, &evict_session));
	evict_session->name = "eviction-server";
	WT_RET(__wt_thread_create(session,
	    &conn->cache_evict_tid, __wt_cache_evict_server, evict_session));
	conn->cache_evict_tid_set = 1;

	/*
	 * Start the optional statistics thread.  Start statistics first
	 * so that other optional threads can know whether statistics are
	 * enabled or not.
	 */
	WT_RET(__wt_statlog_create(conn, cfg));

	/* Start the optional logging/archive thread. */
	WT_RET(__wt_logmgr_create(conn, cfg));

	/* Start the optional checkpoint thread. */
	WT_RET(__wt_checkpoint_create(conn, cfg));

	return (0);
}

/*
 * __wt_connection_close --
 *	Close a connection handle.
 */
int
__wt_connection_close(WT_CONNECTION_IMPL *conn)
{
	WT_CONNECTION *wt_conn;
	WT_DECL_RET;
	WT_DLH *dlh;
	WT_FH *fh;
	WT_NAMED_COLLATOR *ncoll;
	WT_NAMED_COMPRESSOR *ncomp;
	WT_NAMED_DATA_SOURCE *ndsrc;
	WT_SESSION_IMPL *session;

	wt_conn = &conn->iface;
	session = conn->default_session;

	__wt_txn_refresh_force(session);

	/*
	 * Shut down server threads other than the eviction server, which is
	 * needed later to close btree handles.  Some of these threads access
	 * btree handles, so take care in ordering shutdown to make sure they
	 * exit before files are closed.
	 */
	F_CLR(conn, WT_CONN_SERVER_RUN);
	WT_TRET(__wt_logmgr_destroy(conn));
	WT_TRET(__wt_checkpoint_destroy(conn));
	WT_TRET(__wt_statlog_destroy(conn));

	/* Clean up open LSM handles. */
	WT_TRET(__wt_lsm_tree_close_all(session));

	/* Close open data handles. */
	WT_TRET(__wt_conn_dhandle_discard(conn));

	/* Free memory for collators */
	while ((ncoll = TAILQ_FIRST(&conn->collqh)) != NULL)
		WT_TRET(__wt_conn_remove_collator(conn, ncoll));

	/* Free memory for compressors */
	while ((ncomp = TAILQ_FIRST(&conn->compqh)) != NULL)
		WT_TRET(__wt_conn_remove_compressor(conn, ncomp));

	/* Free memory for data sources */
	while ((ndsrc = TAILQ_FIRST(&conn->dsrcqh)) != NULL)
		WT_TRET(__wt_conn_remove_data_source(conn, ndsrc));

	/*
	 * Complain if files weren't closed, ignoring the lock and logging
	 * files, we'll close them in a minute.
	 */
	TAILQ_FOREACH(fh, &conn->fhqh, q) {
		if (fh == conn->lock_fh)
			continue;

		__wt_errx(session,
		    "Connection has open file handles: %s", fh->name);
		WT_TRET(__wt_close(session, fh));
		fh = TAILQ_FIRST(&conn->fhqh);
	}

	/* Shut down the eviction server thread. */
	F_CLR(conn, WT_CONN_EVICTION_RUN);
	if (conn->cache_evict_tid_set) {
		WT_TRET(__wt_evict_server_wake(session));
		WT_TRET(__wt_thread_join(session, conn->cache_evict_tid));
		conn->cache_evict_tid_set = 0;
	}

	/* Disconnect from shared cache - must be before cache destroy. */
	WT_TRET(__wt_conn_cache_pool_destroy(conn));

	/* Discard the cache. */
	WT_TRET(__wt_cache_destroy(conn));

	/* Discard transaction state. */
	__wt_txn_global_destroy(conn);

	/* Close extensions, first calling any unload entry point. */
	while ((dlh = TAILQ_FIRST(&conn->dlhqh)) != NULL) {
		TAILQ_REMOVE(&conn->dlhqh, dlh, q);

		if (dlh->terminate != NULL)
			WT_TRET(dlh->terminate(wt_conn));
		WT_TRET(__wt_dlclose(session, dlh));
	}

	/*
	 * Close the internal (default) session, and switch back to the dummy
	 * session in case of any error messages from the remaining operations
	 * while destroying the connection handle.
	 *
	 * Additionally, the session's hazard pointer memory isn't discarded
	 * during normal session close because access to it isn't serialized.
	 * Discard it now.
	 */
	if (session != &conn->dummy_session) {
		WT_TRET(session->iface.close(&session->iface, NULL));
		__wt_free(&conn->dummy_session, session->hazard);

		conn->default_session = &conn->dummy_session;
	}

	/* Destroy the handle. */
	WT_TRET(__wt_connection_destroy(conn));

	return (ret);
}
