/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

int
__wt_filesize(WT_TOC *toc, WT_FH *fh, off_t *sizep)
{
	struct stat sb;
	ENV *env;

	env = toc->env;

	if (FLD_ISSET(env->verbose, WT_VERB_FILEOPS | WT_VERB_FILEOPS_ALL))
		__wt_env_errx(env, "fileops: %s: fstat", fh->name);

	if (fstat(fh->fd, &sb) == -1) {
		__wt_env_err(env, errno, "%s: fstat", fh->name);
		return (WT_ERROR);
	}

	*sizep = sb.st_size;		/* Return size in bytes. */
	return (0);
}
