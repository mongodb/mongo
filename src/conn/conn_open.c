/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
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
	WT_SESSION_IMPL *session;

	/* Default session. */
	session = conn->default_session;
	WT_ASSERT(session, session->iface.connection == &conn->iface);

	/*
	 * Tell internal server threads to run: this must be set before opening
	 * any sessions.
	 */
	F_SET(conn, WT_CONN_SERVER_RUN | WT_CONN_LOG_SERVER_RUN);

	/* WT_SESSION_IMPL array. */
	WT_RET(__wt_calloc(session,
	    conn->session_size, sizeof(WT_SESSION_IMPL), &conn->sessions));
	WT_CACHE_LINE_ALIGNMENT_VERIFY(session, conn->sessions);

	/*
	 * Open the default session.  We open this before starting service
	 * threads because those may allocate and use session resources that
	 * need to get cleaned up on close.
	 */
	WT_RET(__wt_open_internal_session(
	    conn, "connection", false, 0, &session));

	/*
	 * The connection's default session is originally a static structure,
	 * swap that out for a more fully-functional session.  It's necessary
	 * to have this step: the session allocation code uses the connection's
	 * session, and if we pass a reference to the default session as the
	 * place to store the allocated session, things get confused and error
	 * handling can be corrupted.  So, we allocate into a stack variable
	 * and then assign it on success.
	 */
	conn->default_session = session;

	/*
	 * Publish: there must be a barrier to ensure the connection structure
	 * fields are set before other threads read from the pointer.
	 */
	WT_WRITE_BARRIER();

	/* Create the cache. */
	WT_RET(__wt_cache_create(session, cfg));

	/* Initialize transaction support. */
	WT_RET(__wt_txn_global_init(session, cfg));

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
	WT_SESSION_IMPL *s, *session;
	WT_TXN_GLOBAL *txn_global;
	u_int i;

	wt_conn = &conn->iface;
	txn_global = &conn->txn_global;
	session = conn->default_session;

	/*
	 * We're shutting down.  Make sure everything gets freed.
	 *
	 * It's possible that the eviction server is in the middle of a long
	 * operation, with a transaction ID pinned.  In that case, we will loop
	 * here until the transaction ID is released, when the oldest
	 * transaction ID will catch up with the current ID.
	 */
	for (;;) {
		WT_TRET(__wt_txn_update_oldest(session,
		    WT_TXN_OLDEST_STRICT | WT_TXN_OLDEST_WAIT));
		if (txn_global->oldest_id == txn_global->current)
			break;
		__wt_yield();
	}

	/* Clear any pending async ops. */
	WT_TRET(__wt_async_flush(session));

	/*
	 * Shut down server threads other than the eviction server, which is
	 * needed later to close btree handles.  Some of these threads access
	 * btree handles, so take care in ordering shutdown to make sure they
	 * exit before files are closed.
	 */
	F_CLR(conn, WT_CONN_SERVER_RUN);
	WT_TRET(__wt_async_destroy(session));
	WT_TRET(__wt_lsm_manager_destroy(session));
	WT_TRET(__wt_sweep_destroy(session));

	F_SET(conn, WT_CONN_CLOSING);

	WT_TRET(__wt_checkpoint_server_destroy(session));
	WT_TRET(__wt_statlog_destroy(session, true));
	WT_TRET(__wt_evict_destroy(session));

	/* Shut down the lookaside table, after all eviction is complete. */
	WT_TRET(__wt_las_destroy(session));

	/* Close open data handles. */
	WT_TRET(__wt_conn_dhandle_discard(session));

	/* Shut down metadata tracking, required before creating tables. */
	WT_TRET(__wt_meta_track_destroy(session));

	/*
	 * Now that all data handles are closed, tell logging that a checkpoint
	 * has completed then shut down the log manager (only after closing
	 * data handles).  The call to destroy the log manager is outside the
	 * conditional because we allocate the log path so that printlog can
	 * run without running logging or recovery.
	 */
	if (FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED) &&
	    FLD_ISSET(conn->log_flags, WT_CONN_LOG_RECOVER_DONE))
		WT_TRET(__wt_txn_checkpoint_log(
		    session, true, WT_TXN_LOG_CKPT_STOP, NULL));
	F_CLR(conn, WT_CONN_LOG_SERVER_RUN);
	WT_TRET(__wt_logmgr_destroy(session));

	/* Free memory for collators, compressors, data sources. */
	WT_TRET(__wt_conn_remove_collator(session));
	WT_TRET(__wt_conn_remove_compressor(session));
	WT_TRET(__wt_conn_remove_data_source(session));
	WT_TRET(__wt_conn_remove_encryptor(session));
	WT_TRET(__wt_conn_remove_extractor(session));

	/* Disconnect from shared cache - must be before cache destroy. */
	WT_TRET(__wt_conn_cache_pool_destroy(session));

	/* Discard the cache. */
	WT_TRET(__wt_cache_destroy(session));

	/* Discard transaction state. */
	WT_TRET(__wt_txn_global_destroy(session));

	/* Close extensions, first calling any unload entry point. */
	while ((dlh = TAILQ_FIRST(&conn->dlhqh)) != NULL) {
		TAILQ_REMOVE(&conn->dlhqh, dlh, q);

		if (dlh->terminate != NULL)
			WT_TRET(dlh->terminate(wt_conn));
		WT_TRET(__wt_dlclose(session, dlh));
	}

	/* Close the lock file, opening up the database to other connections. */
	if (conn->lock_fh != NULL)
		WT_TRET(__wt_close(session, &conn->lock_fh));

	/* Close any file handles left open. */
	WT_TRET(__wt_close_connection_close(session));

	/*
	 * Close the internal (default) session, and switch back to the dummy
	 * session in case of any error messages from the remaining operations
	 * while destroying the connection handle.
	 */
	if (session != &conn->dummy_session) {
		WT_TRET(session->iface.close(&session->iface, NULL));
		session = conn->default_session = &conn->dummy_session;
	}

	/*
	 * The session's split stash isn't discarded during normal session close
	 * because it may persist past the life of the session.  Discard it now.
	 */
	if ((s = conn->sessions) != NULL)
		for (i = 0; i < conn->session_size; ++s, ++i)
			__wt_split_stash_discard_all(session, s);

	/*
	 * The session's hazard pointer memory isn't discarded during normal
	 * session close because access to it isn't serialized.  Discard it
	 * now.
	 */
	if ((s = conn->sessions) != NULL)
		for (i = 0; i < conn->session_size; ++s, ++i) {
			/*
			 * If hash arrays were allocated, free them now.
			 */
			__wt_free(session, s->dhhash);
			__wt_free(session, s->tablehash);
			__wt_free(session, s->hazard);
		}

	/* Destroy the handle. */
	WT_TRET(__wt_connection_destroy(conn));

	return (ret);
}

