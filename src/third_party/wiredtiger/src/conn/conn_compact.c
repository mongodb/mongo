/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __compact_server_run_chk --
 *     Check to decide if the compact server should continue running.
 */
static bool
__compact_server_run_chk(WT_SESSION_IMPL *session)
{
    return (FLD_ISSET(S2C(session)->server_flags, WT_CONN_SERVER_COMPACT));
}

/*
 * __compact_server --
 *     The compact server thread.
 */
static WT_THREAD_RET
__compact_server(void *arg)
{
    WT_CONNECTION_IMPL *conn;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_SESSION *wt_session;
    WT_SESSION_IMPL *session;
    int exact;
    const char *config, *key, *prefix, *uri;
    bool full_iteration, running, signalled;

    session = arg;
    conn = S2C(session);
    wt_session = (WT_SESSION *)session;
    cursor = NULL;
    config = NULL;
    key = NULL;
    uri = NULL;
    /* The compact operation is only applied on URIs with a specific prefix. */
    prefix = "file:";
    exact = 0;
    full_iteration = running = signalled = false;

    WT_STAT_CONN_SET(session, background_compact_running, 0);

    for (;;) {

        /* When the entire metadata file has been parsed, take a break or wait until signalled. */
        if (full_iteration || !running) {

            full_iteration = false;
            /*
             * FIXME-WT-11409: Depending on the previous state, we may not want to clear out the
             * last key used. This could be useful if the server was paused to be resumed later.
             */
            __wt_free(session, uri);
            WT_ERR(__wt_strndup(session, prefix, strlen(prefix), &uri));
            /* Check every 10 seconds in case the signal was missed. */
            __wt_cond_wait(
              session, conn->background_compact.cond, 10 * WT_MILLION, __compact_server_run_chk);
        }

        /* Check if we're quitting or being reconfigured. */
        if (!__compact_server_run_chk(session))
            break;

        __wt_spin_lock(session, &conn->background_compact.lock);
        running = conn->background_compact.running;
        if (conn->background_compact.signalled) {
            conn->background_compact.signalled = false;
            WT_STAT_CONN_SET(session, background_compact_running, running);
        }
        __wt_spin_unlock(session, &conn->background_compact.lock);

        /*
         * This check is necessary as we may have timed out while waiting on the mutex to be
         * signalled and compaction is not supposed to be executed.
         */
        if (!running)
            continue;

        /* Open a metadata cursor. */
        WT_ERR(__wt_metadata_cursor(session, &cursor));

        cursor->set_key(cursor, uri);
        WT_ERR(cursor->search_near(cursor, &exact));

        /* Make sure not to go backwards. */
        if (exact <= 0)
            WT_ERR_NOTFOUND_OK(cursor->next(cursor), true);

        /* Find a table to compact. */
        while (ret == 0) {
            WT_ERR(cursor->get_key(cursor, &key));
            /* Check we are still dealing with keys which have the right prefix. */
            if (WT_PREFIX_MATCH(key, prefix)) {
                /* There are files that should not be compacted. */
                if (!WT_STREQ(key, WT_HS_URI))
                    /* FIXME-WT-11343: check if the table is supposed to be compacted. */
                    break;
            } else {
                ret = WT_NOTFOUND;
                break;
            }
            WT_ERR_NOTFOUND_OK(cursor->next(cursor), true);
        }

        /* All the keys with the specified prefix have been parsed. */
        if (ret == WT_NOTFOUND) {
            WT_ERR(__wt_metadata_cursor_release(session, &cursor));
            full_iteration = true;
            continue;
        }

        /* Make a copy of the key as it can be freed once the cursor is released. */
        __wt_free(session, uri);
        WT_ERR(__wt_strndup(session, key, strlen(key), &uri));

        /* Always close the metadata cursor. */
        WT_ERR(__wt_metadata_cursor_release(session, &cursor));

        /* Compact the file with the latest configuration. */
        __wt_spin_lock(session, &conn->background_compact.lock);
        if (config == NULL || !WT_STREQ(config, conn->background_compact.config)) {
            __wt_free(session, config);
            ret = __wt_strndup(session, conn->background_compact.config,
              strlen(conn->background_compact.config), &config);
        }
        __wt_spin_unlock(session, &conn->background_compact.lock);

        WT_ERR(ret);

        ret = wt_session->compact(wt_session, uri, config);

        /* FIXME-WT-11343: compaction is done, update the data structure for this table. */
        /*
         * Compact may return:
         * - EBUSY or WT_ROLLBACK for various reasons.
         * - ETIMEDOUT if the configured timer has elapsed.
         * - ENOENT if the underlying file does not exist.
         */
        if (ret == EBUSY || ret == ENOENT || ret == ETIMEDOUT || ret == WT_ROLLBACK) {
            if (ret == EBUSY && __wt_cache_stuck(session))
                WT_STAT_CONN_INCR(session, background_compact_fail_cache_pressure);

            if (ret == ETIMEDOUT)
                WT_STAT_CONN_INCR(session, background_compact_timeout);

            WT_STAT_CONN_INCR(session, background_compact_fail);
            ret = 0;
        }

        WT_ERR(ret);
    }

    WT_STAT_CONN_SET(session, background_compact_running, 0);

    WT_ERR(__wt_metadata_cursor_close(session));
    __wt_free(session, config);
    __wt_free(session, conn->background_compact.config);
    __wt_free(session, uri);

    if (0) {
err:
        WT_IGNORE_RET(__wt_panic(session, ret, "compact server error"));
    }
    return (WT_THREAD_RET_VALUE);
}

