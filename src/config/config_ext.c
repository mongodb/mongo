/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_ext_config_get --
 *	Given a NULL-terminated list of configuration strings, find the final
 *	value for a given string key (external API version).
 */
int
__wt_ext_config_get(WT_EXTENSION_API *wt_api,
    WT_SESSION *wt_session, WT_CONFIG_ARG *cfg_arg, const char *key,
    WT_CONFIG_ITEM *cval)
{
	WT_CONNECTION_IMPL *conn;
	WT_SESSION_IMPL *session;
	const char **cfg;

	conn = (WT_CONNECTION_IMPL *)wt_api->conn;
	if ((session = (WT_SESSION_IMPL *)wt_session) == NULL)
		session = conn->default_session;

	if ((cfg = (const char **)cfg_arg) == NULL)
		return (WT_NOTFOUND);
	return (__wt_config_gets(session, cfg, key, cval));
}

/*
 * __wt_ext_config_scan_begin --
 *	Start a scan of a config string.
 * (external API only).
 */
int
__wt_ext_config_scan_begin(
    WT_EXTENSION_API *wt_api, WT_SESSION *wt_session,
    const char *str, size_t len, WT_CONFIG_SCAN **scanp)
{
	WT_CONFIG config, *scan;
	WT_CONNECTION_IMPL *conn;
	WT_SESSION_IMPL *session;

	conn = (WT_CONNECTION_IMPL *)wt_api->conn;
	if ((session = (WT_SESSION_IMPL *)wt_session) == NULL)
		session = conn->default_session;

	/* Note: allocate memory last to avoid cleanup. */
	WT_CLEAR(config);
	WT_RET(__wt_config_initn(session, &config, str, len));
	WT_RET(__wt_calloc_def(session, 1, &scan));
	*scan = config;
	*scanp = (WT_CONFIG_SCAN *)scan;
	return (0);
}

/*
 * __wt_ext_config_scan_end --
 *	End a scan of a config string.
 * (external API only).
 */
int
__wt_ext_config_scan_end(WT_EXTENSION_API *wt_api, WT_CONFIG_SCAN *scan)
{
	WT_CONFIG *conf;

	WT_UNUSED(wt_api);

	conf = (WT_CONFIG *)scan;
	__wt_free(conf->session, scan);
	return (0);
}

/*
 * __wt_ext_config_scan_next --
 *	Get the next key/value pair from a config scan.
 * (external API only).
 */
int
__wt_ext_config_scan_next(
    WT_EXTENSION_API *wt_api, WT_CONFIG_SCAN *scan,
    WT_CONFIG_ITEM *key, WT_CONFIG_ITEM *value)
{
	WT_CONFIG *conf;

	WT_UNUSED(wt_api);

	conf = (WT_CONFIG *)scan;
	return (__wt_config_next(conf, key, value));
}
