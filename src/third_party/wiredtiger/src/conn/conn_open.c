/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wti_connection_open --
 *     Open a connection.
 */
int
__wti_connection_open(WT_CONNECTION_IMPL *conn, const char *cfg[])
{
    WT_SESSION_IMPL *session;

    /* Default session. */
    session = conn->default_session;
    WT_ASSERT(session, session->iface.connection == &conn->iface);

    /* WT_SESSION_IMPL array. */
    WT_RET(__wt_calloc(
      session, conn->session_array.size, sizeof(WT_SESSION_IMPL), &WT_CONN_SESSIONS_GET(conn)));

    /*
     * Open the default session. We open this before starting service threads because those may
     * allocate and use session resources that need to get cleaned up on close.
     */
    WT_RET(__wt_open_internal_session(conn, "connection", false, 0, 0, &session));

    /*
     * The connection's default session is originally a static structure, swap that out for a more
     * fully-functional session. It's necessary to have this step: the session allocation code uses
     * the connection's session, and if we pass a reference to the default session as the place to
     * store the allocated session, things get confused and error handling can be corrupted. So, we
     * allocate into a stack variable and then assign it on success.
     */
    conn->default_session = session;

    /*
     * Release write: there must be a barrier to ensure the connection structure fields are set
     * before other threads read from the pointer.
     */
    WT_RELEASE_BARRIER();

    /* Create the cache. */
    WT_RET(__wt_cache_create(session, cfg));

    /* Initialize eviction. */
    WT_RET(__wt_evict_create(session, cfg));

    /* Create shared cache.*/
    WT_RET(__wt_cache_pool_create(session, cfg));

    /* Initialize transaction support. */
    WT_RET(__wt_txn_global_init(session, cfg));

    WT_RET(__wt_rollback_to_stable_init(session, cfg));
    WT_STAT_CONN_SET(session, dh_conn_handle_size, sizeof(WT_DATA_HANDLE));
    return (0);
}

/*
 * __wti_connection_close --
 *     Close a connection handle.
 */
