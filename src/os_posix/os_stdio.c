/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_fopen --
 *	Open a FILE handle.
 */
int
__wt_fopen(WT_SESSION_IMPL *session,
    const char *name, WT_FHANDLE_MODE mode_flag, u_int flags, FILE **fpp)
{
	WT_DECL_RET;
	const char *mode, *path;
	char *pathbuf;

	WT_RET(__wt_verbose(session, WT_VERB_FILEOPS, "%s: fopen", name));

	pathbuf = NULL;
	if (LF_ISSET(WT_FOPEN_FIXED))
		path = name;
	else {
		WT_RET(__wt_filename(session, name, &pathbuf));
		path = pathbuf;
	}

	mode = NULL;
	switch (mode_flag) {
	case WT_FHANDLE_APPEND:
		mode = WT_FOPEN_APPEND;
		break;
	case WT_FHANDLE_READ:
		mode = WT_FOPEN_READ;
		break;
	case WT_FHANDLE_WRITE:
		mode = WT_FOPEN_WRITE;
		break;
	}
	*fpp = fopen(path, mode);
	if (*fpp == NULL)
		ret = __wt_errno();

	if (pathbuf != NULL)
		__wt_free(session, pathbuf);

	if (ret == 0)
		return (0);
	WT_RET_MSG(session, ret, "%s: fopen", name);
}

/*
 * __wt_vfprintf --
 *	Vfprintf for a FILE handle.
 */
int
__wt_vfprintf(FILE *fp, const char *fmt, va_list ap)
{
	return (vfprintf(fp, fmt, ap) < 0 ? __wt_errno() : 0);
}

/*
 * __wt_fprintf --
 *	Fprintf for a FILE handle.
 */
int
__wt_fprintf(FILE *fp, const char *fmt, ...)
    WT_GCC_FUNC_ATTRIBUTE((format (printf, 2, 3)))
{
	WT_DECL_RET;
	va_list ap;

	va_start(ap, fmt);
	ret = __wt_vfprintf(fp, fmt, ap);
	va_end(ap);

	return (ret);
}

/*
 * __wt_fflush --
 *	Flush a FILE handle.
 */
int
__wt_fflush(FILE *fp)
{
	/* Flush the handle. */
	return (fflush(fp) == 0 ? 0 : __wt_errno());
}

/*
 * __wt_fclose --
 *	Close a FILE handle.
 */
int
__wt_fclose(FILE **fpp, WT_FHANDLE_MODE mode_flag)
{
	FILE *fp;
	WT_DECL_RET;

	if (*fpp == NULL)
		return (0);

	fp = *fpp;
	*fpp = NULL;

	/*
	 * If the handle was opened for writing, flush the file to the backing
	 * OS buffers, then flush the OS buffers to the backing disk.
	 */
	if (mode_flag == WT_FHANDLE_APPEND || mode_flag == WT_FHANDLE_WRITE) {
		ret = __wt_fflush(fp);
		if (fsync(fileno(fp)) != 0)
			WT_TRET(__wt_errno());
	}

	/* Close the handle. */
	if (fclose(fp) != 0)
		WT_TRET(__wt_errno());

	return (ret);
}
