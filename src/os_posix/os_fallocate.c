/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
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
 * __posix_std_fallocate --
 *	Linux fallocate call.
 */
static int
__posix_std_fallocate(WT_FILE_HANDLE *file_handle,
    WT_SESSION *wt_session,  wt_off_t offset, wt_off_t len)
{
#if defined(HAVE_FALLOCATE)
	WT_DECL_RET;
	WT_FILE_HANDLE_POSIX *pfh;

	WT_UNUSED(wt_session);

	pfh = (WT_FILE_HANDLE_POSIX *)file_handle;

	WT_SYSCALL_RETRY(fallocate(pfh->fd, 0, offset, len), ret);
	return (ret);
#else
	WT_UNUSED(file_handle);
	WT_UNUSED(wt_session);
	WT_UNUSED(offset);
	WT_UNUSED(len);
	return (ENOTSUP);
#endif
}

/*
 * __posix_sys_fallocate --
 *	Linux fallocate call (system call version).
 */
static int
__posix_sys_fallocate(WT_FILE_HANDLE *file_handle,
    WT_SESSION *wt_session, wt_off_t offset, wt_off_t len)
{
#if defined(__linux__) && defined(SYS_fallocate)
	WT_DECL_RET;
	WT_FILE_HANDLE_POSIX *pfh;

	WT_UNUSED(wt_session);

	pfh = (WT_FILE_HANDLE_POSIX *)file_handle;

	/*
	 * Try the system call for fallocate even if the C library wrapper was
	 * not found.  The system call actually exists in the kernel for some
	 * Linux versions (RHEL 5.5), but not in the version of the C library.
	 * This allows it to work everywhere the kernel supports it.
	 */
	WT_SYSCALL_RETRY(syscall(SYS_fallocate, pfh->fd, 0, offset, len), ret);
	return (ret);
#else
	WT_UNUSED(file_handle);
	WT_UNUSED(wt_session);
	WT_UNUSED(offset);
	WT_UNUSED(len);
	return (ENOTSUP);
#endif
}

/*
 * __posix_posix_fallocate --
 *	POSIX fallocate call.
 */
static int
__posix_posix_fallocate(WT_FILE_HANDLE *file_handle,
    WT_SESSION *wt_session,  wt_off_t offset, wt_off_t len)
{
#if defined(HAVE_POSIX_FALLOCATE)
	WT_DECL_RET;
	WT_FILE_HANDLE_POSIX *pfh;

	WT_UNUSED(wt_session);

	pfh = (WT_FILE_HANDLE_POSIX *)file_handle;

	WT_SYSCALL_RETRY(posix_fallocate(pfh->fd, offset, len), ret);
	return (ret);
#else
	WT_UNUSED(file_handle);
	WT_UNUSED(wt_session);
	WT_UNUSED(offset);
	WT_UNUSED(len);
	return (ENOTSUP);
#endif
}

/*
 * __wt_posix_file_fallocate --
 *	POSIX fallocate.
 */
int
__wt_posix_file_fallocate(WT_FILE_HANDLE *file_handle,
    WT_SESSION *wt_session, wt_off_t offset, wt_off_t len)
{
	/*
	 * The first fallocate call: figure out what fallocate call this system
	 * supports, if any.
	 *
	 * The function is configured as a locking fallocate call, so we know
	 * we're single-threaded through here. Set the nolock function first,
	 * then publish the NULL replacement to ensure the handle functions are
	 * always correct.
	 *
	 * We've seen Linux systems where posix_fallocate has corrupted
	 * existing file data (even though that is explicitly disallowed
	 * by POSIX). FreeBSD and Solaris support posix_fallocate, and
	 * so far we've seen no problems leaving it unlocked. Check for
	 * fallocate (and the system call version of fallocate) first to
	 * avoid locking on Linux if at all possible.
	 */
	if (__posix_std_fallocate(file_handle, wt_session, offset, len) == 0) {
		file_handle->fh_allocate_nolock = __posix_std_fallocate;
		WT_PUBLISH(file_handle->fh_allocate, NULL);
		return (0);
	}
	if (__posix_sys_fallocate(file_handle, wt_session, offset, len) == 0) {
		file_handle->fh_allocate_nolock = __posix_sys_fallocate;
		WT_PUBLISH(file_handle->fh_allocate, NULL);
		return (0);
	}
	if (__posix_posix_fallocate(
	    file_handle, wt_session, offset, len) == 0) {
#if defined(__linux__)
		file_handle->fh_allocate = __posix_posix_fallocate;
		WT_WRITE_BARRIER();
#else
		file_handle->fh_allocate_nolock = __posix_posix_fallocate;
		WT_PUBLISH(file_handle->fh_allocate, NULL);
#endif
		return (0);
	}

	file_handle->fh_allocate = NULL;
	WT_WRITE_BARRIER();
	return (ENOTSUP);
}
