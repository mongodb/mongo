/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wti_connection_init --
 *     Structure initialization for a just-created WT_CONNECTION_IMPL handle.
 */
int
__wti_connection_init(WT_CONNECTION_IMPL *conn)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = conn->default_session;

    TAILQ_INIT(&conn->dhqh);                                       /* Data handle list */
    TAILQ_INIT(&conn->dlhqh);                                      /* Library list */
    TAILQ_INIT(&conn->dsrcqh);                                     /* Data source list */
    TAILQ_INIT(&conn->fhqh);                                       /* File list */
    TAILQ_INIT(&conn->disaggregated_storage.shared_metadata_qh);   /* Shared metadata list */
    TAILQ_INIT(&conn->disaggregated_storage.pending_crypt_key_qh); /* Pending pushed crypt keys */

    /* Prefetch. */
    WT_RET(__wti_conn_prefetch_init(session));

    /* Tiered storage. */
    WT_RET(__wti_conn_tiered_init(session));

    /* I/O capacity subsystem. */
    __wti_conn_capacity_init(session);

    /* Extension interface lists. */
    WT_RET(__wti_conn_ext_init(session));

    /* Random numbers. */
    __wt_session_rng_init_once(session);

    /* Configuration. */
    WT_RET(__wt_conn_config_init(session));

    /* Statistics. */
    WT_RET(__wt_stat_connection_init(session, conn));

    /* Spinlocks. */
    WT_RET(__wt_spin_init(session, &conn->api_lock, "api"));
    WT_SPIN_INIT_TRACKED(session, &conn->checkpoint_lock, checkpoint);
    WT_RET(__wt_spin_init(session, &conn->background_compact.lock, "background compact"));
    WT_RET(__wt_spin_init(
      session, &conn->disaggregated_storage.shared_metadata_queue_lock, "update shared metadata"));
    WT_RET(__wt_spin_init(
      session, &conn->disaggregated_storage.pending_crypt_key_lock, "pending crypt key"));
    WT_RET(__wt_spin_init(session, &conn->fh_lock, "file list"));
    WT_SPIN_INIT_TRACKED(session, &conn->metadata_lock, metadata);
    WT_RET(__wt_spin_init(session, &conn->reconfig_lock, "reconfigure"));
    WT_SPIN_INIT_SESSION_TRACKED(session, &conn->schema_lock, schema);
    WT_RET(__wt_spin_init(session, &conn->turtle_lock, "turtle file"));

    /* Read-write locks */
    WT_RET(__wt_rwlock_init(session, &conn->log_mgr.debug_log_retention_lock));
    WT_RWLOCK_INIT_SESSION_TRACKED(session, &conn->dhandle_lock, dhandle);
    WT_RWLOCK_INIT_TRACKED(session, &conn->table_lock, table);

    /* Hot backup subsystem. */
    WT_RET(__wti_conn_backup_init(session));

    /* Initialize the generation manager. */
    __wt_gen_init(session);

    /*
     * Block manager. XXX If there's ever a second block manager, we'll want to make this more
     * opaque, but for now this is simpler.
     */
    WT_RET(__wt_spin_init(session, &conn->block_lock, "block manager"));
    TAILQ_INIT(&conn->blockqh); /* Block manager list */

    __wt_checkpoint_timer_stats_clear(session);

err:
    return (ret);
}

/*
 * __wti_connection_destroy --
 *     Destroy the connection's underlying WT_CONNECTION_IMPL structure.
 */
void
__wti_connection_destroy(WT_CONNECTION_IMPL *conn)
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
    __wt_conn_config_discard(session); /* configuration */

    __wt_conn_foc_discard(session); /* free-on-close */

    __wt_spin_destroy(session, &conn->api_lock);
    __wt_spin_destroy(session, &conn->background_compact.lock);
    __wt_spin_destroy(session, &conn->block_lock);
    __wt_spin_destroy(session, &conn->checkpoint_lock);
    __wt_spin_destroy(session, &conn->disaggregated_storage.shared_metadata_queue_lock);
    __wti_disagg_pending_crypt_key_clear(session);
    __wt_spin_destroy(session, &conn->disaggregated_storage.pending_crypt_key_lock);
    __wt_rwlock_destroy(session, &conn->log_mgr.debug_log_retention_lock);
    __wt_rwlock_destroy(session, &conn->dhandle_lock);
    __wt_spin_destroy(session, &conn->fh_lock);
    __wti_conn_backup_destroy(session);
    __wt_spin_destroy(session, &conn->metadata_lock);
    __wt_spin_destroy(session, &conn->reconfig_lock);
    __wt_spin_destroy(session, &conn->schema_lock);
    __wt_rwlock_destroy(session, &conn->table_lock);
    __wt_spin_destroy(session, &conn->turtle_lock);
    __wti_conn_prefetch_destroy(session);

    /* Tiered storage. */
    __wti_conn_tiered_destroy(session);

    /* I/O capacity subsystem. */
    __wti_conn_capacity_destroy(session);

    /* Extension interface lists. */
    __wti_conn_ext_destroy(session);

    /* Free allocated hash buckets. */
    __wt_free(session, conn->blockhash);
    __wt_free(session, conn->dh_bucket_count);
    __wt_free(session, conn->dhhash);
    __wt_free(session, conn->fhhash);

    /* Free allocated recovered checkpoint snapshot memory */
    __wt_free(session, conn->recovery_ckpt_snapshot);

    /* Free checkpoint eviction snapshot buffer backing arrays. */
    __wt_free(session, conn->ckpt_eviction_snap_array[0]);
    __wt_free(session, conn->ckpt_eviction_snap_array[1]);

    /* Free allocated memory. */
    __wt_free(session, conn->cfg);
    __wt_free(session, conn->debug.ckpt);
    __wt_free(session, conn->error_prefix);
    __wt_free(session, conn->home);
    __wt_buf_free(session, &conn->repair.last_report);
    __wt_free(session, WT_CONN_SESSIONS_GET(conn));
    __wt_stat_connection_discard(session, conn);

    __wt_free(NULL, conn);
}
