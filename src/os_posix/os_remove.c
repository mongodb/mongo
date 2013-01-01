/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
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
	WT_DECL_RET;
	WT_FH *fh;
	const char *path;

	conn = S2C(session);
	fh = NULL;

	WT_VERBOSE_RET(session, fileops, "%s: remove", name);

	/* If the file is open, close/free it. */
	__wt_spin_lock(session, &conn->fh_lock);
	TAILQ_FOREACH(fh, &conn->fhqh, q) {
		if (strcmp(name, fh->name) == 0)
			break;
	}
	__wt_spin_unlock(session, &conn->fh_lock);

	/* This should be caught at a higher level. */
	WT_ASSERT(session, fh == NULL);

	WT_RET(__wt_filename(session, name, &path));

	WT_SYSCALL_RETRY(remove(path), ret);

	__wt_free(session, path);

	if (ret == 0 || ret == ENOENT)
		return (0);

	WT_RET_MSG(session, ret, "%s: remove", name);
}
