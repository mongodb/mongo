/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_config_collapse --
 *	Given a NULL-terminated list of configuration strings, where the first
 *	one contains all the defaults, collapse them into newly allocated
 *	memory.
 */
int
__wt_config_collapse(
    WT_SESSION_IMPL *session, const char **cfg, const char **config_ret)
{
	WT_CONFIG cparser;
	WT_CONFIG_ITEM k, v;
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;

	WT_RET(__wt_scr_alloc(session, 0, &tmp));

	WT_ERR(__wt_config_init(session, &cparser, cfg[0]));
	while ((ret = __wt_config_next(&cparser, &k, &v)) == 0) {
		if (k.type != WT_CONFIG_ITEM_STRING &&
		    k.type != WT_CONFIG_ITEM_ID)
			WT_ERR_MSG(session, EINVAL,
			    "Invalid configuration key found: '%s'\n", k.str);
		WT_ERR(__wt_config_get(session, cfg, &k, &v));
		/* Include the quotes around string keys/values. */
		if (k.type == WT_CONFIG_ITEM_STRING) {
			--k.str;
			k.len += 2;
		}
		if (v.type == WT_CONFIG_ITEM_STRING) {
			--v.str;
			v.len += 2;
		}
		WT_ERR(__wt_buf_catfmt(session, tmp, "%.*s=%.*s,",
		    (int)k.len, k.str, (int)v.len, v.str));
	}
	if (ret != WT_NOTFOUND)
		goto err;

	/*
	 * If the caller passes us no valid configuration strings, we get here
	 * with no bytes to copy -- that's OK, the underlying string copy can
	 * handle empty strings.
	 *
	 * Strip any trailing comma.
	 */
	if (tmp->size != 0)
		--tmp->size;
	ret = __wt_strndup(session, tmp->data, tmp->size, config_ret);

err:	__wt_scr_free(&tmp);
	return (ret);
}
