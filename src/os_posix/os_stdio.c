/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_fp_open --
 *	Open a FILE handle.
 */
int
__wt_fp_open(WT_SESSION_IMPL *session, const char *name, FILE **fpp)
{
	WT_DECL_RET;
	char *path;

	WT_RET(__wt_filename(session, name, &path));

	if ((*fpp = fopen(path, "w")) == NULL)
		ret = __wt_errno();

	__wt_free(session, path);

	return (ret);
}

/*
 * __wt_fp_close --
 *	Close a FILE handle.
 */
int
__wt_fp_close(WT_SESSION_IMPL *session, FILE **fpp)
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
