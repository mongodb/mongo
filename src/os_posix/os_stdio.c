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
    const char *name, const char *mode, u_int flags, FILE **fpp)
{
	WT_DECL_RET;
	const char *path;
	char *buf;

	WT_RET(__wt_verbose(session, WT_VERB_FILEOPS, "%s: fopen", name));

	buf = NULL;
	if (LF_ISSET(WT_FOPEN_FIXED))
		path = name;
	else {
		WT_RET(__wt_filename(session, name, &buf));
		path = buf;
	}

#ifdef _WIN32
	{
	char buf[10];
	/*
	 * Open in binary (untranslated) mode; translations involving
	 * carriage-return and linefeed characters are suppressed.
	 */
	(void)snprintf(buf, sizeof(buf), "%s" "b", mode);

	*fpp = fopen(path, buf);
	}
#else
	*fpp = fopen(path, mode);
#endif
	if (*fpp == NULL)
		ret = __wt_errno();

	if (buf != NULL)
		__wt_free(session, buf);

	if (ret == 0)
		return (0);
	WT_RET_MSG(session, ret, "%s: fopen", name);
}

/*
 * __wt_vfprintf --
 *	Vfprintf for a FILE handle.
 */
int
__wt_vfprintf(WT_SESSION_IMPL *session, FILE *fp, const char *fmt, va_list ap)
{
	WT_DECL_RET;

	WT_UNUSED(session);

	return (vfprintf(fp, fmt, ap) < 0 ? __wt_errno() : ret);
}

/*
 * __wt_fprintf --
 *	Fprintf for a FILE handle.
 */
int
__wt_fprintf(WT_SESSION_IMPL *session, FILE *fp, const char *fmt, ...)
    WT_GCC_FUNC_ATTRIBUTE((format (printf, 3, 4)))
{
	WT_DECL_RET;
	va_list ap;

	WT_UNUSED(session);

	va_start(ap, fmt);
	ret = __wt_vfprintf(session, fp, fmt, ap);
	va_end(ap);

	return (ret);
}

/*
 * __wt_fflush --
 *	Flush a FILE handle.
 */
int
__wt_fflush(WT_SESSION_IMPL *session, FILE *fp)
{
	WT_UNUSED(session);

	/* Flush the handle. */
	return (fflush(fp) == 0 ? 0 : __wt_errno());
}

/*
 * __wt_fclose --
 *	Close a FILE handle.
 */
int
__wt_fclose(WT_SESSION_IMPL *session, FILE **fpp)
{
	WT_DECL_RET;

	WT_UNUSED(session);

	/* Close the handle (which implicitly flushes the file to disk). */
	if (*fpp != NULL) {
		if (fclose(*fpp) != 0)
			ret = __wt_errno();
		*fpp = NULL;
	}

	return (ret);
}
