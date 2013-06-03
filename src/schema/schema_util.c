/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_schema_get_source --
 *	Find a matching data source or report an error.
 */
WT_DATA_SOURCE *
__wt_schema_get_source(WT_SESSION_IMPL *session, const char *name)
{
	WT_NAMED_DATA_SOURCE *ndsrc;

	TAILQ_FOREACH(ndsrc, &S2C(session)->dsrcqh, q)
		if (WT_PREFIX_MATCH(name, ndsrc->prefix))
			return (ndsrc->dsrc);
	return (NULL);
}

/*
 * __wt_schema_name_check --
 *	Disallow any use of the WiredTiger name space.
 */
int
__wt_schema_name_check(WT_SESSION_IMPL *session, const char *uri)
{
	const char *name, *sep;
	int skipped;

	/*
	 * Check if name is somewhere in the WiredTiger name space: it would be
	 * "bad" if the application truncated the metadata file.  Skip any
	 * leading URI prefix, check and then skip over a table name.
	 */
	name = uri;
	for (skipped = 0; skipped < 2; skipped++) {
		if ((sep = strchr(name, ':')) == NULL)
			break;

		name = sep + 1;
		if (WT_PREFIX_MATCH(name, "WiredTiger"))
			WT_RET_MSG(session, EINVAL,
			    "%s: the \"WiredTiger\" name space may not be "
			    "used by applications", name);
	}

	/*
	 * Disallow JSON quoting characters -- the config string parsing code
	 * supports quoted strings, but there's no good reason to use them in
	 * names and we're not going to do the testing.
	 */
	if (strpbrk(name, "{},:[]\\\"'") != NULL)
		WT_RET_MSG(session, EINVAL,
		    "%s: WiredTiger objects should not include grouping "
		    "characters in their names",
		    name);

	return (0);
}
