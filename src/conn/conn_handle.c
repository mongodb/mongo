/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
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

	session = conn->default_session;

	TAILQ_INIT(&conn->dhqh);		/* Data handle list */
	TAILQ_INIT(&conn->dlhqh);		/* Library list */
	TAILQ_INIT(&conn->dsrcqh);		/* Data source list */
	TAILQ_INIT(&conn->fhqh);		/* File list */
	TAILQ_INIT(&conn->collqh);		/* Collator list */
	TAILQ_INIT(&conn->compqh);		/* Compressor list */

	TAILQ_INIT(&conn->lsmqh);		/* WT_LSM_TREE list */

	/* Configuration. */
	__wt_conn_config_init(conn);

	/* Statistics. */
	__wt_stat_init_connection_stats(&conn->stats);

	/* Locks. */
	__wt_spin_init(session, &conn->api_lock);
	__wt_spin_init(session, &conn->fh_lock);
	__wt_spin_init(session, &conn->metadata_lock);
	__wt_spin_init(session, &conn->schema_lock);
	__wt_spin_init(session, &conn->serial_lock);

	/*
	 * Block manager.
	 * XXX
	 * If there's ever a second block manager, we'll want to make this
	 * more opaque, but for now this is simpler.
	 */
	__wt_spin_init(session, &conn->block_lock);
	TAILQ_INIT(&conn->blockqh);		/* Block manager list */

	return (0);
}

/*
 * __wt_connection_destroy --
 *	Destroy the connection's underlying WT_CONNECTION_IMPL structure.
 */
int
__wt_connection_destroy(WT_CONNECTION_IMPL *conn)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	/* Check there's something to destroy. */
	if (conn == NULL)
		return (0);

	session = conn->default_session;

	/*
	 * Close remaining open files (before discarding the mutex, the
	 * underlying file-close code uses the mutex to guard lists of
	 * open files.
	 */
	if (conn->lock_fh != NULL)
		WT_TRET(__wt_close(session, conn->lock_fh));

	if (conn->log_fh != NULL)
		WT_TRET(__wt_close(session, conn->log_fh));

	/* Remove from the list of connections. */
	__wt_spin_lock(session, &__wt_process.spinlock);
	TAILQ_REMOVE(&__wt_process.connqh, conn, q);
	__wt_spin_unlock(session, &__wt_process.spinlock);

	__wt_spin_destroy(session, &conn->api_lock);
	__wt_spin_destroy(session, &conn->fh_lock);
	__wt_spin_destroy(session, &conn->metadata_lock);
	__wt_spin_destroy(session, &conn->schema_lock);
	__wt_spin_destroy(session, &conn->serial_lock);
	__wt_spin_destroy(session, &conn->block_lock);

	/* Free allocated memory. */
	__wt_free(session, conn->home);
	__wt_free(session, conn->sessions);

	__wt_free(NULL, conn);
	return (ret);
}
