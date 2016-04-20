/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * __wt_getline --
 *	Get a line from a stream.
 */
static inline int
__wt_getline(WT_SESSION_IMPL *session, WT_FSTREAM *fs, WT_ITEM *buf)
{
	return (fs->getline(session, fs, buf));
}

/*
 * __wt_fclose --
 *	Close a stream.
 */
static inline int
__wt_fclose(WT_SESSION_IMPL *session, WT_FSTREAM **fsp)
{
	WT_FSTREAM *fs;

	if ((fs = *fsp) == NULL)
		return (0);
	*fsp = NULL;
	return (fs->close(session, fs));
}

/*
 * __wt_fflush --
 *	Flush a stream.
 */
static inline int
__wt_fflush(WT_SESSION_IMPL *session, WT_FSTREAM *fs)
{
	return (fs->flush(session, fs));
}

/*
 * __wt_vfprintf --
 *	ANSI C vfprintf.
 */
static inline int
__wt_vfprintf(
    WT_SESSION_IMPL *session, WT_FSTREAM *fs, const char *fmt, va_list ap)
{
	WT_RET(__wt_verbose(
	    session, WT_VERB_HANDLEOPS, "%s: handle-printf", fs->name));

	return (fs->printf(session, fs, fmt, ap));
}

/*
 * __wt_fprintf --
 *	ANSI C fprintf.
 */
static inline int
__wt_fprintf(WT_SESSION_IMPL *session, WT_FSTREAM *fs, const char *fmt, ...)
    WT_GCC_FUNC_ATTRIBUTE((format (printf, 3, 4)))
{
	WT_DECL_RET;
	va_list ap;

	va_start(ap, fmt);
	ret = __wt_vfprintf(session, fs, fmt, ap);
	va_end(ap);

	return (ret);
}

/*
 * __wt_sync_and_rename --
 *	Flush and close a stream, then swap it into place.
 */
static inline int
__wt_sync_and_rename(WT_SESSION_IMPL *session,
    WT_FSTREAM **fsp, const char *from, const char *to)
{
	WT_DECL_RET;
	WT_FSTREAM *fs;

	fs = *fsp;
	*fsp = NULL;

	/* Flush to disk and close the handle. */
	WT_TRET(__wt_fflush(session, fs));
	WT_TRET(__wt_fsync(session, fs->fh, true));
	WT_TRET(__wt_fclose(session, &fs));
	WT_RET(ret);

	return (__wt_rename_and_sync_directory(session, from, to));
}
