/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_schema_get_source --
 *	Find a matching data source or report an error.
 */
int
__wt_schema_get_source(
    WT_SESSION_IMPL *session, const char *name, WT_DATA_SOURCE **dsrcp)
{
	WT_NAMED_DATA_SOURCE *ndsrc;

	TAILQ_FOREACH(ndsrc, &S2C(session)->dsrcqh, q) {
		if (!WT_PREFIX_MATCH(name, ndsrc->prefix))
			continue;
		*dsrcp = ndsrc->dsrc;
		return (0);
	}

	return (__wt_unknown_object_type(session, name));
}

/*
 * __wt_schema_name_check --
 *	Disallow any use of the WiredTiger name space.
 */
int
__wt_schema_name_check(WT_SESSION_IMPL *session, const char *uri)
{
	const char *name, *sep;

	/*
	 * Check if name is somewhere in the WiredTiger name space: it would be
	 * "bad" if the application truncated the metadata file.  We get passed
	 * both objects and simple strings, skip any leading URI prefix.
	 */
	name = uri;
	if (WT_PREFIX_SKIP(name, "colgroup:") ||
	    WT_PREFIX_SKIP(name, "index:")) {
		/* These URIs normally reference a table name. */
		if ((sep = strchr(name, ':')) != NULL)
			name = sep + 1;
	} else if (!WT_PREFIX_SKIP(name, "table:") &&
	    !WT_PREFIX_SKIP(name, "file:"))
		return (__wt_unknown_object_type(session, uri));

	if (WT_PREFIX_MATCH(name, "WiredTiger"))
		WT_RET_MSG(session, EINVAL,
		    "%s: the \"WiredTiger\" name space may not be used by "
		    "applications",
		    name);

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
