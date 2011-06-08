/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_schema_mapping --
 *	Calculate a mapping of column numbers given a list of key columns,
 *	a list of value columns and a list of mapping columns.  The result
 *	is a list of integer column numbers that can be used in repacking
 *	operations.
 */
int
__wt_schema_mapping(WT_SESSION_IMPL *session,
    const char *kcols, const char *vcols, const char *mcols, size_t mlen, int **mapping)
{
	WT_CONFIG kconf, mconf, vconf;		/* Used to walk column lists. */
	WT_CONFIG_ITEM k, mk, v;
	int *end, *p;
	size_t num_columns;
	int found, i, ret;

	/* Count the columns in the mapping. */
	WT_RET(__wt_config_initn(session, &mconf, mcols, mlen));
	for (num_columns = 0;
	    (ret = __wt_config_next(&mconf, &mk, &v)) == 0;
	    num_columns++)
		;
	if (ret != WT_NOTFOUND)
		return (ret);

	WT_RET(__wt_calloc_def(session, num_columns + 1, mapping));
	end = *mapping + num_columns;

	WT_ERR(__wt_config_initn(session, &mconf, mcols, mlen));
	for (p = *mapping; p < end; p++) {
		WT_ERR(__wt_config_next(&mconf, &mk, &v));
		WT_ERR(__wt_config_init(session, &kconf, kcols));
		WT_ERR(__wt_config_init(session, &vconf, vcols));
		for (i = found = 0;
		    !found && (ret = __wt_config_next(&kconf, &k, &v)) == 0;
		    i++)
			found = (k.len == mk.len &&
			    strncmp(k.str, mk.str, mk.len) == 0);
		for (; !found && (ret = __wt_config_next(&vconf, &k, &v)) == 0;
		    i++)
			found = (k.len == mk.len &&
			    strncmp(k.str, mk.str, mk.len) == 0);
		if (!found) {
			__wt_errx(session, "Column '%.*s' not found",
			    (int)mk.len, mk.str);
			ret = EINVAL;
			goto err;
		}
		*p = i;
	}
	*end = -1;

	if (0) {
err:		__wt_free(session, *mapping);
	}
	return (ret);
}
