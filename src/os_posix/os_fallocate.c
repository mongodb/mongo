/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#if defined(HAVE_FALLOCATE) || defined(__linux__)
#include <linux/falloc.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

/*
 * __wt_fallocate_config --
 *	Configure fallocate behavior for a file handle.
 */
void
__wt_fallocate_config(WT_SESSION_IMPL *session, WT_FH *fh)
{
	WT_UNUSED(session);

	fh->fallocate_available = 0;
	fh->fallocate_requires_locking = 0;

#ifdef __linux__
	/*
	 * We've seen Linux systems where posix_fallocate corrupts existing data
	 * (even though that is explicitly disallowed by POSIX).  We've not seen
	 * problems with fallocate, it's unlocked for now.
	 */
#if defined(HAVE_FALLOCATE)
	fh->fallocate_available = 1;
	fh->fallocate_requires_locking = 0;
#elif defined(HAVE_POSIX_FALLOCATE)
	fh->fallocate_available = 1;
	fh->fallocate_requires_locking = 1;
#endif
#elif defined(HAVE_POSIX_FALLOCATE)
	/*
	 * FreeBSD and Solaris support posix_fallocate, and so far we've seen
	 * no problems leaving it unlocked.
	 */
	fh->fallocate_available = 1;
	fh->fallocate_requires_locking = 0;
#endif
}

/*
 * __wt_fallocate --
 *	Allocate space for a file handle.
 */
int
__wt_fallocate(
    WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t offset, wt_off_t len)
{
	WT_DECL_RET;

#if defined(HAVE_FALLOCATE)
	WT_RET(__wt_verbose(
	    session, WT_VERB_FILEOPS, "%s: fallocate", fh->name));
	WT_SYSCALL_RETRY(
	    fallocate(fh->fd, FALLOC_FL_KEEP_SIZE, offset, len), ret);
	if (ret == 0)
		return (0);

	/*
	 * Linux returns ENOTSUP for fallocate on some file systems; we return
	 * ENOTSUP, and our caller should avoid calling us again.
	 */
	if (ret != ENOTSUP)
		WT_RET_MSG(session, ret, "%s: fallocate", fh->name);
#elif defined(HAVE_POSIX_FALLOCATE)
	WT_RET(__wt_verbose(
	    session, WT_VERB_FILEOPS, "%s: posix_fallocate", fh->name));

#if defined(__linux__)
    /**
     * We try the direct syscall for fallocate if the libc wrapper was not found.
     * The syscall actually exists in the kernel for RHEL 5.5, but not in
     * the version of libc, so this allows it to work everywhere the kernel
     * supports it.
     */
    WT_SYSCALL_RETRY(syscall(SYS_fallocate, fh->fd, FALLOC_FL_KEEP_SIZE, offset, len), ret);
    if (ret == 0) {
        fh->fallocate_requires_locking = 0;
    }
    else if (ret == ENOSYS) {
        WT_SYSCALL_RETRY(posix_fallocate(fh->fd, offset, len), ret);
    }
    else if (ret != ENOTSUP) {
        // see above handling for ENOTSUP
        WT_RET_MSG(session, ret, "%s: fallocate", fh->name);
    }
#else
    WT_SYSCALL_RETRY(posix_fallocate(fh->fd, offset, len), ret);
#endif

	if (ret == 0)
		return (0);

	/*
	 * Solaris returns EINVAL for posix_fallocate on some file systems; we
	 * return ENOTSUP, and our caller should avoid calling us again.
	 */
	if (ret != EINVAL)
		WT_RET_MSG(session, ret, "%s: posix_fallocate", fh->name);
#else
	WT_UNUSED(session);
	WT_UNUSED(fh);
	WT_UNUSED(offset);
	WT_UNUSED(len);
	WT_UNUSED(ret);
#endif

	fh->fallocate_available = 0;
	fh->fallocate_requires_locking = 0;
	return (ENOTSUP);
}
