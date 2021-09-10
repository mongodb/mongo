/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __ckpt_server_start(WT_CONNECTION_IMPL *);

/*
 * __ckpt_server_config --
 *     Parse and setup the checkpoint server options.
 */
static int
__ckpt_server_config(WT_SESSION_IMPL *session, const char **cfg, bool *startp)
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;

    *startp = false;

    conn = S2C(session);

    WT_RET(__wt_config_gets(session, cfg, "checkpoint.wait", &cval));
    conn->ckpt_usecs = (uint64_t)cval.val * WT_MILLION;

    WT_RET(__wt_config_gets(session, cfg, "checkpoint.log_size", &cval));
    conn->ckpt_logsize = (wt_off_t)cval.val;

    /*
     * The checkpoint configuration requires a wait time and/or a log size, if neither is set, we're
     * not running at all. Checkpoints based on log size also require logging be enabled.
     */
    if (conn->ckpt_usecs != 0 ||
      (conn->ckpt_logsize != 0 && FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED))) {
        /*
         * If checkpointing based on log data, use a minimum of the log file size. The logging
         * subsystem has already been initialized.
         */
        if (conn->ckpt_logsize != 0 && FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED))
            conn->ckpt_logsize = WT_MAX(conn->ckpt_logsize, conn->log_file_max);
        /* Checkpoints are incompatible with in-memory configuration */
        WT_RET(__wt_config_gets(session, cfg, "in_memory", &cval));
        if (cval.val != 0)
            WT_RET_MSG(session, EINVAL,
              "checkpoint configuration incompatible with in-memory configuration");

        __wt_log_written_reset(session);

        *startp = true;
    }

    return (0);
}

/*
 * __ckpt_server_run_chk --
 *     Check to decide if the checkpoint server should continue running.
 */
static bool
__ckpt_server_run_chk(WT_SESSION_IMPL *session)
{
    return (FLD_ISSET(S2C(session)->server_flags, WT_CONN_SERVER_CHECKPOINT));
}

/*
 * __ckpt_server --
 *     The checkpoint server thread.
 */
static WT_THREAD_RET
__ckpt_server(void *arg)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_SESSION *wt_session;
    WT_SESSION_IMPL *session;
    uint64_t checkpoint_gen;

    session = arg;
    conn = S2C(session);
    wt_session = (WT_SESSION *)session;

    for (;;) {
        /*
         * Wait... NOTE: If the user only configured logsize, then usecs will be 0 and this wait
         * won't return until signalled.
         */
        __wt_cond_wait(session, conn->ckpt_cond, conn->ckpt_usecs, __ckpt_server_run_chk);

        /* Check if we're quitting or being reconfigured. */
        if (!__ckpt_server_run_chk(session))
            break;

        checkpoint_gen = __wt_gen(session, WT_GEN_CHECKPOINT);
        WT_ERR(wt_session->checkpoint(wt_session, NULL));

        /*
         * Reset the log file size counters if the checkpoint wasn't skipped.
         */
        if (checkpoint_gen != __wt_gen(session, WT_GEN_CHECKPOINT) && conn->ckpt_logsize) {
            __wt_log_written_reset(session);
            conn->ckpt_signalled = false;

            /*
             * In case we crossed the log limit during the checkpoint and the condition variable was
             * already signalled, do a tiny wait to clear it so we don't do another checkpoint
             * immediately.
             */
            __wt_cond_wait(session, conn->ckpt_cond, 1, NULL);
        }
    }

    if (0) {
err:
        WT_IGNORE_RET(__wt_panic(session, ret, "checkpoint server error"));
    }
    return (WT_THREAD_RET_VALUE);
}

/*
 * __ckpt_server_start --
 *     Start the checkpoint server thread.
 */
