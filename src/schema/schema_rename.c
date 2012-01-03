/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __rename_file --
 *	Rename a file.
 */
static int
__rename_file(WT_SESSION_IMPL *session,
    const char *fileuri, const char *newname, const char *cfg[])
{
	WT_UNUSED(session);
	WT_UNUSED(fileuri);
	WT_UNUSED(newname);
	WT_UNUSED(cfg);
	return (ENOTSUP);
}

/*
 * __rename_table --
 *	Rename a table.
 */
static int
__rename_table(WT_SESSION_IMPL *session,
    const char *tableuri, const char *newname, const char *cfg[])
{
	WT_UNUSED(session);
	WT_UNUSED(tableuri);
	WT_UNUSED(newname);
	WT_UNUSED(cfg);
	return (ENOTSUP);
}

int
__wt_schema_rename(WT_SESSION_IMPL *session,
    const char *uri, const char *newname, const char *cfg[])
{
	WT_UNUSED(cfg);

	if (WT_PREFIX_MATCH(uri, "file:"))
		return (__rename_file(session, uri, newname, cfg));
	if (WT_PREFIX_MATCH(uri, "table:"))
		return (__rename_table(session, uri, newname, cfg));

	return (__wt_unknown_object_type(session, uri));
}
