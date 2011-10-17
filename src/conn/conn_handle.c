/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_connection_init --
 *	Structure initialization for a just-created WT_CONNECTION_IMPL handle.
 */
int
__wt_connection_init(WT_CONNECTION_IMPL *conn)
{
	WT_SESSION_IMPL *session;

	session = &conn->default_session;

	TAILQ_INIT(&conn->btqh);		/* WT_BTREE list */
	TAILQ_INIT(&conn->dlhqh);		/* Library list */
	TAILQ_INIT(&conn->fhqh);		/* File list */
	TAILQ_INIT(&conn->collqh);		/* Collator list */
	TAILQ_INIT(&conn->compqh);		/* Compressor list */

	/* Statistics. */
	WT_RET(__wt_stat_alloc_connection_stats(session, &conn->stats));

	/* WorkQ spinlock. */
	WT_SPINLOCK_INIT(&conn->workq_lock);

	/*
	 * Connection mutex.
	 *
	 * !!!
	 * Don't allocate the mutex until after we allocate statistics,
	 * the lock functions update the statistics.
	 */
	return (__wt_mtx_alloc(session, "WT_CONNECTION_IMPL", 0, &conn->mtx));
}

/*
 * __wt_connection_destroy --
 *	Destroy the connection's underlying WT_CONNECTION_IMPL structure.
 */
void
__wt_connection_destroy(WT_CONNECTION_IMPL *conn)
{
	WT_SESSION_IMPL *session;

	session = &conn->default_session;

	/* Check there's something to destroy. */
	if (conn == NULL)
		return;

	/*
	 * Close remaining open files (before discarding the mutex, the
	 * underlying file-close code uses the mutex to guard lists of
	 * open files.
	 */
	if (conn->lock_fh != NULL)
		(void)__wt_close(session, conn->lock_fh);

	if (conn->log_fh != NULL)
		(void)__wt_close(session, conn->log_fh);

	if (conn->mtx != NULL)
		(void)__wt_mtx_destroy(session, conn->mtx);

	/* Remove from the list of connections. */
	__wt_lock(session, __wt_process.mtx);
	TAILQ_REMOVE(&__wt_process.connqh, conn, q);
	__wt_unlock(session, __wt_process.mtx);

	/* Free allocated memory. */
	__wt_free(session, conn->home);
	__wt_free(session, conn->sessions);
	__wt_free(session, conn->session_array);
	__wt_free(session, conn->hazard);
	__wt_free(session, conn->stats);

	__wt_free(NULL, conn);
}