static int
__ckpt_server_start(WT_CONNECTION_IMPL *conn)
{
    WT_SESSION_IMPL *session;
    uint32_t session_flags;

    /* Nothing to do if the server is already running. */
    if (conn->ckpt_session != NULL)
        return (0);

    FLD_SET(conn->server_flags, WT_CONN_SERVER_CHECKPOINT);

    /*
     * The checkpoint server gets its own session.
     *
     * Checkpoint does enough I/O it may be called upon to perform slow operations for the block
     * manager.
     */
    session_flags = WT_SESSION_CAN_WAIT;
    WT_RET(__wt_open_internal_session(
      conn, "checkpoint-server", true, session_flags, 0, &conn->ckpt_session));
    session = conn->ckpt_session;

    WT_RET(__wt_cond_alloc(session, "checkpoint server", &conn->ckpt_cond));

    /*
     * Start the thread.
     */
    WT_RET(__wt_thread_create(session, &conn->ckpt_tid, __ckpt_server, session));
    conn->ckpt_tid_set = true;

    return (0);
}

/*
 * __wt_checkpoint_server_create --
 *     Configure and start the checkpoint server.
 */
int
__wt_checkpoint_server_create(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CONNECTION_IMPL *conn;
    bool start;

    conn = S2C(session);
    start = false;

    /*
     * Stop any server that is already running. This means that each time reconfigure is called
     * we'll bounce the server even if there are no configuration changes. This makes our life
     * easier as the underlying configuration routine doesn't have to worry about freeing objects in
     * the connection structure (it's guaranteed to always start with a blank slate), and we don't
     * have to worry about races where a running server is reading configuration information that
     * we're updating, and it's not expected that reconfiguration will happen a lot.
     */
    if (conn->ckpt_session != NULL)
        WT_RET(__wt_checkpoint_server_destroy(session));

    WT_RET(__ckpt_server_config(session, cfg, &start));
    if (start)
        WT_RET(__ckpt_server_start(conn));

    return (0);
}

/*
 * __wt_checkpoint_server_destroy --
 *     Destroy the checkpoint server thread.
 */
int
__wt_checkpoint_server_destroy(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    conn = S2C(session);

    FLD_CLR(conn->server_flags, WT_CONN_SERVER_CHECKPOINT);
    if (conn->ckpt_tid_set) {
        __wt_cond_signal(session, conn->ckpt_cond);
        WT_TRET(__wt_thread_join(session, &conn->ckpt_tid));
        conn->ckpt_tid_set = false;
    }
    __wt_cond_destroy(session, &conn->ckpt_cond);

    /* Close the server thread's session. */
    if (conn->ckpt_session != NULL)
        WT_TRET(__wt_session_close_internal(conn->ckpt_session));

    /*
     * Ensure checkpoint settings are cleared - so that reconfigure doesn't get confused.
     */
    conn->ckpt_session = NULL;
    conn->ckpt_tid_set = false;
    conn->ckpt_cond = NULL;
    conn->ckpt_usecs = 0;

    return (ret);
}

/*
 * __wt_checkpoint_signal --
 *     Signal the checkpoint thread if sufficient log has been written.
 */
void
__wt_checkpoint_signal(WT_SESSION_IMPL *session, wt_off_t logsize)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);
    WT_ASSERT(session, WT_CKPT_LOGSIZE(conn));
    if (logsize >= conn->ckpt_logsize && !conn->ckpt_signalled) {
        __wt_cond_signal(session, conn->ckpt_cond);
        conn->ckpt_signalled = true;
    }
}

/*
 * __wt_checkpoint_reserved_session_init --
 *     Initialize checkpoint reserved session.
 */
int
__wt_checkpoint_reserved_session_init(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    WT_ASSERT(session, conn->ckpt_reserved_session == NULL);

    return (__wt_open_internal_session(
      conn, "ckpt-reserved", false, WT_SESSION_NO_RECONCILE, 0, &conn->ckpt_reserved_session));
}

/*
 * __wt_checkpoint_reserved_session_destroy --
 *     Release resources allocated for checkpoint reserved session.
 */
int
__wt_checkpoint_reserved_session_destroy(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    conn = S2C(session);

    if (conn->ckpt_reserved_session != NULL) {
        WT_TRET(__wt_session_close_internal(conn->ckpt_reserved_session));
        conn->ckpt_reserved_session = NULL;
    }

    return (ret);
}