int
__wti_connection_close(WT_CONNECTION_IMPL *conn)
{
    WT_CONNECTION *wt_conn;
    WT_DECL_RET;
    WT_DLH *dlh;
    WT_SESSION_IMPL *s, *session;
    u_int i;

    wt_conn = &conn->iface;
    session = conn->default_session;

    /* Shut down the subsystems, ensuring workers see the state change. */
    F_SET_ATOMIC_32(conn, WT_CONN_CLOSING);
    WT_FULL_BARRIER();

    /* The default session is used to access data handles during close. */
    F_CLR(session, WT_SESSION_NO_DATA_HANDLES);

    /* Shut down the page history tracker. */
    WT_TRET(__wti_conn_page_history_destroy(session));

    /*
     * Shut down server threads. Some of these threads access btree handles and eviction, shut them
     * down before the eviction server, and shut all servers down before closing open data handles.
     */
    WT_TRET(__wt_live_restore_server_destroy(session));
    WT_TRET(__wti_background_compact_server_destroy(session));
    WT_TRET(__wt_checkpoint_server_destroy(session));
    WT_TRET(__wti_statlog_destroy(session, true));
    WT_TRET(__wti_tiered_storage_destroy(session, false));
    WT_TRET(__wti_sweep_destroy(session));
    WT_TRET(__wt_chunkcache_teardown(session));
    WT_TRET(__wti_chunkcache_metadata_destroy(session));
    WT_TRET(__wti_prefetch_destroy(session));

    /* The eviction server is shut down last. */
    WT_TRET(__wt_evict_threads_destroy(session));
    /* The capacity server can only be shut down after all I/O is complete. */
    WT_TRET(__wti_capacity_server_destroy(session));

    /* There should be no more file opens after this point. */
    F_SET_ATOMIC_32(conn, WT_CONN_CLOSING_NO_MORE_OPENS);
    WT_FULL_BARRIER();

    /* Close open data handles. */
    WT_TRET(__wti_conn_dhandle_discard(session));

    /* Shut down metadata tracking. */
    WT_TRET(__wt_meta_track_destroy(session));

    /* Shut down the block cache */
    __wt_blkcache_destroy(session);

    /* Shut down layered table manager - this should be done after closing out data handles. */
    WT_TRET(__wti_layered_table_manager_destroy(session));

    /*
     * Now that all data handles are closed, tell logging that a checkpoint has completed then shut
     * down the log manager (only after closing data handles). The call to destroy the log manager
     * is outside the conditional because we allocate the log path so that printlog can run without
     * running logging or recovery.
     */
    if (ret == 0 && F_ISSET(&conn->log_mgr, WT_LOG_ENABLED) &&
      F_ISSET(&conn->log_mgr, WT_LOG_RECOVER_DONE))
        WT_TRET(__wt_checkpoint_log(session, true, WT_TXN_LOG_CKPT_STOP, NULL));
    WT_TRET(__wt_logmgr_destroy(session));

    /* Shut down disaggregated storage. */
    WT_TRET(__wti_disagg_destroy(session));

    /* Free memory for collators, compressors, data sources. */
    WT_TRET(__wti_conn_remove_collator(session));
    WT_TRET(__wti_conn_remove_compressor(session));
    WT_TRET(__wti_conn_remove_data_source(session));
    WT_TRET(__wti_conn_remove_encryptor(session));
    WT_TRET(__wti_conn_remove_page_log(session));
    WT_TRET(__wti_conn_remove_storage_source(session));

    /* Disconnect from shared cache - must be before cache destroy. */
    WT_TRET(__wt_cache_pool_destroy(session));

    /* Destroy Eviction. */
    WT_TRET(__wt_evict_destroy(session));

    /* Discard the cache. */
    WT_TRET(__wt_cache_destroy(session));

    /* Discard transaction state. */
    __wt_txn_global_destroy(session);

    /* Close the lock file, opening up the database to other connections. */
    if (conn->lock_fh != NULL)
        WT_TRET(__wt_close(session, &conn->lock_fh));

    /* Close any optrack files */
    if (session->optrack_fh != NULL)
        WT_TRET(__wt_close(session, &session->optrack_fh));

    /* Close operation tracking */
    WT_TRET(__wti_conn_optrack_teardown(session, false));

#ifdef HAVE_CALL_LOG
    WT_TRET(__wt_conn_call_log_teardown(session));
#endif

    __wt_backup_destroy(session);

    /* Close any file handles left open. */
    WT_TRET(__wt_close_connection_close(session));

    /*
     * Close the internal (default) session, and switch back to the dummy session in case of any
     * error messages from the remaining operations while destroying the connection handle.
     */
    if (session != &conn->dummy_session) {
        WT_TRET(__wt_session_close_internal(session));
        session = conn->default_session = &conn->dummy_session;
    }

    /*
     * The session split stash, hazard information and handle arrays aren't discarded during normal
     * session close, they persist past the life of the session. Discard them now.
     */
    if (!F_ISSET_ATOMIC_32(conn, WT_CONN_LEAK_MEMORY))
        if ((s = WT_CONN_SESSIONS_GET(conn)) != NULL)
            for (i = 0; i < conn->session_array.size; ++s, ++i) {
                __wt_free(session, s->cursor_cache);
                __wt_free(session, s->dhhash);
                __wt_stash_discard_all(session, s);
                __wt_free(session, s->hazards.arr);
            }

    /* Destroy the file-system configuration. */
    if (conn->file_system != NULL && conn->file_system->terminate != NULL)
        WT_TRET(conn->file_system->terminate(conn->file_system, (WT_SESSION *)session));

    /* Close extensions, first calling any unload entry point. */
    while ((dlh = TAILQ_FIRST(&conn->dlhqh)) != NULL) {
        TAILQ_REMOVE(&conn->dlhqh, dlh, q);

        if (dlh->terminate != NULL)
            WT_TRET(dlh->terminate(wt_conn));
        WT_TRET(__wt_dlclose(session, dlh));
    }

    /* Destroy any precompiled configuration. */
    __wt_conf_compile_discard(session);

    /* Destroy the handle. */
    __wti_connection_destroy(conn);

    return (ret);
}

