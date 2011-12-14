/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_config_collapse --
 *	Given a NULL-terminated list of configuration strings, where the first
 *	one contains all the defaults, collapse them into a newly allocated
 *	buffer.
 */
int
__wt_config_collapse(WT_SESSION_IMPL *session,
    const char **cfg, const char **config_ret)
{
	WT_BUF buf;
	WT_CONFIG cparser;
	WT_CONFIG_ITEM k, v;
	int ret;

	WT_CLEAR(buf);

	WT_RET(__wt_config_init(session, &cparser, cfg[0]));
	while ((ret = __wt_config_next(&cparser, &k, &v)) == 0) {
		if (k.type != ITEM_STRING && k.type != ITEM_ID) {
			__wt_errx(session,
			    "Invalid configuration key found: '%s'\n", k.str);
			return (EINVAL);
		}
		WT_ERR(__wt_config_get(session, cfg, &k, &v));
		/* Include the quotes around string keys/values. */
		if (k.type == ITEM_STRING) {
			--k.str;
			k.len += 2;
		}
		if (v.type == ITEM_STRING) {
			--v.str;
			v.len += 2;
		}
		WT_ERR(__wt_buf_catfmt(session, &buf, "%.*s=%.*s,",
		    (int)k.len, k.str, (int)v.len, v.str));
	}

	if (ret != WT_NOTFOUND)
		goto err;

	/*
	 * If the caller passes us no valid configuration strings, we end up
	 * here with no allocated memory to return.  Check the final buffer
	 * size: empty configuration strings are possible, and paranoia is
	 * good.
	 */
	if (buf.size == 0)
		WT_RET(__wt_buf_initsize(session, &buf, 1));

	/* Strip the trailing comma and NUL-terminate */
	((char *)buf.data)[buf.size - 1] = '\0';

	*config_ret = buf.data;
	return (0);

err:	__wt_buf_free(session, &buf);
	return (ret);
}
