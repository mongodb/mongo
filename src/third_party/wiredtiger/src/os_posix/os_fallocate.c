/*-
 * Copyright (c) 2014-present MongoDB, Inc.
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
 * WT_CALL_FUNCTION --
 *	Call the underlying fallocate function, wrapped in a macro so there's only a single copy of
 * the mmap support code.
 */
#if defined(HAVE_FALLOCATE) || (defined(__linux__) && defined(SYS_fallocate)) || \
  defined(HAVE_POSIX_FALLOCATE)
#define WT_CALL_FUNCTION(op)                                                            \
    do {                                                                                \
        WT_DECL_RET;                                                                    \
        WT_FILE_HANDLE_POSIX *pfh;                                                      \
        bool remap;                                                                     \
                                                                                        \
        pfh = (WT_FILE_HANDLE_POSIX *)file_handle;                                      \
                                                                                        \
        /* Always call prepare. It will return whether a remap is needed or not. */     \
        __wti_posix_prepare_remap_resize_file(file_handle, wt_session, offset, &remap); \
                                                                                        \
        WT_SYSCALL_RETRY(op, ret);                                                      \
        if (remap) {                                                                    \
            if (ret == 0)                                                               \
                __wti_posix_remap_resize_file(file_handle, wt_session);                 \
            else {                                                                      \
                __wti_posix_release_without_remap(file_handle);                         \
                WT_RET(ret);                                                            \
            }                                                                           \
        }                                                                               \
        return (ret);                                                                   \
    } while (0)
#endif

/*
 * __posix_std_fallocate --
 *     Linux fallocate call.
 */
static int
__posix_std_fallocate(WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session, wt_off_t offset)
{
#if defined(HAVE_FALLOCATE)
    WT_CALL_FUNCTION(fallocate(pfh->fd, 0, (wt_off_t)0, offset));
#else
    WT_UNUSED(file_handle);
    WT_UNUSED(offset);

    return (__wt_set_return((WT_SESSION_IMPL *)wt_session, ENOTSUP));
#endif
}

/*
 * __posix_sys_fallocate --
 *     Linux fallocate call (system call version).
 */
static int
__posix_sys_fallocate(WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session, wt_off_t offset)
{
#if defined(__linux__) && defined(SYS_fallocate)
    WT_CALL_FUNCTION(syscall(SYS_fallocate, pfh->fd, 0, (wt_off_t)0, offset));
#else
    WT_UNUSED(file_handle);
    WT_UNUSED(offset);

    return (__wt_set_return((WT_SESSION_IMPL *)wt_session, ENOTSUP));
#endif
}

/*
 * __posix_posix_fallocate --
 *     POSIX fallocate call.
 */
static int
__posix_posix_fallocate(WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session, wt_off_t offset)
{
#if defined(HAVE_POSIX_FALLOCATE)
    WT_CALL_FUNCTION(posix_fallocate(pfh->fd, (wt_off_t)0, offset));
#else
    WT_UNUSED(file_handle);
    WT_UNUSED(offset);

    return (__wt_set_return((WT_SESSION_IMPL *)wt_session, ENOTSUP));
#endif
}

/*
 * __wti_posix_file_extend --
 *     Extend the file.
 */
int
__wti_posix_file_extend(WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session, wt_off_t offset)
{
    /*
     * The first file extension call: figure out what this system has.
     *
     * This function is configured as a locking call, so we know we're single-threaded through here.
     * Set the nolock function first, then release write the NULL replacement to ensure the handle
     * functions are always correct.
     *
     * We've seen Linux systems where posix_fallocate has corrupted existing file data (even though
     * that is explicitly disallowed by POSIX). FreeBSD and Solaris support posix_fallocate, and so
     * far we've seen no problems leaving it unlocked. Check for fallocate (and the system call
     * version of fallocate) first to avoid locking on Linux if at all possible.
     */
    if (__posix_std_fallocate(file_handle, wt_session, offset) == 0) {
        file_handle->fh_extend_nolock = __posix_std_fallocate;
        WT_RELEASE_WRITE_WITH_BARRIER(file_handle->fh_extend, NULL);
        return (0);
    }
    if (__posix_sys_fallocate(file_handle, wt_session, offset) == 0) {
        file_handle->fh_extend_nolock = __posix_sys_fallocate;
        WT_RELEASE_WRITE_WITH_BARRIER(file_handle->fh_extend, NULL);
        return (0);
    }
    if (__posix_posix_fallocate(file_handle, wt_session, offset) == 0) {
#if defined(__linux__)
        file_handle->fh_extend = __posix_posix_fallocate;
        WT_RELEASE_BARRIER();
#else
        file_handle->fh_extend_nolock = __posix_posix_fallocate;
        WT_RELEASE_WRITE_WITH_BARRIER(file_handle->fh_extend, NULL);
#endif
        return (0);
    }

    /*
     * Use the POSIX ftruncate call if there's nothing else, it can extend files. Note ftruncate
     * requires locking.
     */
    if (file_handle->fh_truncate != NULL &&
      file_handle->fh_truncate(file_handle, wt_session, offset) == 0) {
        file_handle->fh_extend = file_handle->fh_truncate;
        WT_RELEASE_BARRIER();
        return (0);
    }

    file_handle->fh_extend = NULL;
    WT_RELEASE_BARRIER();
    return (ENOTSUP);
}
