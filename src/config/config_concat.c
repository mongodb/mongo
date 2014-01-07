/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_config_concat --
 *	Given a NULL-terminated list of configuration strings, concatenate them
 *	into newly allocated memory.  Nothing special is assumed about any of
 *	the config strings, they are simply combined in order.
 *
 *	This code deals with the case where some of the config strings are
 *	wrapped in brackets but others aren't: the resulting string does not
 *	have brackets.
 */
int
__wt_config_concat(
    WT_SESSION_IMPL *session, const char **cfg, const char **config_ret)
{
	WT_CONFIG cparser;
	WT_CONFIG_ITEM k, v;
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	const char **cp;

	WT_RET(__wt_scr_alloc(session, 0, &tmp));

	for (cp = cfg; *cp != NULL; ++cp) {
		WT_ERR(__wt_config_init(session, &cparser, *cp));
		while ((ret = __wt_config_next(&cparser, &k, &v)) == 0) {
			if (k.type != WT_CONFIG_ITEM_STRING &&
			    k.type != WT_CONFIG_ITEM_ID)
				WT_ERR_MSG(session, EINVAL,
				    "Invalid configuration key found: '%s'\n",
				    k.str);
			/* Include the quotes around string keys/values. */
			if (k.type == WT_CONFIG_ITEM_STRING) {
				--k.str;
				k.len += 2;
			}
			if (v.type == WT_CONFIG_ITEM_STRING) {
				--v.str;
				v.len += 2;
			}
			WT_ERR(__wt_buf_catfmt(session, tmp, "%.*s%s%.*s,",
			    (int)k.len, k.str,
			    (v.len > 0) ? "=" : "",
			    (int)v.len, v.str));
		}
		if (ret != WT_NOTFOUND)
			goto err;
	}

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
