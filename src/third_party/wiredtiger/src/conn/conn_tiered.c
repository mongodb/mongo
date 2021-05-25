/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#ifdef __GNUC__
#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ > 1)
/*
 * !!!
 * GCC with -Wformat-nonliteral complains about calls to strftime in this file.
 * There's nothing wrong, this makes the warning go away.
 */
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
#endif

/*
 * __flush_tier_once --
 *     Perform one iteration of tiered storage maintenance.
 */
static int
__flush_tier_once(WT_SESSION_IMPL *session, bool force)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    const char *key, *value;

    WT_UNUSED(force);
    __wt_verbose(session, WT_VERB_TIERED, "%s", "FLUSH_TIER_ONCE: Called");
    /*
     * - See if there is any "merging" work to do to prepare and create an object that is
     *   suitable for placing onto tiered storage.
     * - Do the work to create said objects.
     * - Move the objects.
     */
    cursor = NULL;
    WT_RET(__wt_metadata_cursor(session, &cursor));
    while (cursor->next(cursor) == 0) {
        cursor->get_key(cursor, &key);
        cursor->get_value(cursor, &value);
        /* For now just switch tiers which just does metadata manipulation. */
        if (WT_PREFIX_MATCH(key, "tiered:")) {
            __wt_verbose(session, WT_VERB_TIERED, "FLUSH_TIER_ONCE: %s %s", key, value);
            WT_ERR(__wt_session_get_dhandle(session, key, NULL, NULL, WT_DHANDLE_EXCLUSIVE));
            /*
             * When we call wt_tiered_switch the session->dhandle points to the tiered: entry and
             * the arg is the config string that is currently in the metadata.
             */
            WT_ERR(__wt_tiered_switch(session, value));
            WT_ERR(__wt_session_release_dhandle(session));
        }
    }
    WT_ERR(__wt_metadata_cursor_release(session, &cursor));

    return (0);

err:
    WT_TRET(__wt_session_release_dhandle(session));
    WT_TRET(__wt_metadata_cursor_release(session, &cursor));
    return (ret);
}

/*
 * __tier_storage_remove_local --
 *     Perform one iteration of tiered storage local tier removal.
 */
static int
__tier_storage_remove_local(WT_SESSION_IMPL *session, const char *uri, bool force)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    size_t len;
    uint64_t now;
    char *config, *newfile;
    const char *cfg[2], *filename;

    config = newfile = NULL;
    if (uri == NULL)
        return (0);
    __wt_verbose(session, WT_VERB_TIERED, "Removing tree %s", uri);
    filename = uri;
    WT_PREFIX_SKIP_REQUIRED(session, filename, "tiered:");
    len = strlen("file:") + strlen(filename) + 1;
    WT_ERR(__wt_calloc_def(session, len, &newfile));
    WT_ERR(__wt_snprintf(newfile, len, "file:%s", filename));

    /*
     * If the file:URI of the tiered object does not exist, there is nothing to do.
     */
    ret = __wt_metadata_search(session, newfile, &config);
    if (ret == WT_NOTFOUND) {
        ret = 0;
        goto err;
    }
    WT_ERR(ret);

    /*
     * We have a local version of this tiered data. Check its metadata for when it expires and
     * remove if necessary.
     */
    cfg[0] = config;
    cfg[1] = NULL;
    WT_ERR(__wt_config_gets(session, cfg, "local_retention", &cval));
    __wt_seconds(session, &now);
    if (force || (uint64_t)cval.val + S2C(session)->bstorage->retain_secs >= now)
        /*
         * We want to remove the entry and the file. Probably do a schema_drop on the file:uri.
         */
        __wt_verbose(session, WT_VERB_TIERED, "Would remove %s. Local retention expired", newfile);

err:
    __wt_free(session, config);
    __wt_free(session, newfile);
    return (ret);
}

/*
 * __tier_storage_copy --
 *     Perform one iteration of copying newly flushed objects to the shared storage.
 */
static int
__tier_storage_copy(WT_SESSION_IMPL *session)
{
    /*
     * Walk the work queue and copy file:<name> to shared storage object:<name>. Walk a tiered
     * table's tiers array and copy it to any tier that allows WT_TIERS_OP_FLUSH.
     */
    /* XXX: We don't want to call this here, it is just to quiet the compiler that this function
     * can return NULL. So it is a placeholder until we have real content here.
     */
    WT_RET(__tier_storage_remove_local(session, NULL, 0));
    return (0);
}

/*
 * __tier_storage_remove --
 *     Perform one iteration of tiered storage local tier removal.
 */
