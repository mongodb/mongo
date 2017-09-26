/*-
 * Copyright (c) 2014-2017 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_conn_compat_config --
 *	Configure compatibility version.
 */
int
__wt_conn_compat_config(WT_SESSION_IMPL *session, const char **cfg)
{
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;
	uint16_t major, minor, patch;
	bool txn_active;

	conn = S2C(session);
	WT_RET(__wt_config_gets(session, cfg, "compatibility.release", &cval));
	if (cval.len == 0) {
		conn->compat_major = WIREDTIGER_VERSION_MAJOR;
		conn->compat_minor = WIREDTIGER_VERSION_MINOR;
		return (0);
	}

	/*
	 * Accept either a major.minor release string or a major.minor.patch
	 * release string.  We ignore the patch value, but allow it in the
	 * string.
	 */
	if (sscanf(cval.str, "%" SCNu16 ".%" SCNu16, &major, &minor) != 2 &&
	    sscanf(cval.str, "%" SCNu16 ".%" SCNu16 ".%" SCNu16,
	    &major, &minor, &patch) != 3)
		WT_RET_MSG(session, EINVAL, "illegal compatibility release");
	if (major > WIREDTIGER_VERSION_MAJOR)
		WT_RET_MSG(session, ENOTSUP, "unsupported major version");
	if (major == WIREDTIGER_VERSION_MAJOR &&
	    minor > WIREDTIGER_VERSION_MINOR)
		WT_RET_MSG(session, ENOTSUP, "unsupported minor version");

	/*
	 * We're doing an upgrade or downgrade, check whether transactions are
	 * active.
	 */
	WT_RET(__wt_txn_activity_check(session, &txn_active));
	if (txn_active)
		WT_RET_MSG(session, ENOTSUP,
		    "system must be quiescent for upgrade or downgrade");
	conn->compat_major = major;
	conn->compat_minor = minor;
	return (0);
}

/*
 * __wt_conn_statistics_config --
 *	Set statistics configuration.
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
	if ((ret = __wt_config_subgets(
	    session, &cval, "none", &sval)) == 0 && sval.val != 0) {
		flags = 0;
		++set;
	}
	WT_RET_NOTFOUND_OK(ret);

	if ((ret = __wt_config_subgets(
	    session, &cval, "fast", &sval)) == 0 && sval.val != 0) {
		LF_SET(WT_STAT_TYPE_FAST);
		++set;
	}
	WT_RET_NOTFOUND_OK(ret);

	if ((ret = __wt_config_subgets(
	    session, &cval, "all", &sval)) == 0 && sval.val != 0) {
		LF_SET(
		    WT_STAT_TYPE_ALL | WT_STAT_TYPE_CACHE_WALK |
		    WT_STAT_TYPE_FAST | WT_STAT_TYPE_TREE_WALK);
		++set;
	}
	WT_RET_NOTFOUND_OK(ret);

	if (set > 1)
		WT_RET_MSG(session, EINVAL,
		    "Only one of all, fast, none configuration values should "
		    "be specified");

	/*
	 * Now that we've parsed general statistics categories, process
	 * sub-categories.
	 */
	if ((ret = __wt_config_subgets(
	    session, &cval, "cache_walk", &sval)) == 0 && sval.val != 0)
		/*
		 * Configuring cache walk statistics implies fast statistics.
		 * Keep that knowledge internal for now - it may change in the
		 * future.
		 */
		LF_SET(WT_STAT_TYPE_FAST | WT_STAT_TYPE_CACHE_WALK);
	WT_RET_NOTFOUND_OK(ret);

	if ((ret = __wt_config_subgets(
	    session, &cval, "tree_walk", &sval)) == 0 && sval.val != 0)
		/*
		 * Configuring tree walk statistics implies fast statistics.
		 * Keep that knowledge internal for now - it may change in the
		 * future.
		 */
		LF_SET(WT_STAT_TYPE_FAST | WT_STAT_TYPE_TREE_WALK);
	WT_RET_NOTFOUND_OK(ret);

	if ((ret = __wt_config_subgets(
	    session, &cval, "clear", &sval)) == 0 && sval.val != 0) {
		if (!LF_ISSET(WT_STAT_TYPE_ALL | WT_STAT_TYPE_CACHE_WALK |
		    WT_STAT_TYPE_FAST | WT_STAT_TYPE_TREE_WALK))
			WT_RET_MSG(session, EINVAL,
			    "the value \"clear\" can only be specified if "
			    "statistics are enabled");
		LF_SET(WT_STAT_CLEAR);
	}
	WT_RET_NOTFOUND_OK(ret);

	/* Configuring statistics clears any existing values. */
	conn->stat_flags = flags;

	return (0);
}

/*
 * __wt_conn_reconfig --
 *	Reconfigure a connection (internal version).
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

	/*
	 * The configuration argument has been checked for validity, update the
	 * previous connection configuration.
	 *
	 * DO NOT merge the configuration before the reconfigure calls.  Some
	 * of the underlying reconfiguration functions do explicit checks with
	 * the second element of the configuration array, knowing the defaults
	 * are in slot #1 and the application's modifications are in slot #2.
	 *
	 * Replace the base configuration set up by CONNECTION_API_CALL with
	 * the current connection configuration, otherwise reconfiguration
	 * functions will find the base value instead of previously configured
	 * value.
	 */
	cfg[0] = conn->cfg;

	/*
	 * Reconfigure the system.
	 *
	 * The compatibility version check is special: upgrade / downgrade
	 * cannot be done with transactions active, and checkpoints must not
	 * span a version change.  Hold the checkpoint lock to avoid conflicts
	 * with WiredTiger's checkpoint thread, and rely on the documentation
	 * specifying that no new operations can start until the upgrade /
	 * downgrade completes.
	 */
	WT_WITH_CHECKPOINT_LOCK(session,
	    ret = __wt_conn_compat_config(session, cfg));
	WT_ERR(ret);
	WT_ERR(__wt_conn_statistics_config(session, cfg));
	WT_ERR(__wt_async_reconfig(session, cfg));
	WT_ERR(__wt_cache_config(session, true, cfg));
	WT_ERR(__wt_checkpoint_server_create(session, cfg));
	WT_ERR(__wt_logmgr_reconfig(session, cfg));
	WT_ERR(__wt_lsm_manager_reconfig(session, cfg));
	WT_ERR(__wt_statlog_create(session, cfg));
	WT_ERR(__wt_sweep_config(session, cfg));
	WT_ERR(__wt_verbose_config(session, cfg));
	WT_ERR(__wt_timing_stress_config(session, cfg));

	/* Third, merge everything together, creating a new connection state. */
	WT_ERR(__wt_config_merge(session, cfg, NULL, &p));
	__wt_free(session, conn->cfg);
	conn->cfg = p;

err:	__wt_spin_unlock(session, &conn->reconfig_lock);

	return (ret);
}
