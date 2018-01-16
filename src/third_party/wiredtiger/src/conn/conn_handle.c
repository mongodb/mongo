/*-
 * Copyright (c) 2014-2018 MongoDB, Inc.
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
	WT_RET(__wt_stat_connection_init(session, conn));

	/* Spinlocks. */
	WT_RET(__wt_spin_init(session, &conn->api_lock, "api"));
	WT_SPIN_INIT_TRACKED(session, &conn->checkpoint_lock, checkpoint);
	WT_RET(__wt_spin_init(session, &conn->encryptor_lock, "encryptor"));
	WT_RET(__wt_spin_init(session, &conn->fh_lock, "file list"));
	WT_SPIN_INIT_TRACKED(session, &conn->metadata_lock, metadata);
	WT_RET(__wt_spin_init(session, &conn->reconfig_lock, "reconfigure"));
	WT_SPIN_INIT_TRACKED(session, &conn->schema_lock, schema);
	WT_RET(__wt_spin_init(session, &conn->turtle_lock, "turtle file"));

	/* Read-write locks */
	WT_RWLOCK_INIT_TRACKED(session, &conn->dhandle_lock, dhandle);
	WT_RET(__wt_rwlock_init(session, &conn->hot_backup_lock));
	WT_RWLOCK_INIT_TRACKED(session, &conn->table_lock, table);

	/* Setup the spin locks for the LSM manager queues. */
	WT_RET(__wt_spin_init(session,
	    &conn->lsm_manager.app_lock, "LSM application queue lock"));
	WT_RET(__wt_spin_init(session,
	    &conn->lsm_manager.manager_lock, "LSM manager queue lock"));
	WT_RET(__wt_spin_init(
	    session, &conn->lsm_manager.switch_lock, "LSM switch queue lock"));
	WT_RET(__wt_cond_alloc(
	    session, "LSM worker cond", &conn->lsm_manager.work_cond));

	/* Initialize the generation manager. */
	__wt_gen_init(session);

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
void
__wt_connection_destroy(WT_CONNECTION_IMPL *conn)
{
	WT_SESSION_IMPL *session;

	/* Check there's something to destroy. */
	if (conn == NULL)
		return;

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
	__wt_rwlock_destroy(session, &conn->dhandle_lock);
	__wt_spin_destroy(session, &conn->encryptor_lock);
	__wt_spin_destroy(session, &conn->fh_lock);
	__wt_rwlock_destroy(session, &conn->hot_backup_lock);
	__wt_spin_destroy(session, &conn->metadata_lock);
	__wt_spin_destroy(session, &conn->reconfig_lock);
	__wt_spin_destroy(session, &conn->schema_lock);
	__wt_rwlock_destroy(session, &conn->table_lock);
	__wt_spin_destroy(session, &conn->turtle_lock);

	/* Free allocated memory. */
	__wt_free(session, conn->cfg);
	__wt_free(session, conn->home);
	__wt_free(session, conn->error_prefix);
	__wt_free(session, conn->sessions);
	__wt_stat_connection_discard(session, conn);

	__wt_free(NULL, conn);
}
