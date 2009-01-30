/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * __wt_open --
 *	Open a file handle.
 */
int
__wt_open(ENV *env,
    const char *name, mode_t mode, u_int32_t flags, WT_FH **fhp)
{
	WT_FH *fh;
	int f, fd, ret;

	fh = NULL;

	if (FLD_ISSET(env->verbose, WT_VERB_FILEOPS | WT_VERB_FILEOPS_ALL))
		__wt_env_errx(env, "fileops: %s: open", name);

	f = O_RDWR;
#ifdef O_BINARY
	/* Windows clones: we always want to treat the file as a binary. */
	f |= O_BINARY;
#endif
	if (LF_ISSET(WT_OPEN_CREATE))
		f |= O_CREAT;

	if ((fd = open(name, f, mode)) == -1) {
		__wt_env_err(env, errno, "%s", name);
		return (WT_ERROR);
	}

	if ((ret = __wt_calloc(env, 1, sizeof(WT_FH), &fh)) != 0)
		return (ret);
	if ((ret = __wt_stat_alloc_fh_stats(env, &fh->stats)) != 0)
		goto err;
	if ((ret = __wt_strdup(env, name, &fh->name)) != 0)
		goto err;

#if defined(HAVE_FCNTL) && defined(FD_CLOEXEC)
	/*
	 * Security:
	 * The application may spawn a new process, and we don't want another
	 * process to have access to our file handles.  There's an obvious
	 * race here...
	 */
	if ((f = fcntl(fd, F_GETFD)) == -1 ||
	    fcntl(fd, F_SETFD, f | FD_CLOEXEC) == -1) {
		__wt_env_err(env, errno, "%s: fcntl", name);
		goto err;
	}
#endif

	fh->fd = fd;
	*fhp = fh;
	return (0);

err:	if (fh != NULL) {
		if (fh->name != NULL)
			__wt_free(env, fh->name);
		__wt_free(env, fh);
	}
	(void)close(fd);
	return (WT_ERROR);
}

/*
 * __wt_close --
 *	Close a file handle.
 */
int
__wt_close(ENV *env, WT_FH *fh)
{
	__wt_free(env, fh->name);
	__wt_free(env, fh->stats);
	__wt_free(env, fh);
	return (0);
}
