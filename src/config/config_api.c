/*-
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

	__wt_free(config_parser->session, config_parser->config_str);
	__wt_free(config_parser->session, config_parser);
	return (0);
}

/*
 * __config_parser_search --
 *      WT_CONFIG_PARSER->search method.
 */
static int
__config_parser_search(WT_CONFIG_PARSER *wt_config_parser,
     const char *key, WT_CONFIG_ITEM *cval)
{
	WT_CONFIG_PARSER_IMPL *config_parser;

	config_parser = (WT_CONFIG_PARSER_IMPL *)wt_config_parser;

	if (config_parser == NULL)
		return (EINVAL);

	/*
	 * TODO: Search needs to feed into next (want to search based
	 * on our WT_CONFIG, not the config_str.
	 */
	return (__wt_config_getones(config_parser->session,
	    config_parser->config_str, key, cval));
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
 * __config_parser_reset --
 *      WT_CONFIG_PARSER->reset method.
 */
static int
__config_parser_reset(WT_CONFIG_PARSER *wt_config_parser)
{
	WT_CONFIG_PARSER_IMPL *config_parser;

	config_parser = (WT_CONFIG_PARSER_IMPL *)wt_config_parser;

	if (config_parser == NULL)
		return (EINVAL);

	return (__wt_config_init(config_parser->session,
	    &config_parser->config, config_parser->config_str));
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
		__config_parser_reset,
		__config_parser_search
	};
	WT_CONFIG_PARSER_IMPL *config_parser;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	char *config_str;

	*config_parserp = NULL;
	session = (WT_SESSION_IMPL *)wt_session;

	WT_RET(__wt_calloc_def(session, 1, &config_parser));
	config_parser->iface = stds;
	config_parser->session = session;
	/*
	 * Copy the input string to avoid scope issues with the input buffer,
	 * this also allows us to ensure that the string is nul terminated.
	 */
	WT_ERR(__wt_strndup(session, config, len, &config_str));
	WT_ERR(__wt_config_init(
	    session, &config_parser->config, config_str));
	config_parser->config_str = (const char *)config_str;

	if (ret == 0)
		*config_parserp = (WT_CONFIG_PARSER *)config_parser;
	else {
err:		__wt_free(session, config_str);
		__wt_free(session, config_parser);
	}

	return (ret);
}
