/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#if defined(__linux__)
#include <linux/falloc.h>
#include <sys/syscall.h>
#endif
/*
 * __wt_fallocate_config --
 *	Configure file-extension behavior for a file handle.
 */
void
__wt_fallocate_config(WT_SESSION_IMPL *session, WT_FH *fh)
{
	WT_UNUSED(session);

	fh->fallocate_available = WT_FALLOCATE_NOT_AVAILABLE;
	fh->fallocate_requires_locking = 0;

	/*
	 * Check for the availability of some form of fallocate; in all cases,
	 * start off requiring locking, we'll relax that requirement once we
	 * know which system calls work with the handle's underlying filesystem.
	 */
#if defined(HAVE_FALLOCATE) || defined(HAVE_POSIX_FALLOCATE)
	fh->fallocate_available = WT_FALLOCATE_AVAILABLE;
	fh->fallocate_requires_locking = 1;
#endif
#if defined(__linux__) && defined(SYS_fallocate)
	fh->fallocate_available = WT_FALLOCATE_AVAILABLE;
	fh->fallocate_requires_locking = 1;
#endif
}

/*
 * __wt_std_fallocate --
 *	Linux fallocate call.
 */
static int
__wt_std_fallocate(WT_FH *fh, wt_off_t offset, wt_off_t len)
{
#if defined(HAVE_FALLOCATE)
	WT_DECL_RET;

	WT_SYSCALL_RETRY(
	    fallocate(fh->fd, FALLOC_FL_KEEP_SIZE, offset, len), ret);
	return (ret);
#else
	WT_UNUSED(fh);
	WT_UNUSED(offset);
	WT_UNUSED(len);
	return (ENOTSUP);
#endif
}

/*
 * __wt_sys_fallocate --
 *	Linux fallocate call (system call version).
 */
static int
__wt_sys_fallocate(WT_FH *fh, wt_off_t offset, wt_off_t len)
{
#if defined(__linux__) && defined(SYS_fallocate)
	WT_DECL_RET;

	/*
	 * Try the system call for fallocate even if the C library wrapper was
	 * not found.  The system call actually exists in the kernel for some
	 * Linux versions (RHEL 5.5), but not in the version of the C library.
	 * This allows it to work everywhere the kernel supports it.
	 */
	WT_SYSCALL_RETRY(syscall(
	    SYS_fallocate, fh->fd, FALLOC_FL_KEEP_SIZE, offset, len), ret);
	return (ret);
#else
	WT_UNUSED(fh);
	WT_UNUSED(offset);
	WT_UNUSED(len);
	return (ENOTSUP);
#endif
}

/*
 * __wt_posix_fallocate --
 *	POSIX fallocate call.
 */
static int
__wt_posix_fallocate(WT_FH *fh, wt_off_t offset, wt_off_t len)
{
#if defined(HAVE_POSIX_FALLOCATE)
	WT_DECL_RET;

	WT_SYSCALL_RETRY(posix_fallocate(fh->fd, offset, len), ret);
	return (ret);
#else
	WT_UNUSED(fh);
	WT_UNUSED(offset);
	WT_UNUSED(len);
	return (ENOTSUP);
#endif
}

/*
 * __wt_fallocate --
 *	Extend a file.
 */
int
__wt_fallocate(
    WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t offset, wt_off_t len)
{
	WT_DECL_RET;

	switch (fh->fallocate_available) {
	/*
	 * Check for already configured handles and make the configured call.
	 */
	case WT_FALLOCATE_POSIX:
		WT_RET(__wt_verbose(
		    session, WT_VERB_FILEOPS, "%s: posix_fallocate", fh->name));
		if ((ret = __wt_posix_fallocate(fh, offset, len)) == 0)
			return (0);
		WT_RET_MSG(session, ret, "%s: posix_fallocate", fh->name);
	case WT_FALLOCATE_STD:
		WT_RET(__wt_verbose(
		    session, WT_VERB_FILEOPS, "%s: fallocate", fh->name));
		if ((ret = __wt_std_fallocate(fh, offset, len)) == 0)
			return (0);
		WT_RET_MSG(session, ret, "%s: fallocate", fh->name);
	case WT_FALLOCATE_SYS:
		WT_RET(__wt_verbose(
		    session, WT_VERB_FILEOPS, "%s: sys_fallocate", fh->name));
		if ((ret = __wt_sys_fallocate(fh, offset, len)) == 0)
			return (0);
		WT_RET_MSG(session, ret, "%s: sys_fallocate", fh->name);

	/*
	 * Figure out what allocation call this system/filesystem supports, if
	 * any.
	 */
	case WT_FALLOCATE_AVAILABLE:
		/*
		 * We've seen Linux systems where posix_fallocate has corrupted
		 * existing file data (even though that is explicitly disallowed
		 * by POSIX). FreeBSD and Solaris support posix_fallocate, and
		 * so far we've seen no problems leaving it unlocked. Check for
		 * fallocate (and the system call version of fallocate) first to
		 * avoid locking on Linux if at all possible.
		 */
		if ((ret = __wt_std_fallocate(fh, offset, len)) == 0) {
			fh->fallocate_available = WT_FALLOCATE_STD;
			fh->fallocate_requires_locking = 0;
			return (0);
		}
		if ((ret = __wt_sys_fallocate(fh, offset, len)) == 0) {
			fh->fallocate_available = WT_FALLOCATE_SYS;
			fh->fallocate_requires_locking = 0;
			return (0);
		}
		if ((ret = __wt_posix_fallocate(fh, offset, len)) == 0) {
			fh->fallocate_available = WT_FALLOCATE_POSIX;
#if !defined(__linux__)
			fh->fallocate_requires_locking = 0;
#endif
			return (0);
		}
		/* FALLTHROUGH */
	case WT_FALLOCATE_NOT_AVAILABLE:
	default:
		fh->fallocate_available = WT_FALLOCATE_NOT_AVAILABLE;
		return (ENOTSUP);
	}
	/* NOTREACHED */
}
