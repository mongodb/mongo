/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
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
  *     Needs to work with a NULL session handle - since this is called via
  *     the exists API which is used by the test utilities.
 */
int
__wt_nfilename(
    WT_SESSION_IMPL *session, const char *name, size_t namelen, char **path)
{
	size_t len;
	char *buf;

	*path = NULL;

	if (session == NULL || __wt_absolute_path(name))
		WT_RET(__wt_strndup(session, name, namelen, path));
	else {
		len = strlen(S2C(session)->home) + 1 + namelen + 1;
		WT_RET(__wt_calloc(session, 1, len, &buf));
		snprintf(buf, len, "%s%s%.*s", S2C(session)->home,
		    __wt_path_separator(), (int)namelen, name);
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
	bool exist;

	WT_RET(__wt_exist(session, name, &exist));
	if (exist)
		WT_RET(__wt_remove(session, name));
	return (0);
}

/*
 * __wt_rename_and_sync_directory --
 *	Rename a file and sync the enclosing directory.
 */
int
__wt_rename_and_sync_directory(
    WT_SESSION_IMPL *session, const char *from, const char *to)
{
	const char *fp, *tp;
	bool same_directory;

	/* Rename the source file to the target. */
	WT_RET(__wt_rename(session, from, to));

	/*
	 * Flush the backing directory to guarantee the rename. My reading of
	 * POSIX 1003.1 is there's no guarantee flushing only one of the from
	 * or to directories, or flushing a common parent, is sufficient, and
	 * even if POSIX were to make that guarantee, existing filesystems are
	 * known to not provide the guarantee or only provide the guarantee
	 * with specific mount options. Flush both of the from/to directories
	 * until it's a performance problem.
	 */
	WT_RET(__wt_directory_sync(session, from));

	/*
	 * In almost all cases, we're going to be renaming files in the same
	 * directory, we can at least fast-path that.
	 */
	fp = strrchr(from, '/');
	tp = strrchr(to, '/');
	same_directory = (fp == NULL && tp == NULL) ||
	    (fp != NULL && tp != NULL &&
	    fp - from == tp - to && memcmp(from, to, (size_t)(fp - from)) == 0);

	return (same_directory ? 0 : __wt_directory_sync(session, to));
}

/*
 * __wt_fh_sync_and_rename --
 *	Sync and close a file, and swap it into place.
 */
int
__wt_fh_sync_and_rename(
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

	return (__wt_rename_and_sync_directory(session, from, to));
}

/*
 * __wt_sync_fp_and_rename --
 *	Sync and close a file, and swap it into place.
 */
int
__wt_sync_fp_and_rename(
    WT_SESSION_IMPL *session, FILE **fpp, const char *from, const char *to)
{
	FILE *fp;

	fp = *fpp;
	*fpp = NULL;

	/* Flush to disk and close the handle. */
	WT_RET(__wt_fclose(&fp, WT_FHANDLE_WRITE));

	return (__wt_rename_and_sync_directory(session, from, to));
}
