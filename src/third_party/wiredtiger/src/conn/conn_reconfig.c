/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __conn_compat_parse --
 *     Parse a compatibility release string into its parts.
 */
static int
__conn_compat_parse(
  WT_SESSION_IMPL *session, WT_CONFIG_ITEM *cvalp, uint16_t *majorp, uint16_t *minorp)
{
    uint16_t unused_patch;

    /*
     * Accept either a major.minor.patch release string or a major.minor release string. We ignore
     * the patch value, but allow it in the string.
     */
    /* NOLINTNEXTLINE(cert-err34-c) */
    if (sscanf(cvalp->str, "%" SCNu16 ".%" SCNu16, majorp, minorp) != 2 &&
      /* NOLINTNEXTLINE(cert-err34-c) */
      sscanf(cvalp->str, "%" SCNu16 ".%" SCNu16 ".%" SCNu16, majorp, minorp, &unused_patch) != 3)
        WT_RET_MSG(session, EINVAL, "illegal compatibility release");
    if (*majorp > WIREDTIGER_VERSION_MAJOR)
        WT_RET_MSG(session, ENOTSUP, WT_COMPAT_MSG_PREFIX "unsupported major version");
    if (*majorp == WIREDTIGER_VERSION_MAJOR && *minorp > WIREDTIGER_VERSION_MINOR)
        WT_RET_MSG(session, ENOTSUP, WT_COMPAT_MSG_PREFIX "unsupported minor version");
    return (0);
}

/*
 * __wt_conn_compat_config --
 *     Configure compatibility version.
 */