/*
 * __wt_connection_workers --
 *	Start the worker threads.
 */
int
__wt_connection_workers(WT_SESSION_IMPL *session, const char *cfg[])
{
	/*
	 * Start the optional statistics thread.  Start statistics first so that
	 * other optional threads can know if statistics are enabled or not.
	 */
	WT_RET(__wt_statlog_create(session, cfg));
	WT_RET(__wt_logmgr_create(session, cfg));

	/*
	 * Run recovery.
	 * NOTE: This call will start (and stop) eviction if recovery is
	 * required.  Recovery must run before the lookaside table is created
	 * (because recovery will update the metadata), and before eviction is
	 * started for real.
	 */
	WT_RET(__wt_txn_recover(session));

	/*
	 * Start the optional logging/archive threads.
	 * NOTE: The log manager must be started before checkpoints so that the
	 * checkpoint server knows if logging is enabled.  It must also be
	 * started before any operation that can commit, or the commit can
	 * block.
	 */
	WT_RET(__wt_logmgr_open(session));

	/* Initialize metadata tracking, required before creating tables. */
	WT_RET(__wt_meta_track_init(session));

	/* Create the lookaside table. */
	WT_RET(__wt_las_create(session));

	/*
	 * Start eviction threads.
	 * NOTE: Eviction must be started after the lookaside table is created.
	 */
	WT_RET(__wt_evict_create(session));

	/* Start the handle sweep thread. */
	WT_RET(__wt_sweep_create(session));

	/* Start the optional async threads. */
	WT_RET(__wt_async_create(session, cfg));

	/* Start the optional checkpoint thread. */
	WT_RET(__wt_checkpoint_server_create(session, cfg));

	return (0);
}