static int
__tier_storage_remove(WT_SESSION_IMPL *session, bool force)
{
    WT_UNUSED(session);
    WT_UNUSED(force);

    /*
     * We want to walk the metadata perhaps and for each tiered URI, call remove on its file:URI
     * version.
     */
    WT_RET(__tier_storage_remove_local(session, NULL, force));
    return (0);
}

/*
 * __wt_flush_tier --
 *     Entry function for flush_tier method.
 */
int
__wt_flush_tier(WT_SESSION_IMPL *session, const char *config)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    const char *cfg[3];
    bool force;

    WT_STAT_CONN_INCR(session, flush_tier);
    if (FLD_ISSET(S2C(session)->server_flags, WT_CONN_SERVER_TIERED_MGR))
        WT_RET_MSG(
          session, EINVAL, "Cannot call flush_tier when storage manager thread is configured");

    cfg[0] = WT_CONFIG_BASE(session, WT_SESSION_flush_tier);
    cfg[1] = (char *)config;
    cfg[2] = NULL;
    WT_RET(__wt_config_gets(session, cfg, "force", &cval));
    force = cval.val != 0;

    WT_WITH_SCHEMA_LOCK(session, ret = __flush_tier_once(session, force));
    return (ret);
}

/*
 * __tiered_manager_config --
 *     Parse and setup the storage manager options.
 */
static int
__tiered_manager_config(WT_SESSION_IMPL *session, const char **cfg, bool *runp)
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_TIERED_MANAGER *mgr;

    conn = S2C(session);
    mgr = &conn->tiered_mgr;

    /* Only start the server if wait time is non-zero */
    WT_RET(__wt_config_gets(session, cfg, "tiered_manager.wait", &cval));
    mgr->wait_usecs = (uint64_t)cval.val * WT_MILLION;
    if (runp != NULL)
        *runp = mgr->wait_usecs != 0;

    WT_RET(__wt_config_gets(session, cfg, "tiered_manager.threads_max", &cval));
    if (cval.val > WT_TIERED_MAX_WORKERS)
        WT_RET_MSG(session, EINVAL, "Maximum storage workers of %" PRIu32 " larger than %d",
          (uint32_t)cval.val, WT_TIERED_MAX_WORKERS);
    mgr->workers_max = (uint32_t)cval.val;

    WT_RET(__wt_config_gets(session, cfg, "tiered_manager.threads_min", &cval));
    if (cval.val < WT_TIERED_MIN_WORKERS)
        WT_RET_MSG(session, EINVAL, "Minimum storage workers of %" PRIu32 " less than %d",
          (uint32_t)cval.val, WT_TIERED_MIN_WORKERS);
    mgr->workers_min = (uint32_t)cval.val;
    WT_ASSERT(session, mgr->workers_min <= mgr->workers_max);
    return (0);
}

/*
 * __tiered_server_run_chk --
 *     Check to decide if the tiered storage server should continue running.
 */
static bool
__tiered_server_run_chk(WT_SESSION_IMPL *session)
{
    return (FLD_ISSET(S2C(session)->server_flags, WT_CONN_SERVER_TIERED));
}

/*
 * __tiered_server --
 *     The tiered storage server thread.
 */
static WT_THREAD_RET
__tiered_server(void *arg)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_ITEM path, tmp;
    WT_SESSION_IMPL *session;

    session = arg;
    conn = S2C(session);

    WT_CLEAR(path);
    WT_CLEAR(tmp);

    for (;;) {
        /* Wait until the next event. */
        __wt_cond_wait(session, conn->tiered_cond, WT_MINUTE, __tiered_server_run_chk);

        /* Check if we're quitting or being reconfigured. */
        if (!__tiered_server_run_chk(session))
            break;

        /*
         * Here is where we do work. Work we expect to do:
         *  - Copy any files that need moving from a flush tier call.
         *  - Remove any cached objects that are aged out.
         */
        WT_ERR(__tier_storage_copy(session));
        WT_ERR(__tier_storage_remove(session, false));
    }

    if (0) {
err:
        WT_IGNORE_RET(__wt_panic(session, ret, "storage server error"));
    }
    __wt_buf_free(session, &path);
    __wt_buf_free(session, &tmp);
    return (WT_THREAD_RET_VALUE);
}

/*
 * __tiered_mgr_run_chk --
 *     Check to decide if the tiered storage manager should continue running.
 */
static bool
__tiered_mgr_run_chk(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);
    return ((FLD_ISSET(conn->server_flags, WT_CONN_SERVER_TIERED_MGR)) &&
      !F_ISSET(&conn->tiered_mgr, WT_TIERED_MANAGER_SHUTDOWN));
}

/*
 * __tiered_mgr_server --
 *     The tiered storage manager thread.
 */