int
__wt_conn_compat_config(WT_SESSION_IMPL *session, const char **cfg, bool reconfig)
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    uint16_t max_major, max_minor, min_major, min_minor;
    uint16_t rel_major, rel_minor;
    char *value;
    bool txn_active, unchg;

    conn = S2C(session);
    value = NULL;
    max_major = WT_CONN_COMPAT_NONE;
    max_minor = WT_CONN_COMPAT_NONE;
    min_major = WT_CONN_COMPAT_NONE;
    min_minor = WT_CONN_COMPAT_NONE;
    unchg = false;

    WT_RET(__wt_config_gets(session, cfg, "compatibility.release", &cval));
    if (cval.len == 0) {
        rel_major = WIREDTIGER_VERSION_MAJOR;
        rel_minor = WIREDTIGER_VERSION_MINOR;
        F_CLR(conn, WT_CONN_COMPATIBILITY);
    } else {
        WT_RET(__conn_compat_parse(session, &cval, &rel_major, &rel_minor));

        /*
         * If the user is running downgraded, then the compatibility string is part of the
         * configuration string. Determine if the user is actually changing the compatibility.
         */
        if (reconfig && rel_major == conn->compat_major && rel_minor == conn->compat_minor)
            unchg = true;
        else {
            /*
             * We're doing an upgrade or downgrade, check whether transactions are active.
             */
            WT_RET(__wt_txn_activity_check(session, &txn_active));
            if (txn_active)
                WT_RET_MSG(session, ENOTSUP, "system must be quiescent for upgrade or downgrade");
        }
        F_SET(conn, WT_CONN_COMPATIBILITY);
    }
    /*
     * If we're a reconfigure and the user did not set any compatibility or did not change the
     * setting, we're done.
     */
    if (reconfig && (!F_ISSET(conn, WT_CONN_COMPATIBILITY) || unchg))
        goto done;

    /*
     * The maximum and minimum required version for existing files is only available on opening the
     * connection, not reconfigure.
     */
    WT_RET(__wt_config_gets(session, cfg, "compatibility.require_min", &cval));
    if (cval.len != 0)
        WT_RET(__conn_compat_parse(session, &cval, &min_major, &min_minor));

    WT_RET(__wt_config_gets(session, cfg, "compatibility.require_max", &cval));
    if (cval.len != 0)
        WT_RET(__conn_compat_parse(session, &cval, &max_major, &max_minor));

    /*
     * The maximum required must be greater than or equal to the compatibility release we're using
     * now. This is on an open and we're checking the two against each other. We'll check against
     * what was saved on a restart later.
     */
    if (!reconfig && max_major != WT_CONN_COMPAT_NONE &&
      (max_major < rel_major || (max_major == rel_major && max_minor < rel_minor)))
        WT_RET_MSG(session, ENOTSUP,
          WT_COMPAT_MSG_PREFIX "required max of %" PRIu16 ".%" PRIu16
                               "cannot be smaller than compatibility release %" PRIu16 ".%" PRIu16,
          max_major, max_minor, rel_major, rel_minor);

    /*
     * The minimum required must be less than or equal to the compatibility release we're using now.
     * This is on an open and we're checking the two against each other. We'll check against what
     * was saved on a restart later.
     */
    if (!reconfig && min_major != WT_CONN_COMPAT_NONE &&
      (min_major > rel_major || (min_major == rel_major && min_minor > rel_minor)))
        WT_RET_MSG(session, ENOTSUP,
          WT_COMPAT_MSG_PREFIX "required min of %" PRIu16 ".%" PRIu16
                               "cannot be larger than compatibility release %" PRIu16 ".%" PRIu16,
          min_major, min_minor, rel_major, rel_minor);

    /*
     * On a reconfigure, check the new release version against any required maximum version set on
     * open.
     */
    if (reconfig && conn->req_max_major != WT_CONN_COMPAT_NONE &&
      (conn->req_max_major < rel_major ||
        (conn->req_max_major == rel_major && conn->req_max_minor < rel_minor)))
        WT_RET_MSG(session, ENOTSUP,
          WT_COMPAT_MSG_PREFIX "required max of %" PRIu16 ".%" PRIu16
                               "cannot be smaller than requested compatibility release %" PRIu16
                               ".%" PRIu16,
          conn->req_max_major, conn->req_max_minor, rel_major, rel_minor);

    /*
     * On a reconfigure, check the new release version against any required minimum version set on
     * open.
     */
    if (reconfig && conn->req_min_major != WT_CONN_COMPAT_NONE &&
      (conn->req_min_major > rel_major ||
        (conn->req_min_major == rel_major && conn->req_min_minor > rel_minor)))
        WT_RET_MSG(session, ENOTSUP,
          WT_COMPAT_MSG_PREFIX "required min of %" PRIu16 ".%" PRIu16
                               "cannot be larger than requested compatibility release %" PRIu16
                               ".%" PRIu16,
          conn->req_min_major, conn->req_min_minor, rel_major, rel_minor);

    conn->compat_major = rel_major;
    conn->compat_minor = rel_minor;

    /*
     * Only rewrite the turtle file if this is a reconfig. On startup it will get written as part of
     * creating the connection. We do this after checking the required minimum version so that we
     * don't rewrite the turtle file if there is an error.
     */
    if (reconfig)
        WT_RET(__wt_metadata_turtle_rewrite(session));

    /*
     * The required maximum and minimum cannot be set via reconfigure and they are meaningless on a
     * newly created database. We're done in those cases.
     */
    if (reconfig || conn->is_new ||
      (min_major == WT_CONN_COMPAT_NONE && max_major == WT_CONN_COMPAT_NONE))
        goto done;

    /*
     * Check the minimum required against any saved compatibility version in the turtle file saved
     * from an earlier run.
     */
    rel_major = rel_minor = WT_CONN_COMPAT_NONE;
    WT_ERR_NOTFOUND_OK(__wt_metadata_search(session, WT_METADATA_COMPAT, &value), true);
    if (ret == 0) {
        WT_ERR(__wt_config_getones(session, value, "major", &cval));
        rel_major = (uint16_t)cval.val;
        WT_ERR(__wt_config_getones(session, value, "minor", &cval));
        rel_minor = (uint16_t)cval.val;
        if (max_major != WT_CONN_COMPAT_NONE &&
          (max_major < rel_major || (max_major == rel_major && max_minor < rel_minor)))
            WT_ERR_MSG(session, ENOTSUP,
              WT_COMPAT_MSG_PREFIX "required max of %" PRIu16 ".%" PRIu16
                                   "cannot be larger than saved release %" PRIu16 ".%" PRIu16,
              max_major, max_minor, rel_major, rel_minor);
        if (min_major != WT_CONN_COMPAT_NONE &&
          (min_major > rel_major || (min_major == rel_major && min_minor > rel_minor)))
            WT_ERR_MSG(session, ENOTSUP,
              WT_COMPAT_MSG_PREFIX "required min of %" PRIu16 ".%" PRIu16
                                   "cannot be larger than saved release %" PRIu16 ".%" PRIu16,
              min_major, min_minor, rel_major, rel_minor);
    } else if (ret == WT_NOTFOUND)
        ret = 0;

done:
    conn->req_max_major = max_major;
    conn->req_max_minor = max_minor;
    conn->req_min_major = min_major;
    conn->req_min_minor = min_minor;
err:
    __wt_free(session, value);

    return (ret);
}

