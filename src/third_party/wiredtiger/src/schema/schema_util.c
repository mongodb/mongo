/*-
 * Copyright (c) 2014-2017 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_schema_backup_check --
 *	Check if a backup cursor is open and give an error if the schema
 * operation will conflict.  This is called after the schema operations
 * have taken the schema lock so no hot backup cursor can be created until
 * this is done.
 */
int
__wt_schema_backup_check(WT_SESSION_IMPL *session, const char *name)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	int i;
	char **backup_list;

	conn = S2C(session);
	if (!conn->hot_backup)
		return (0);
	__wt_readlock(session, &conn->hot_backup_lock);
	/*
	 * There is a window at the end of a backup where the list has been
	 * cleared from the connection but the flag is still set.  It is safe
	 * to drop at that point.
	 */
	if (!conn->hot_backup ||
	    (backup_list = conn->hot_backup_list) == NULL) {
		__wt_readunlock(session, &conn->hot_backup_lock);
		return (0);
	}
	for (i = 0; backup_list[i] != NULL; ++i) {
		if (strcmp(backup_list[i], name) == 0) {
			ret = EBUSY;
			break;
		}
	}
	__wt_readunlock(session, &conn->hot_backup_lock);
	return (ret);
}

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
 * __wt_str_name_check --
 *	Disallow any use of the WiredTiger name space.
 */
int
__wt_str_name_check(WT_SESSION_IMPL *session, const char *str)
{
	const char *name, *sep;
	int skipped;

	/*
	 * Check if name is somewhere in the WiredTiger name space: it would be
	 * "bad" if the application truncated the metadata file.  Skip any
	 * leading URI prefix, check and then skip over a table name.
	 */
	name = str;
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

/*
 * __wt_name_check --
 *	Disallow any use of the WiredTiger name space.
 */
int
__wt_name_check(WT_SESSION_IMPL *session, const char *str, size_t len)
{
	WT_DECL_RET;
	WT_DECL_ITEM(tmp);

	WT_RET(__wt_scr_alloc(session, len, &tmp));

	WT_ERR(__wt_buf_fmt(session, tmp, "%.*s", (int)len, str));

	ret = __wt_str_name_check(session, tmp->data);

err:	__wt_scr_free(session, &tmp);
	return (ret);
}
