/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_filesize --
 *	Get the size of a file in bytes.
 */
int
__wt_filesize(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t *sizep)
{
	WT_DECL_RET;
	LARGE_INTEGER size;

	WT_RET(__wt_verbose(
	    session, WT_VERB_FILEOPS, "%s: GetFileSizeEx", fh->name));

	if ((ret = GetFileSizeEx(fh->filehandle, &size)) != 0) {
		*sizep = size.QuadPart;
		return (0);
	}

	WT_RET_MSG(session, __wt_errno(), "%s: GetFileSizeEx", fh->name);
}

/*
 * __wt_filesize_name --
 *	Return the size of a file in bytes, given a file name.
 */
int
__wt_filesize_name(
    WT_SESSION_IMPL *session, const char *filename, wt_off_t *sizep)
{
	WT_DECL_RET;
	WIN32_FILE_ATTRIBUTE_DATA data;
	char *path;

	WT_RET(__wt_filename(session, filename, &path));

	ret = GetFileAttributesExA(path, GetFileExInfoStandard, &data);

	__wt_free(session, path);

	if (ret != 0)
		*sizep =
		    ((int64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;

	/* Some callers expect failure, so don't log an error message. */
	return (ret);
}
