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
	char *config, *end, *p;
	const char **cp;
	WT_CONFIG cparser;
	WT_CONFIG_ITEM k, v;
	int ret;
	size_t len;

	/*
	 * Be conservative when allocating the buffer: it can't be longer
	 * than the sum of the lengths of the layered configurations.
	 * Add 2 to allow for a trailing comma and NUL.
	 */
	for (cp = cfg, len = 2; *cp != NULL; ++cp)
		len += strlen(*cp);

	WT_RET(__wt_calloc_def(session, len, &config));
	p = config;
	end = config + len;

	WT_RET(__wt_config_init(session, &cparser, cfg[0]));
	while ((ret = __wt_config_next(&cparser, &k, &v)) == 0) {
		if (k.type != ITEM_STRING && k.type != ITEM_ID) {
			__wt_errx(session,
			    "Invalid configuration key found: '%s'\n", k.str);
			return (EINVAL);
		}
		WT_ERR(__wt_config_get(session, cfg, &k, &v));
		/* Include the quotes around string values. */
		if (v.type == ITEM_STRING) {
			--v.str;
			v.len += 2;
		}
		p += snprintf(p, (size_t)(end - p), "%.*s=%.*s,",
		    (int)k.len, k.str, (int)v.len, v.str);
	}

	if (ret == WT_NOTFOUND) {
		ret = 0;
		/* Strip off the trailing comma and NUL-terminate. */
		if (p > config)
			--p;
		*p = '\0';
		*config_ret = config;
	} else {
err:		__wt_free(session, config);
	}
	return (ret);
}
