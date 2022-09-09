/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_connection_open --
 *     Open a connection.
 */
int
__wt_connection_open(WT_CONNECTION_IMPL *conn, const char *cfg[])
{
    WT_SESSION_IMPL *session;

    /* Default session. */
    session = conn->default_session;
    WT_ASSERT(session, session->iface.connection == &conn->iface);

    /* WT_SESSION_IMPL array. */
    WT_RET(__wt_calloc(session, conn->session_size, sizeof(WT_SESSION_IMPL), &conn->sessions));

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
     * Publish: there must be a barrier to ensure the connection structure fields are set before
     * other threads read from the pointer.
     */
    WT_WRITE_BARRIER();

    /* Create the cache. */
    WT_RET(__wt_cache_create(session, cfg));

    /* Initialize transaction support. */
    WT_RET(__wt_txn_global_init(session, cfg));

    WT_STAT_CONN_SET(session, dh_conn_handle_size, sizeof(WT_DATA_HANDLE));
    return (0);
}

/*
 * __wt_connection_close --
 *     Close a connection handle.
 */
int
__wt_connection_close(WT_CONNECTION_IMPL *conn)
{
    WT_CONNECTION *wt_conn;
    WT_DECL_RET;
    WT_DLH *dlh;
    WT_SESSION_IMPL *s, *session;
    u_int i;

    wt_conn = &conn->iface;
    session = conn->default_session;

    /*
     * The LSM services are not shut down in this path (which is called when wiredtiger_open hits an
     * error (as well as during normal shutdown). Assert they're not running.
     */
    WT_ASSERT(session, !FLD_ISSET(conn->server_flags, WT_CONN_SERVER_LSM));

    /* Shut down the subsystems, ensuring workers see the state change. */
    F_SET(conn, WT_CONN_CLOSING);
    WT_FULL_BARRIER();

    /* The default session is used to access data handles during close. */
    F_CLR(session, WT_SESSION_NO_DATA_HANDLES);

    /*
     * Shut down server threads. Some of these threads access btree handles and eviction, shut them
     * down before the eviction server, and shut all servers down before closing open data handles.
     */
    WT_TRET(__wt_capacity_server_destroy(session));
    WT_TRET(__wt_checkpoint_server_destroy(session));
    WT_TRET(__wt_statlog_destroy(session, true));
    WT_TRET(__wt_tiered_storage_destroy(session, false));
    WT_TRET(__wt_sweep_destroy(session));

    /* The eviction server is shut down last. */
    WT_TRET(__wt_evict_destroy(session));

    /* There should be no more file opens after this point. */
    F_SET(conn, WT_CONN_CLOSING_NO_MORE_OPENS);
    WT_FULL_BARRIER();

    /* Close open data handles. */
    WT_TRET(__wt_conn_dhandle_discard(session));

    /* Close the checkpoint reserved session. */
    WT_TRET(__wt_checkpoint_reserved_session_destroy(session));

    /* Shut down metadata tracking. */
    WT_TRET(__wt_meta_track_destroy(session));

    /* Shut down the block cache */
    __wt_blkcache_destroy(session);

    /*
     * Now that all data handles are closed, tell logging that a checkpoint has completed then shut
     * down the log manager (only after closing data handles). The call to destroy the log manager
     * is outside the conditional because we allocate the log path so that printlog can run without
     * running logging or recovery.
     */
    if (ret == 0 && FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED) &&
      FLD_ISSET(conn->log_flags, WT_CONN_LOG_RECOVER_DONE))
        WT_TRET(__wt_txn_checkpoint_log(session, true, WT_TXN_LOG_CKPT_STOP, NULL));
    WT_TRET(__wt_logmgr_destroy(session));

    /* Free memory for collators, compressors, data sources. */
    WT_TRET(__wt_conn_remove_collator(session));
    WT_TRET(__wt_conn_remove_compressor(session));
    WT_TRET(__wt_conn_remove_data_source(session));
    WT_TRET(__wt_conn_remove_encryptor(session));
    WT_TRET(__wt_conn_remove_extractor(session));
    WT_TRET(__wt_conn_remove_storage_source(session));

    /* Disconnect from shared cache - must be before cache destroy. */
    WT_TRET(__wt_conn_cache_pool_destroy(session));

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
    WT_TRET(__wt_conn_optrack_teardown(session, false));

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
    if (!F_ISSET(conn, WT_CONN_LEAK_MEMORY))
        if ((s = conn->sessions) != NULL)
            for (i = 0; i < conn->session_size; ++s, ++i) {
                __wt_free(session, s->cursor_cache);
                __wt_free(session, s->dhhash);
                __wt_stash_discard_all(session, s);
                __wt_free(session, s->hazard);
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

    /* Destroy the handle. */
    __wt_connection_destroy(conn);

    return (ret);
}

/*
 * __wt_connection_workers --
 *     Start the worker threads.
 */
int
__wt_connection_workers(WT_SESSION_IMPL *session, const char *cfg[])
{
    /*
     * Start the optional statistics thread. Start statistics first so that other optional threads
     * can know if statistics are enabled or not.
     */
    WT_RET(__wt_statlog_create(session, cfg));
    WT_RET(__wt_tiered_storage_create(session));
    WT_RET(__wt_logmgr_create(session));

    /*
     * Run recovery. NOTE: This call will start (and stop) eviction if recovery is required.
     * Recovery must run before the history store table is created (because recovery will update the
     * metadata, and set the maximum file id seen), and before eviction is started for real.
     */
    WT_RET(__wt_txn_recover(session, cfg));

    /* Initialize metadata tracking, required before creating tables. */
    WT_RET(__wt_meta_track_init(session));

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
    WT_RET(__wt_evict_create(session));

    /* Start the handle sweep thread. */
    WT_RET(__wt_sweep_create(session));

    /* Start the optional capacity thread. */
    WT_RET(__wt_capacity_server_create(session, cfg));

    /* Initialize checkpoint reserved session, required for the checkpoint operation. */
    WT_RET(__wt_checkpoint_reserved_session_init(session));

    /* Start the optional checkpoint thread. */
    WT_RET(__wt_checkpoint_server_create(session, cfg));

    return (0);
}
