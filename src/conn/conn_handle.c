/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
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

	TAILQ_INIT(&conn->btqh);		/* WT_BTREE list */
	TAILQ_INIT(&conn->dlhqh);		/* Library list */
	TAILQ_INIT(&conn->dsrcqh);		/* Data source list */
	TAILQ_INIT(&conn->fhqh);		/* File list */
	TAILQ_INIT(&conn->collqh);		/* Collator list */
	TAILQ_INIT(&conn->compqh);		/* Compressor list */

	/* Statistics. */
	WT_RET(__wt_stat_alloc_connection_stats(session, &conn->stats));

	/* File handle spinlock. */
	__wt_spin_init(session, &conn->fh_lock);

	/* Serialized function call spinlock. */
	__wt_spin_init(session, &conn->serial_lock);

	/* General purpose spinlock. */
	__wt_spin_init(session, &conn->spinlock);

	/* Checkpoint lock. */
	WT_RET(__wt_rwlock_alloc(session, "checkpoint", &conn->ckpt_rwlock));

	return (0);
}

/*
 * __wt_connection_destroy --
 *	Destroy the connection's underlying WT_CONNECTION_IMPL structure.
 */
void
__wt_connection_destroy(WT_CONNECTION_IMPL *conn)
{
	WT_SESSION_IMPL *session;

	session = conn->default_session;

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

	/* Remove from the list of connections. */
	__wt_spin_lock(session, &__wt_process.spinlock);
	TAILQ_REMOVE(&__wt_process.connqh, conn, q);
	__wt_spin_unlock(session, &__wt_process.spinlock);

	__wt_spin_destroy(session, &conn->fh_lock);
	__wt_spin_destroy(session, &conn->serial_lock);
	__wt_spin_destroy(session, &conn->spinlock);

	if (conn->ckpt_rwlock != NULL)
		(void)__wt_rwlock_destroy(session, conn->ckpt_rwlock);

	/* Free allocated memory. */
	__wt_free(session, conn->home);
	__wt_free(session, conn->sessions);
	__wt_free(session, conn->stats);

	__wt_free(NULL, conn);
}