/*
 * __wt_compact_server_create --
 *     Start the compact thread.
 */
int
__wt_compact_server_create(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    uint32_t session_flags;

    conn = S2C(session);

    /* Set first, the thread might run before we finish up. */
    FLD_SET(conn->server_flags, WT_CONN_SERVER_COMPACT);

    /*
     * Compaction does enough I/O it may be called upon to perform slow operations for the block
     * manager.
     */
    session_flags = WT_SESSION_CAN_WAIT;
    WT_RET(__wt_open_internal_session(
      conn, "compact-server", true, session_flags, 0, &conn->background_compact.session));
    session = conn->background_compact.session;

    WT_RET(__wt_cond_alloc(session, "compact server", &conn->background_compact.cond));

    WT_RET(__wt_thread_create(session, &conn->background_compact.tid, __compact_server, session));
    conn->background_compact.tid_set = true;

    return (0);
}

/*
 * __wt_compact_server_destroy --
 *     Destroy the background compaction server thread.
 */
int
__wt_compact_server_destroy(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    conn = S2C(session);

    FLD_CLR(conn->server_flags, WT_CONN_SERVER_COMPACT);
    if (conn->background_compact.tid_set) {
        conn->background_compact.running = false;
        __wt_cond_signal(session, conn->background_compact.cond);
        WT_TRET(__wt_thread_join(session, &conn->background_compact.tid));
        conn->background_compact.tid_set = false;
    }
    __wt_cond_destroy(session, &conn->background_compact.cond);

    /* Close the server thread's session. */
    if (conn->background_compact.session != NULL) {
        WT_TRET(__wt_session_close_internal(conn->background_compact.session));
        conn->background_compact.session = NULL;
    }

    return (ret);
}

/*
 * __wt_compact_signal --
 *     Signal the compact thread. Return an error if the background compaction server has not
 *     processed a previous signal yet or because of an invalid configuration.
 */
int
__wt_compact_signal(WT_SESSION_IMPL *session, const char *config)
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    const char *cfg[3] = {NULL, NULL, NULL}, *stripped_config;
    bool running;

    conn = S2C(session);
    cfg[0] = WT_CONFIG_BASE(session, WT_SESSION_compact);
    cfg[1] = config;
    cfg[2] = NULL;
    stripped_config = NULL;

    /* Wait for any previous signal to be processed first. */
    __wt_spin_lock(session, &conn->background_compact.lock);
    if (conn->background_compact.signalled) {
        ret = EBUSY;
        goto err;
    }

    running = conn->background_compact.running;

    WT_ERR(__wt_config_getones(session, config, "background", &cval));
    if (cval.val == running)
        /*
         * This is an error as we are already in the same state and reconfiguration is not allowed.
         */
        WT_ERR_MSG(
          session, EINVAL, "Background compaction is already %s", running ? "enabled" : "disabled");
    conn->background_compact.running = !running;

    /* Strip the background field from the configuration now it has been parsed. */
    WT_ERR(__wt_config_merge(session, cfg, "background=", &stripped_config));
    __wt_free(session, conn->background_compact.config);
    conn->background_compact.config = stripped_config;

    conn->background_compact.signalled = true;

err:
    __wt_spin_unlock(session, &conn->background_compact.lock);
    if (ret == 0)
        __wt_cond_signal(session, conn->background_compact.cond);
    return (ret);
}
