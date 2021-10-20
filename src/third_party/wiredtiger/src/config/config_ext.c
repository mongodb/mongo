/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_ext_config_get --
 *     Given a NULL-terminated list of configuration strings, find the final value for a given
 *     string key (external API version).
 */
int
__wt_ext_config_get(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, WT_CONFIG_ARG *cfg_arg,
  const char *key, WT_CONFIG_ITEM *cval)
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
 * __wt_ext_config_get_string --
 *     Given a configuration string, find the value for a given string key (external API version).
 */
int
__wt_ext_config_get_string(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, const char *config,
  const char *key, WT_CONFIG_ITEM *cval)
{
    WT_CONNECTION_IMPL *conn;
    WT_SESSION_IMPL *session;

    conn = (WT_CONNECTION_IMPL *)wt_api->conn;
    if ((session = (WT_SESSION_IMPL *)wt_session) == NULL)
        session = conn->default_session;

    return (__wt_config_getones(session, config, key, cval));
}

/*
 * __wt_ext_config_parser_open --
 *     WT_EXTENSION_API->config_parser_open implementation
 */
int
__wt_ext_config_parser_open(WT_EXTENSION_API *wt_ext, WT_SESSION *wt_session, const char *config,
  size_t len, WT_CONFIG_PARSER **config_parserp)
{
    WT_UNUSED(wt_ext);
    return (wiredtiger_config_parser_open(wt_session, config, len, config_parserp));
}

/*
 * __wt_ext_config_parser_open_arg --
 *     WT_EXTENSION_API->config_parser_open_arg implementation
 */
int
__wt_ext_config_parser_open_arg(WT_EXTENSION_API *wt_ext, WT_SESSION *wt_session,
  WT_CONFIG_ARG *cfg_arg, WT_CONFIG_PARSER **config_parserp)
{
    size_t len;
    const char **cfg, *p;

    WT_UNUSED(wt_ext);

    /* Find the last non-NULL entry in the configuration stack. */
    if ((cfg = (const char **)cfg_arg) == NULL || *cfg == NULL) {
        p = NULL;
        len = 0;
    } else {
        while (cfg[1] != NULL)
            ++cfg;
        p = *cfg;
        len = strlen(p);
    }

    return (wiredtiger_config_parser_open(wt_session, p, len, config_parserp));
}
