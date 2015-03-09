/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __config_parser_close --
 *      WT_CONFIG_PARSER->close method.
 */
static int
__config_parser_close(WT_CONFIG_PARSER *wt_config_parser)
{
	WT_CONFIG_PARSER_IMPL *config_parser;

	config_parser = (WT_CONFIG_PARSER_IMPL *)wt_config_parser;

	if (config_parser == NULL)
		return (EINVAL);

	__wt_free(config_parser->session, config_parser);
	return (0);
}

/*
 * __config_parser_get --
 *      WT_CONFIG_PARSER->search method.
 */
static int
__config_parser_get(WT_CONFIG_PARSER *wt_config_parser,
     const char *key, WT_CONFIG_ITEM *cval)
{
	WT_CONFIG_PARSER_IMPL *config_parser;

	config_parser = (WT_CONFIG_PARSER_IMPL *)wt_config_parser;

	if (config_parser == NULL)
		return (EINVAL);

	return (__wt_config_subgets(config_parser->session,
	    &config_parser->config_item, key, cval));
}

/*
 * __config_parser_next --
 *	WT_CONFIG_PARSER->next method.
 */
static int
__config_parser_next(WT_CONFIG_PARSER *wt_config_parser,
     WT_CONFIG_ITEM *key, WT_CONFIG_ITEM *cval)
{
	WT_CONFIG_PARSER_IMPL *config_parser;

	config_parser = (WT_CONFIG_PARSER_IMPL *)wt_config_parser;

	if (config_parser == NULL)
		return (EINVAL);

	return (__wt_config_next(&config_parser->config, key, cval));
}

/*
 * wiredtiger_config_parser_open --
 *	Create a configuration parser.
 */
int
wiredtiger_config_parser_open(WT_SESSION *wt_session,
    const char *config, size_t len, WT_CONFIG_PARSER **config_parserp)
{
	static const WT_CONFIG_PARSER stds = {
		__config_parser_close,
		__config_parser_next,
		__config_parser_get
	};
	WT_CONFIG_ITEM config_item =
	    { config, len, 0, WT_CONFIG_ITEM_STRING };
	WT_CONFIG_PARSER_IMPL *config_parser;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	*config_parserp = NULL;
	session = (WT_SESSION_IMPL *)wt_session;

	WT_RET(__wt_calloc_one(session, &config_parser));
	config_parser->iface = stds;
	config_parser->session = session;

	/*
	 * Setup a WT_CONFIG_ITEM to be used for get calls and a WT_CONFIG
	 * structure for iterations through the configuration string.
	 */
	memcpy(&config_parser->config_item, &config_item, sizeof(config_item));
	WT_ERR(__wt_config_initn(session, &config_parser->config, config, len));

	if (ret == 0)
		*config_parserp = (WT_CONFIG_PARSER *)config_parser;
	else
err:		__wt_free(session, config_parser);

	return (ret);
}

/*
 * wiredtiger_config_validate --
 *	Validate a configuration string.
 */
int
wiredtiger_config_validate(
    WT_SESSION *wt_session, const char *method, const char *config)
{
	WT_CONNECTION_IMPL *conn;
	WT_SESSION_IMPL *session;
	const WT_CONFIG_ENTRY *ep, **epp;

	session = (WT_SESSION_IMPL *)wt_session;

	if (method == NULL)
		WT_RET_MSG(session, EINVAL, "no method specified");
	if (config == NULL)
		WT_RET_MSG(session, EINVAL, "no configuration specified");

	/*
	 * If we don't yet have a connection, look for a matching method in
	 * the static list, otherwise look in the configuration list (which
	 * has any configuration information the application has added).
	 */
	if (session == NULL)
		ep = __wt_conn_config_match(method);
	else {
		conn = S2C(session);

		for (epp = conn->config_entries; (*epp)->method != NULL; ++epp)
			if (strcmp((*epp)->method, method) == 0)
				break;
		ep = *epp;
	}
	if (ep == NULL || ep->method == NULL)
		WT_RET_MSG(session,
		    WT_NOTFOUND, "no method matching %s found", method);

	return (__wt_config_check(session, ep, config, 0));
}
