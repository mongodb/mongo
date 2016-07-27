/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
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
	u_int i;

	session = conn->default_session;

	for (i = 0; i < WT_HASH_ARRAY_SIZE; i++) {
		TAILQ_INIT(&conn->dhhash[i]);	/* Data handle hash lists */
		TAILQ_INIT(&conn->fhhash[i]);	/* File handle hash lists */
	}

	TAILQ_INIT(&conn->dhqh);		/* Data handle list */
	TAILQ_INIT(&conn->dlhqh);		/* Library list */
	TAILQ_INIT(&conn->dsrcqh);		/* Data source list */
	TAILQ_INIT(&conn->fhqh);		/* File list */
	TAILQ_INIT(&conn->collqh);		/* Collator list */
	TAILQ_INIT(&conn->compqh);		/* Compressor list */
	TAILQ_INIT(&conn->encryptqh);		/* Encryptor list */
	TAILQ_INIT(&conn->extractorqh);		/* Extractor list */

	TAILQ_INIT(&conn->lsmqh);		/* WT_LSM_TREE list */

	/* Setup the LSM work queues. */
	TAILQ_INIT(&conn->lsm_manager.switchqh);
	TAILQ_INIT(&conn->lsm_manager.appqh);
	TAILQ_INIT(&conn->lsm_manager.managerqh);

	/* Random numbers. */
	__wt_random_init(&session->rnd);

	/* Configuration. */
	WT_RET(__wt_conn_config_init(session));

	/* Statistics. */
	__wt_stat_connection_init(conn);

	/* Locks. */
	WT_RET(__wt_spin_init(session, &conn->api_lock, "api"));
	WT_RET(__wt_spin_init(session, &conn->checkpoint_lock, "checkpoint"));
	WT_RET(__wt_spin_init(session, &conn->dhandle_lock, "data handle"));
	WT_RET(__wt_spin_init(session, &conn->encryptor_lock, "encryptor"));
	WT_RET(__wt_spin_init(session, &conn->fh_lock, "file list"));
	WT_RET(__wt_rwlock_alloc(session,
	    &conn->hot_backup_lock, "hot backup"));
	WT_RET(__wt_spin_init(session, &conn->las_lock, "lookaside table"));
	WT_RET(__wt_spin_init(session, &conn->metadata_lock, "metadata"));
	WT_RET(__wt_spin_init(session, &conn->reconfig_lock, "reconfigure"));
	WT_RET(__wt_spin_init(session, &conn->schema_lock, "schema"));
	WT_RET(__wt_spin_init(session, &conn->table_lock, "table creation"));
	WT_RET(__wt_spin_init(session, &conn->turtle_lock, "turtle file"));

	WT_RET(__wt_calloc_def(session, WT_PAGE_LOCKS, &conn->page_lock));
	WT_CACHE_LINE_ALIGNMENT_VERIFY(session, conn->page_lock);
	for (i = 0; i < WT_PAGE_LOCKS; ++i)
		WT_RET(
		    __wt_spin_init(session, &conn->page_lock[i], "btree page"));

	/* Setup the spin locks for the LSM manager queues. */
	WT_RET(__wt_spin_init(session,
	    &conn->lsm_manager.app_lock, "LSM application queue lock"));
	WT_RET(__wt_spin_init(session,
	    &conn->lsm_manager.manager_lock, "LSM manager queue lock"));
	WT_RET(__wt_spin_init(
	    session, &conn->lsm_manager.switch_lock, "LSM switch queue lock"));
	WT_RET(__wt_cond_alloc(
	    session, "LSM worker cond", false, &conn->lsm_manager.work_cond));

	/*
	 * Generation numbers.
	 *
	 * Start split generations at one.  Threads publish this generation
	 * number before examining tree structures, and zero when they leave.
	 * We need to distinguish between threads that are in a tree before the
	 * first split has happened, and threads that are not in a tree.
	 */
	conn->split_gen = 1;

	/*
	 * Block manager.
	 * XXX
	 * If there's ever a second block manager, we'll want to make this
	 * more opaque, but for now this is simpler.
	 */
	WT_RET(__wt_spin_init(session, &conn->block_lock, "block manager"));
	for (i = 0; i < WT_HASH_ARRAY_SIZE; i++)
		TAILQ_INIT(&conn->blockhash[i]);/* Block handle hash lists */
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
	u_int i;

	/* Check there's something to destroy. */
	if (conn == NULL)
		return (0);

	session = conn->default_session;

	/* Remove from the list of connections. */
	__wt_spin_lock(session, &__wt_process.spinlock);
	TAILQ_REMOVE(&__wt_process.connqh, conn, q);
	__wt_spin_unlock(session, &__wt_process.spinlock);

	/* Configuration */
	__wt_conn_config_discard(session);		/* configuration */

	__wt_conn_foc_discard(session);			/* free-on-close */

	__wt_spin_destroy(session, &conn->api_lock);
	__wt_spin_destroy(session, &conn->block_lock);
	__wt_spin_destroy(session, &conn->checkpoint_lock);
	__wt_spin_destroy(session, &conn->dhandle_lock);
	__wt_spin_destroy(session, &conn->encryptor_lock);
	__wt_spin_destroy(session, &conn->fh_lock);
	WT_TRET(__wt_rwlock_destroy(session, &conn->hot_backup_lock));
	__wt_spin_destroy(session, &conn->las_lock);
	__wt_spin_destroy(session, &conn->metadata_lock);
	__wt_spin_destroy(session, &conn->reconfig_lock);
	__wt_spin_destroy(session, &conn->schema_lock);
	__wt_spin_destroy(session, &conn->table_lock);
	__wt_spin_destroy(session, &conn->turtle_lock);
	for (i = 0; i < WT_PAGE_LOCKS; ++i)
		__wt_spin_destroy(session, &conn->page_lock[i]);
	__wt_free(session, conn->page_lock);

	/* Destroy the file-system configuration. */
	if (conn->file_system != NULL && conn->file_system->terminate != NULL)
		WT_TRET(conn->file_system->terminate(
		    conn->file_system, (WT_SESSION *)session));

	/* Free allocated memory. */
	__wt_free(session, conn->cfg);
	__wt_free(session, conn->home);
	__wt_free(session, conn->error_prefix);
	__wt_free(session, conn->sessions);

	__wt_free(NULL, conn);
	return (ret);
}
