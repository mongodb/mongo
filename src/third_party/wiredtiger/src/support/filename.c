/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_filename --
 *	Build a file name in a scratch buffer, automatically calculate the
 *	length of the file name.
 */
int
__wt_filename(WT_SESSION_IMPL *session, const char *name, char **path)
{
	return (__wt_nfilename(session, name, strlen(name), path));
}

/*
 * __wt_nfilename --
 *	Build a file name in a scratch buffer.  If the name is already an
 *	absolute path duplicate it, otherwise generate a path relative to the
 *	connection home directory.
 */
int
__wt_nfilename(
    WT_SESSION_IMPL *session, const char *name, size_t namelen, char **path)
{
	WT_CONNECTION_IMPL *conn;
	size_t len;
	char *buf;

	conn = S2C(session);
	*path = NULL;

	if (__wt_absolute_path(name))
		WT_RET(__wt_strndup(session, name, namelen, path));
	else {
		len = strlen(conn->home) + 1 + namelen + 1;
		WT_RET(__wt_calloc(session, 1, len, &buf));
		snprintf(buf, len, "%s%s%.*s",
		    conn->home, __wt_path_separator(), (int)namelen, name);
		*path = buf;
	}

	return (0);
}

/*
 * __wt_remove_if_exists --
 *	Remove a file if it exists.
 */
int
__wt_remove_if_exists(WT_SESSION_IMPL *session, const char *name)
{
	int exist;

	WT_RET(__wt_exist(session, name, &exist));
	if (exist)
		WT_RET(__wt_remove(session, name));
	return (0);
}

/*
 * __wt_sync_and_rename_fh --
 *	Sync and close a file, and swap it into place.
 */
int
__wt_sync_and_rename_fh(
    WT_SESSION_IMPL *session, WT_FH **fhp, const char *from, const char *to)
{
	WT_DECL_RET;
	WT_FH *fh;

	fh = *fhp;
	*fhp = NULL;

	/* Flush to disk and close the handle. */
	ret = __wt_fsync(session, fh);
	WT_TRET(__wt_close(session, &fh));
	WT_RET(ret);

	/* Rename the source file to the target. */
	WT_RET(__wt_rename(session, from, to));

	/* Flush the backing directory to guarantee the rename. */
	return (__wt_directory_sync(session, NULL));
}

/*
 * __wt_sync_and_rename_fp --
 *	Sync and close a file, and swap it into place.
 */
int
__wt_sync_and_rename_fp(
    WT_SESSION_IMPL *session, FILE **fpp, const char *from, const char *to)
{
	FILE *fp;

	fp = *fpp;
	*fpp = NULL;

	/* Flush to disk and close the handle. */
	WT_RET(__wt_fclose(session, &fp, WT_FHANDLE_WRITE));

	/* Rename the source file to the target. */
	WT_RET(__wt_rename(session, from, to));

	/* Flush the backing directory to guarantee the rename. */
	return (__wt_directory_sync(session, NULL));
}
