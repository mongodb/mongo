/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_schema_name_check --
 *	Disallow any use of the WiredTiger name space.
 */
int
__wt_schema_name_check(WT_SESSION_IMPL *session, const char *uri)
{
	const char *name;

	/*
	 * Check if name is somewhere in the WiredTiger name space: it would be
	 * "bad" if the application truncated the schema file.  We get passed
	 * both objects and simple strings, skip any leading URI prefix.
	 */
	if ((name = strchr(uri, ':')) == NULL)
		name = uri;
	else
		++name;
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
