/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_absolute_path --
 *	Return if a filename is an absolute path.
 */
int
__wt_absolute_path(const char *path)
{
	return (path[0] == '/' ? 1 : 0);
}

/*
 * __wt_filename --
 *	Build a filename in a scratch buffer.
 */
int
__wt_filename(WT_SESSION_IMPL *session, const char *name, const char **path)
{
	WT_CONNECTION_IMPL *conn;
	size_t len;
	char *buf;

	conn = S2C(session);
	*path = NULL;

	len = strlen(conn->home) + 1 + strlen(name) + 1;
	WT_RET(__wt_calloc(session, 1, len, &buf));
	snprintf(buf, len, "%s/%s", conn->home, name);

	*path = buf;
	return (0);
}