/*
 * __wti_connection_workers --
 *     Start the worker threads.
 */
int
__wti_connection_workers(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CONFIG_ITEM cval;

    __wt_verbose_info(session, WT_VERB_RECOVERY, "%s", "starting WiredTiger utility threads");

    /*
     * Start the optional statistics thread. Start statistics first so that other optional threads
     * can know if statistics are enabled or not.
     */
    WT_RET(__wti_statlog_create(session, cfg));
    WT_RET(__wti_tiered_storage_create(session));
    WT_RET(__wt_logmgr_create(session));

    /* Initialize the page history tracker. */
    WT_RET(__wti_conn_page_history_config(session, cfg, false));

    /*
     * Run recovery. NOTE: This call will start (and stop) eviction if recovery is required.
     * Recovery must run before the history store table is created (because recovery will update the
     * metadata, and set the maximum file id seen), and before eviction is started for real.
     *
     * FIXME-WT-14721: the disagg config check is a giant hack. Ideally, we'd have a single
     * top-level disagg config item that can be checked, and set a variable elsewhere so we could
     * gate this on a call like __wt_conn_is_disagg.
     *
     * As it stands, __wt_conn_is_disagg only works after we have metadata access, which depends on
     * having run recovery, so the config hack is the simplest way to break that dependency.
     */
    WT_RET(__wt_config_gets(session, cfg, "disaggregated.page_log", &cval));
    WT_RET(__wt_txn_recover(session, cfg, cval.len != 0));

    /*
     * If we're performing a live restore start the server. This is intentionally placed after
     * recovery finishes as we depend on the metadata file containing the list of objects that need
     * live restoration.
     */
    WT_RET(__wt_live_restore_server_create(session, cfg));

    /* Initialize metadata tracking, required before creating tables. */
    WT_RET(__wt_meta_track_init(session)); /* XXXXXX */

    /*
     * Initialize disaggregated storage. It technically doesn't belong here, but it must be
     * initialized after metadata tracking and before the history store.
     */
    WT_RET(__wti_disagg_conn_config(session, cfg, false));

    /* Can create a table, so must be done after metadata tracking. */
    WT_RET(__wt_chunkcache_setup(session, cfg));
    WT_RET(__wti_chunkcache_metadata_create(session));

    /*
     * Create the history store file. This will only actually create it on a clean upgrade or when
     * creating a new database.
     */
    WT_RET(__wt_hs_open(session, cfg));

    /*
     * Start the optional logging/removal threads. NOTE: The log manager must be started before
     * checkpoints so that the checkpoint server knows if logging is enabled. It must also be
     * started before any operation that can commit, or the commit can block.
     */
    WT_RET(__wt_logmgr_open(session));

    /*
     * Start eviction threads. NOTE: Eviction must be started after the history store table is
     * created.
     */
    WT_RET(__wt_evict_threads_create(session));

    /* Start the handle sweep thread. */
    WT_RET(__wti_sweep_create(session));

    /* Start the compact thread. */
    WT_RET(__wti_background_compact_server_create(session));

    /* Start the optional capacity thread. */
    WT_RET(__wti_capacity_server_create(session, cfg));

    /* Start the optional checkpoint thread. */
    WT_RET(__wt_checkpoint_server_create(session, cfg));

    /* Start pre-fetch utilities. */
    WT_RET(__wti_prefetch_create(session, cfg));

    /* Start the checkpoint cleanup thread. */
    WT_RET(__wt_checkpoint_cleanup_create(session, cfg));

    __wt_verbose_info(
      session, WT_VERB_RECOVERY, "%s", "WiredTiger utility threads started successfully");

    return (0);
}