/*
 * __wt_conn_optrack_setup --
 *     Set up operation logging.
 */
int
__wt_conn_optrack_setup(WT_SESSION_IMPL *session, const char *cfg[], bool reconfig)
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(buf);
    WT_DECL_RET;

    conn = S2C(session);

    /* Once an operation tracking path has been set it can't be changed. */
    if (!reconfig) {
        WT_RET(__wt_config_gets(session, cfg, "operation_tracking.path", &cval));
        WT_RET(__wt_strndup(session, cval.str, cval.len, &conn->optrack_path));
    }

    WT_RET(__wt_config_gets(session, cfg, "operation_tracking.enabled", &cval));
    if (cval.val == 0) {
        if (F_ISSET(conn, WT_CONN_OPTRACK)) {
            WT_RET(__wt_conn_optrack_teardown(session, reconfig));
            F_CLR(conn, WT_CONN_OPTRACK);
        }
        return (0);
    }
    if (F_ISSET(conn, WT_CONN_READONLY))
        /* Operation tracking isn't supported in read-only mode */
        WT_RET_MSG(
          session, EINVAL, "Operation tracking is incompatible with read only configuration");
    if (F_ISSET(conn, WT_CONN_OPTRACK))
        /* Already enabled, nothing else to do */
        return (0);

    /*
     * Operation tracking files will include the ID of the creating process in their name, so we can
     * distinguish between log files created by different WiredTiger processes in the same
     * directory. We cache the process id for future use.
     */
    conn->optrack_pid = __wt_process_id();

    /*
     * Open the file in the same directory that will hold a map of translations between function
     * names and function IDs. If the file exists, remove it.
     */
    WT_RET(__wt_scr_alloc(session, 0, &buf));
    WT_ERR(__wt_filename_construct(
      session, conn->optrack_path, "optrack-map", conn->optrack_pid, UINT32_MAX, buf));
    WT_ERR(__wt_open(session, (const char *)buf->data, WT_FS_OPEN_FILE_TYPE_REGULAR,
      WT_FS_OPEN_CREATE, &conn->optrack_map_fh));

    WT_ERR(__wt_spin_init(session, &conn->optrack_map_spinlock, "optrack map spinlock"));

    WT_ERR(__wt_malloc(session, WT_OPTRACK_BUFSIZE, &conn->dummy_session.optrack_buf));

    /* Set operation tracking on */
    F_SET(conn, WT_CONN_OPTRACK);

err:
    __wt_scr_free(session, &buf);
    return (ret);
}

/*
 * __wt_conn_optrack_teardown --
 *     Clean up connection-wide resources used for operation logging.
 */
int
__wt_conn_optrack_teardown(WT_SESSION_IMPL *session, bool reconfig)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    conn = S2C(session);

    if (!reconfig)
        /* Looks like we are shutting down */
        __wt_free(session, conn->optrack_path);

    if (!F_ISSET(conn, WT_CONN_OPTRACK))
        return (0);

    __wt_spin_destroy(session, &conn->optrack_map_spinlock);

    WT_TRET(__wt_close(session, &conn->optrack_map_fh));
    __wt_free(session, conn->dummy_session.optrack_buf);

    return (ret);
}

/*
 * __wt_conn_statistics_config --
 *     Set statistics configuration.
 */
