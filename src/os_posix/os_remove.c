/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __remove_file_check --
 *	Check if the file is currently open before removing it.
 */
static void
__remove_file_check(WT_SESSION_IMPL *session, const char *name)
{
#ifdef HAVE_DIAGNOSTIC
	WT_CONNECTION_IMPL *conn;
	WT_FH *fh;

	conn = S2C(session);
	fh = NULL;

	/*
	 * Check if the file is open: it's an error if it is, since a higher
	 * level should have closed it before removing.
	 */
	__wt_spin_lock(session, &conn->fh_lock);
	TAILQ_FOREACH(fh, &conn->fhqh, q) {
		if (strcmp(name, fh->name) == 0)
			break;
	}
	__wt_spin_unlock(session, &conn->fh_lock);

	WT_ASSERT(session, fh == NULL);
#else
	WT_UNUSED(session);
	WT_UNUSED(name);
#endif
}

/*
 * __wt_remove --
 *	Remove a file.
 */
int
__wt_remove(WT_SESSION_IMPL *session, const char *name)
{
	WT_DECL_RET;
	char *path;

	WT_RET(__wt_verbose(session, WT_VERB_FILEOPS, "%s: remove", name));

	__remove_file_check(session, name);

	WT_RET(__wt_filename(session, name, &path));

	WT_SYSCALL_RETRY(remove(path), ret);

	__wt_free(session, path);

	if (ret == 0 || ret == ENOENT)
		return (0);

	WT_RET_MSG(session, ret, "%s: remove", name);
}