static WT_THREAD_RET
__tiered_mgr_server(void *arg)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_ITEM path, tmp;
    WT_SESSION_IMPL *session;
    WT_TIERED_MANAGER *mgr;

    session = arg;
    conn = S2C(session);
    mgr = &conn->tiered_mgr;

    WT_CLEAR(path);
    WT_CLEAR(tmp);

    for (;;) {
        /* Wait until the next event. */
        __wt_cond_wait(session, conn->tiered_mgr_cond, mgr->wait_usecs, __tiered_mgr_run_chk);

        /* Check if we're quitting or being reconfigured. */
        if (!__tiered_mgr_run_chk(session))
            break;

        /*
         * Here is where we do work. Work we expect to do:
         */
        WT_WITH_SCHEMA_LOCK(session, ret = __flush_tier_once(session, false));
        WT_ERR(ret);
        WT_ERR(__tier_storage_remove(session, false));
    }

    if (0) {
err:
        WT_IGNORE_RET(__wt_panic(session, ret, "storage server error"));
    }
    __wt_buf_free(session, &path);
    __wt_buf_free(session, &tmp);
    return (WT_THREAD_RET_VALUE);
}
/*
 * __tiered_mgr_start --
 *     Start the tiered manager flush thread.
 */
static int
__tiered_mgr_start(WT_CONNECTION_IMPL *conn)
{
    WT_SESSION_IMPL *session;

    FLD_SET(conn->server_flags, WT_CONN_SERVER_TIERED_MGR);
    WT_RET(__wt_open_internal_session(
      conn, "storage-mgr-server", true, 0, 0, &conn->tiered_mgr_session));
    session = conn->tiered_mgr_session;

    WT_RET(__wt_cond_alloc(session, "storage server", &conn->tiered_mgr_cond));

    /* Start the thread. */
    WT_RET(__wt_thread_create(session, &conn->tiered_mgr_tid, __tiered_mgr_server, session));
    conn->tiered_mgr_tid_set = true;
    return (0);
}

/*
 * __wt_tiered_storage_create --
 *     Start the tiered storage subsystem.
 */
int
__wt_tiered_storage_create(WT_SESSION_IMPL *session, const char *cfg[], bool reconfig)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    bool start;

    conn = S2C(session);
    start = false;

    /* Destroy any existing thread since we could be a reconfigure. */
    WT_RET(__wt_tiered_storage_destroy(session));
    if (reconfig)
        WT_RET(__wt_tiered_conn_config(session, cfg, reconfig));
    WT_RET(__tiered_manager_config(session, cfg, &start));

    /* Start the internal thread. */
    FLD_SET(conn->server_flags, WT_CONN_SERVER_TIERED);

    WT_ERR(__wt_open_internal_session(conn, "storage-server", true, 0, 0, &conn->tiered_session));
    session = conn->tiered_session;

    WT_ERR(__wt_cond_alloc(session, "storage server", &conn->tiered_cond));

    /* Start the thread. */
    WT_ERR(__wt_thread_create(session, &conn->tiered_tid, __tiered_server, session));
    conn->tiered_tid_set = true;

    /* After starting non-configurable threads, start the tiered manager if needed. */
    if (start)
        WT_ERR(__tiered_mgr_start(conn));

    if (0) {
err:
        WT_TRET(__wt_tiered_storage_destroy(session));
    }
    return (ret);
}

/*
 * __wt_tiered_storage_destroy --
 *     Destroy the tiered storage server thread.
 */
int
__wt_tiered_storage_destroy(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    conn = S2C(session);

    /* Stop the internal server thread. */
    FLD_CLR(conn->server_flags, WT_CONN_SERVER_TIERED | WT_CONN_SERVER_TIERED_MGR);
    if (conn->tiered_tid_set) {
        __wt_cond_signal(session, conn->tiered_cond);
        WT_TRET(__wt_thread_join(session, &conn->tiered_tid));
        conn->tiered_tid_set = false;
    }
    __wt_cond_destroy(session, &conn->tiered_cond);
    if (conn->tiered_session != NULL) {
        WT_TRET(__wt_session_close_internal(conn->tiered_session));
        conn->tiered_session = NULL;
    }

    /* Stop the storage manager thread. */
    if (conn->tiered_mgr_tid_set) {
        __wt_cond_signal(session, conn->tiered_mgr_cond);
        WT_TRET(__wt_thread_join(session, &conn->tiered_mgr_tid));
        conn->tiered_mgr_tid_set = false;
    }
    __wt_cond_destroy(session, &conn->tiered_mgr_cond);

    if (conn->tiered_mgr_session != NULL) {
        WT_TRET(__wt_session_close_internal(conn->tiered_mgr_session));
        conn->tiered_mgr_session = NULL;
    }

    return (ret);
}