int
__wt_conn_statistics_config(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CONFIG_ITEM cval, sval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    uint32_t flags;
    int set;

    conn = S2C(session);

    WT_RET(__wt_config_gets(session, cfg, "statistics", &cval));

    flags = 0;
    set = 0;
    if ((ret = __wt_config_subgets(session, &cval, "none", &sval)) == 0 && sval.val != 0) {
        flags = 0;
        ++set;
    }
    WT_RET_NOTFOUND_OK(ret);

    if ((ret = __wt_config_subgets(session, &cval, "fast", &sval)) == 0 && sval.val != 0) {
        LF_SET(WT_STAT_TYPE_FAST);
        ++set;
    }
    WT_RET_NOTFOUND_OK(ret);

    if ((ret = __wt_config_subgets(session, &cval, "all", &sval)) == 0 && sval.val != 0) {
        LF_SET(
          WT_STAT_TYPE_ALL | WT_STAT_TYPE_CACHE_WALK | WT_STAT_TYPE_FAST | WT_STAT_TYPE_TREE_WALK);
        ++set;
    }
    WT_RET_NOTFOUND_OK(ret);

    if (set > 1)
        WT_RET_MSG(
          session, EINVAL, "Only one of all, fast, none configuration values should be specified");

    /*
     * Now that we've parsed general statistics categories, process sub-categories.
     */
    if ((ret = __wt_config_subgets(session, &cval, "cache_walk", &sval)) == 0 && sval.val != 0)
        /*
         * Configuring cache walk statistics implies fast statistics. Keep that knowledge internal
         * for now - it may change in the future.
         */
        LF_SET(WT_STAT_TYPE_FAST | WT_STAT_TYPE_CACHE_WALK);
    WT_RET_NOTFOUND_OK(ret);

    if ((ret = __wt_config_subgets(session, &cval, "tree_walk", &sval)) == 0 && sval.val != 0)
        /*
         * Configuring tree walk statistics implies fast statistics. Keep that knowledge internal
         * for now - it may change in the future.
         */
        LF_SET(WT_STAT_TYPE_FAST | WT_STAT_TYPE_TREE_WALK);
    WT_RET_NOTFOUND_OK(ret);

    if ((ret = __wt_config_subgets(session, &cval, "clear", &sval)) == 0 && sval.val != 0) {
        if (!LF_ISSET(WT_STAT_TYPE_ALL | WT_STAT_TYPE_CACHE_WALK | WT_STAT_TYPE_FAST |
              WT_STAT_TYPE_TREE_WALK))
            WT_RET_MSG(session, EINVAL,
              "the value \"clear\" can only be specified if statistics are enabled");
        LF_SET(WT_STAT_CLEAR);
    }
    WT_RET_NOTFOUND_OK(ret);

    /* Configuring statistics clears any existing values. */
    conn->stat_flags = flags;

    return (0);
}

/*
 * __wt_conn_reconfig --
 *     Reconfigure a connection (internal version).
 */
int
__wt_conn_reconfig(WT_SESSION_IMPL *session, const char **cfg)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    const char *p;

    conn = S2C(session);

    /* Serialize reconfiguration. */
    __wt_spin_lock(session, &conn->reconfig_lock);
    F_SET(conn, WT_CONN_RECONFIGURING);

    /*
     * The configuration argument has been checked for validity, update the previous connection
     * configuration.
     *
     * DO NOT merge the configuration before the reconfigure calls. Some of the underlying
     * reconfiguration functions do explicit checks with the second element of the configuration
     * array, knowing the defaults are in slot #1 and the application's modifications are in slot
     * #2.
     *
     * Replace the base configuration set up by CONNECTION_API_CALL with the current connection
     * configuration, otherwise reconfiguration functions will find the base value instead of
     * previously configured value.
     */
    cfg[0] = conn->cfg;

    /*
     * Reconfigure the system.
     *
     * The compatibility version check is special: upgrade / downgrade cannot be done with
     * transactions active, and checkpoints must not span a version change. Hold the checkpoint lock
     * to avoid conflicts with WiredTiger's checkpoint thread, and rely on the documentation
     * specifying that no new operations can start until the upgrade / downgrade completes.
     */
    WT_WITH_CHECKPOINT_LOCK(session, ret = __wt_conn_compat_config(session, cfg, true));
    WT_ERR(ret);
    WT_ERR(__wt_block_cache_setup(session, cfg, true));
    WT_ERR(__wt_conn_optrack_setup(session, cfg, true));
    WT_ERR(__wt_conn_statistics_config(session, cfg));
    WT_ERR(__wt_cache_config(session, cfg, true));
    WT_ERR(__wt_capacity_server_create(session, cfg));
    WT_ERR(__wt_checkpoint_server_create(session, cfg));
    WT_ERR(__wt_debug_mode_config(session, cfg));
    WT_ERR(__wt_hs_config(session, cfg));
    WT_ERR(__wt_logmgr_reconfig(session, cfg));
    WT_ERR(__wt_lsm_manager_reconfig(session, cfg));
    WT_ERR(__wt_statlog_create(session, cfg));
    WT_ERR(__wt_tiered_conn_config(session, cfg, true));
    WT_ERR(__wt_sweep_config(session, cfg));
    WT_ERR(__wt_timing_stress_config(session, cfg));
    WT_ERR(__wt_json_config(session, cfg, true));
    WT_ERR(__wt_verbose_config(session, cfg, true));

    /* Third, merge everything together, creating a new connection state. */
    WT_ERR(__wt_config_merge(session, cfg, NULL, &p));
    __wt_free(session, conn->cfg);
    conn->cfg = p;

err:
    F_CLR(conn, WT_CONN_RECONFIGURING);
    __wt_spin_unlock(session, &conn->reconfig_lock);

    return (ret);
}
