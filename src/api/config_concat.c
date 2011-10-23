/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_config_concat --
 *	Given a NULL-terminated list of configuration strings, concatenate them
 *	into a newly allocated buffer.  Nothing special is assumed about any
 *	of the config strings, they are simply combined in order.
 *
 *	This code deals with the case where some of the config strings are
 *	wrapped in brackets but others aren't: the resulting string does not
 *	have brackets.
 */
int
__wt_config_concat(
    WT_SESSION_IMPL *session, const char **cfg, const char **config_ret)
{
	WT_BUF buf;
	WT_CONFIG cparser;
	WT_CONFIG_ITEM k, v;
	int ret;
	const char **cp;

	WT_CLEAR(buf);
	ret = 0;

	for (cp = cfg; *cp != NULL; ++cp) {
		WT_ERR(__wt_config_init(session, &cparser, *cp));
		while ((ret = __wt_config_next(&cparser, &k, &v)) == 0) {
			if (k.type != ITEM_STRING && k.type != ITEM_ID) {
				__wt_errx(session,
				    "Invalid configuration key found: '%s'\n",
				    k.str);
				WT_ERR(EINVAL);
			}
			/* Include the quotes around string keys/values. */
			if (k.type == ITEM_STRING) {
				--k.str;
				k.len += 2;
			}
			if (v.type == ITEM_STRING) {
				--v.str;
				v.len += 2;
			}
			WT_ERR(__wt_buf_catfmt(session, &buf, "%.*s%s%.*s,",
			    (int)k.len, k.str,
			    (v.len > 0) ? "=" : "",
			    (int)v.len, v.str));
		}
		if (ret != WT_NOTFOUND)
			goto err;
	}

	/*
	 * If the caller passes us no valid configuration strings, we end up
	 * here with no allocated memory to return.  Check the final buffer
	 * size: empty configuration strings are possible, and paranoia is
	 * good.
	 */
	if (buf.size == 0)
		WT_RET(__wt_buf_initsize(session, &buf, 1));

	*config_ret = buf.data;
	return (0);

err:	__wt_buf_free(session, &buf);
	return (ret);
}
