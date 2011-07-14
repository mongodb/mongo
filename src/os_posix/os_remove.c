/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_remove --
 *	Remove a file.
 */
int
__wt_remove(WT_SESSION_IMPL *session, const char *name)
{
	WT_CONNECTION_IMPL *conn;
	WT_FH *fh;

	conn = S2C(session);
	fh = NULL;

	 WT_VERBOSE(session, FILEOPS, "fileops: %s: remove", name);

	/* If the file is open, close / free it. */
	__wt_lock(session, conn->mtx);
	TAILQ_FOREACH(fh, &conn->fhqh, q) {
		if (strcmp(name, fh->name) == 0)
			break;
	}
	__wt_unlock(session, conn->mtx);

	/* This should be caught at a higher level. */
	WT_ASSERT(session, fh == NULL);

	if (remove(name) == -1) {
		__wt_err(session, errno, "%s", name);
		return (WT_ERROR);
	}

	return (0);
}
