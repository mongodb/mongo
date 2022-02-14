/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "wt_internal.h"

/*
 * __posix_sync --
 *     Underlying support function to flush a file descriptor. Fsync calls (or fsync-style calls,
 *     for example, fdatasync) are not retried on failure, and failure halts the system. Excerpted
 *     from the LWN.net article https://lwn.net/Articles/752063/: In short, PostgreSQL assumes that
 *     a successful call to fsync() indicates that all data written since the last successful call
 *     made it safely to persistent storage. But that is not what the kernel actually does. When a
 *     buffered I/O write fails due to a hardware-level error, filesystems will respond differently,
 *     but that behavior usually includes discarding the data in the affected pages and marking them
 *     as being clean. So a read of the blocks that were just written will likely return something
 *     other than the data that was written. Given the shared history of UNIX filesystems, and the
 *     difficulty of knowing what specific error will be returned under specific circumstances, we
 *     don't retry fsync-style calls and panic if a flush operation fails.
 */
static int
__posix_sync(WT_SESSION_IMPL *session, int fd, const char *name, const char *func)
{
    WT_DECL_RET;

#if defined(F_FULLFSYNC)
    /*
     * OS X fsync documentation: "Note that while fsync() will flush all data from the host to the
     * drive (i.e. the "permanent storage device"), the drive itself may not physically write the
     * data to the platters for quite some time and it may be written in an out-of-order sequence.
     * For applications that require tighter guarantees about the integrity of their data, Mac OS X
     * provides the F_FULLFSYNC fcntl. The F_FULLFSYNC fcntl asks the drive to flush all buffered
     * data to permanent storage."
     *
     * OS X F_FULLFSYNC fcntl documentation: "This is currently implemented on HFS, MS-DOS (FAT),
     * and Universal Disk Format (UDF) file systems."
     *
     * See comment in __posix_sync(): sync cannot be retried or fail.
     */
    static enum { FF_NOTSET, FF_IGNORE, FF_OK } ff_status = FF_NOTSET;
    switch (ff_status) {
    case FF_NOTSET:
        WT_SYSCALL(fcntl(fd, F_FULLFSYNC, 0) == -1 ? -1 : 0, ret);
        if (ret == 0) {
            ff_status = FF_OK;
            return (0);
        }

        /*
         * If the first F_FULLFSYNC fails, assume the file system doesn't support it and fallback to
         * fdatasync or fsync.
         */
        ff_status = FF_IGNORE;
        __wt_err(session, ret, "fcntl(F_FULLFSYNC) failed, falling back to fdatasync or fsync");
        break;
    case FF_IGNORE:
        break;
    case FF_OK:
        WT_SYSCALL(fcntl(fd, F_FULLFSYNC, 0) == -1 ? -1 : 0, ret);
        if (ret == 0)
            return (0);
        WT_RET_PANIC(session, ret, "%s: %s: fcntl(F_FULLFSYNC)", name, func);
    }
#endif
#if defined(HAVE_FDATASYNC)
    /* See comment in __posix_sync(): sync cannot be retried or fail. */
    WT_SYSCALL(fdatasync(fd), ret);
    if (ret == 0)
        return (0);
    WT_RET_PANIC(session, ret, "%s: %s: fdatasync", name, func);
#else
    /* See comment in __posix_sync(): sync cannot be retried or fail. */
    WT_SYSCALL(fsync(fd), ret);
    if (ret == 0)
        return (0);
    WT_RET_PANIC(session, ret, "%s: %s: fsync", name, func);
#endif
}

#ifdef __linux__
/*
 * __posix_directory_sync --
 *     Flush a directory to ensure file creation, remove or rename is durable.
 */
static int
__posix_directory_sync(WT_SESSION_IMPL *session, const char *path)
{
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    int fd, tret;
    char *dir;

    WT_RET(__wt_scr_alloc(session, 0, &tmp));
    WT_ERR(__wt_buf_setstr(session, tmp, path));

    /*
     * This layer should never see a path that doesn't include a trailing path separator, this code
     * asserts that fact.
     */
    dir = tmp->mem;
    strrchr(dir, '/')[1] = '\0';

    fd = 0; /* -Wconditional-uninitialized */
    WT_SYSCALL_RETRY(((fd = open(dir, O_RDONLY | O_CLOEXEC, 0444)) == -1 ? -1 : 0), ret);
    if (ret != 0)
        WT_ERR_MSG(session, ret, "%s: directory-sync: open", dir);

    ret = __posix_sync(session, fd, dir, "directory-sync");

    WT_SYSCALL(close(fd), tret);
    if (tret != 0) {
        __wt_err(session, tret, "%s: directory-sync: close", dir);
        WT_TRET(tret);
    }

err:
    __wt_scr_free(session, &tmp);
    if (ret == 0)
        return (ret);

    /* See comment in __posix_sync(): sync cannot be retried or fail. */
    WT_RET_PANIC(session, ret, "%s: directory-sync", path);
}
#endif

/*
 * __posix_fs_exist --
 *     Return if the file exists.
 */
static int
__posix_fs_exist(
  WT_FILE_SYSTEM *file_system, WT_SESSION *wt_session, const char *name, bool *existp)
{
    struct stat sb;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    WT_UNUSED(file_system);

    session = (WT_SESSION_IMPL *)wt_session;

    WT_SYSCALL(stat(name, &sb), ret);
    if (ret == 0) {
        *existp = true;
        return (0);
    }
    if (ret == ENOENT) {
        *existp = false;
        return (0);
    }
    WT_RET_MSG(session, ret, "%s: file-exist: stat", name);
}

/*
 * __posix_fs_remove --
 *     Remove a file.
 */
static int
__posix_fs_remove(
  WT_FILE_SYSTEM *file_system, WT_SESSION *wt_session, const char *name, uint32_t flags)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    WT_UNUSED(file_system);

    session = (WT_SESSION_IMPL *)wt_session;

    /*
     * ISO C doesn't require remove return -1 on failure or set errno (note POSIX 1003.1 extends C
     * with those requirements). Regardless, use the unlink system call, instead of remove, to
     * simplify error handling; where we're not doing any special checking for standards compliance,
     * using unlink may be marginally safer.
     */
    WT_SYSCALL(unlink(name), ret);
    if (ret != 0)
        WT_RET_MSG(session, ret, "%s: file-remove: unlink", name);

    if (!LF_ISSET(WT_FS_DURABLE))
        return (0);

#ifdef __linux__
    /* Flush the backing directory to guarantee the remove. */
    WT_RET(__wt_log_printf(session, "REMOVE: posix_directory_sync %s", name));
    WT_RET(__posix_directory_sync(session, name));
    WT_RET(__wt_log_printf(session, "REMOVE: DONE posix_directory_sync %s", name));
#endif
    return (0);
}

/*
 * __posix_fs_rename --
 *     Rename a file.
 */
static int
__posix_fs_rename(WT_FILE_SYSTEM *file_system, WT_SESSION *wt_session, const char *from,
  const char *to, uint32_t flags)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    WT_UNUSED(file_system);

    session = (WT_SESSION_IMPL *)wt_session;

    /*
     * ISO C doesn't require rename return -1 on failure or set errno (note POSIX 1003.1 extends C
     * with those requirements). Be cautious, force any non-zero return to -1 so we'll check errno.
     * We can still end up with the wrong errno (if errno is garbage), or the generic WT_ERROR
     * return (if errno is 0), but we've done the best we can.
     */
    WT_SYSCALL(rename(from, to) != 0 ? -1 : 0, ret);
    if (ret != 0)
        WT_RET_MSG(session, ret, "%s to %s: file-rename: rename", from, to);

    if (!LF_ISSET(WT_FS_DURABLE))
        return (0);
#ifdef __linux__
    /*
     * Flush the backing directory to guarantee the rename. My reading of POSIX 1003.1 is there's no
     * guarantee flushing only one of the from or to directories, or flushing a common parent, is
     * sufficient, and even if POSIX were to make that guarantee, existing filesystems are known to
     * not provide the guarantee or only provide the guarantee with specific mount options. Flush
     * both of the from/to directories until it's a performance problem.
     */
    WT_RET(__wt_log_printf(session, "RENAME: posix_directory_sync %s", from));
    WT_RET(__posix_directory_sync(session, from));
    WT_RET(__wt_log_printf(session, "RENAME: DONE posix_directory_sync %s", from));

    /*
     * In almost all cases, we're going to be renaming files in the same directory, we can at least
     * fast-path that.
     */
    {
        bool same_directory;
        const char *fp, *tp;

        fp = strrchr(from, '/');
        tp = strrchr(to, '/');
        same_directory = (fp == NULL && tp == NULL) ||
          (fp != NULL && tp != NULL && fp - from == tp - to &&
            memcmp(from, to, (size_t)(fp - from)) == 0);

        if (!same_directory)
            WT_RET(__posix_directory_sync(session, to));
    }
#endif
    return (0);
}

/*
 * __posix_fs_size --
 *     Get the size of a file in bytes, by file name.
 */
static int
__posix_fs_size(
  WT_FILE_SYSTEM *file_system, WT_SESSION *wt_session, const char *name, wt_off_t *sizep)
{
    struct stat sb;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    WT_UNUSED(file_system);

    session = (WT_SESSION_IMPL *)wt_session;

    WT_SYSCALL(stat(name, &sb), ret);
    if (ret == 0) {
        *sizep = sb.st_size;
        return (0);
    }
    WT_RET_MSG(session, ret, "%s: file-size: stat", name);
}

#if defined(HAVE_POSIX_FADVISE)
/*
 * __posix_file_advise --
 *     POSIX fadvise.
 */
static int
__posix_file_advise(
  WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session, wt_off_t offset, wt_off_t len, int advice)
{
    WT_DECL_RET;
    WT_FILE_HANDLE_POSIX *pfh;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;
    pfh = (WT_FILE_HANDLE_POSIX *)file_handle;

    WT_SYSCALL(posix_fadvise(pfh->fd, offset, len, advice), ret);
    if (ret == 0)
        return (0);

    /*
     * Treat EINVAL as not-supported, some systems don't support some flags. Quietly fail, callers
     * expect not-supported failures, and reset the handle method to prevent future calls.
     */
    if (ret == EINVAL) {
        file_handle->fh_advise = NULL;
        return (__wt_set_return(session, ENOTSUP));
    }

    WT_RET_MSG(session, ret, "%s: handle-advise: posix_fadvise", file_handle->name);
}
#endif

/*
 * __posix_file_close --
 *     ANSI C close.
 */
static int
__posix_file_close(WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session)
{
    WT_DECL_RET;
    WT_FILE_HANDLE_POSIX *pfh;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;
    pfh = (WT_FILE_HANDLE_POSIX *)file_handle;

    __wt_verbose(session, WT_VERB_FILEOPS, "%s, file-close: fd=%d", file_handle->name, pfh->fd);

    if (pfh->mmap_file_mappable && pfh->mmap_buf != NULL)
        __wt_unmap_file(file_handle, wt_session);

    /* Close the file handle. */
    if (pfh->fd != -1) {
        WT_SYSCALL(close(pfh->fd), ret);
        if (ret != 0)
            __wt_err(session, ret, "%s: handle-close: close", file_handle->name);
    }

    __wt_free(session, file_handle->name);
    __wt_free(session, pfh);
    return (ret);
}

/*
 * __posix_file_lock --
 *     Lock/unlock a file.
 */
static int
__posix_file_lock(WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session, bool lock)
{
    struct flock fl;
    WT_DECL_RET;
    WT_FILE_HANDLE_POSIX *pfh;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;
    pfh = (WT_FILE_HANDLE_POSIX *)file_handle;

    /*
     * WiredTiger requires this function be able to acquire locks past the end of file.
     *
     * Note we're using fcntl(2) locking: all fcntl locks associated with a file for a given process
     * are removed when any file descriptor for the file is closed by the process, even if a lock
     * was never requested for that file descriptor.
     */
    fl.l_start = 0;
    fl.l_len = 1;
    fl.l_type = lock ? F_WRLCK : F_UNLCK;
    fl.l_whence = SEEK_SET;

    WT_SYSCALL(fcntl(pfh->fd, F_SETLK, &fl) == -1 ? -1 : 0, ret);
    if (ret == 0)
        return (0);
    WT_RET_MSG(session, ret, "%s: handle-lock: fcntl", file_handle->name);
}

/*
 * __posix_file_read --
 *     POSIX pread.
 */
static int
__posix_file_read(
  WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session, wt_off_t offset, size_t len, void *buf)
{
    WT_FILE_HANDLE_POSIX *pfh;
    WT_SESSION_IMPL *session;
    size_t chunk;
    ssize_t nr;
    uint8_t *addr;

    session = (WT_SESSION_IMPL *)wt_session;
    pfh = (WT_FILE_HANDLE_POSIX *)file_handle;

    __wt_verbose(session, WT_VERB_READ, "read: %s, fd=%d, offset=%" PRId64 ", len=%" WT_SIZET_FMT,
      file_handle->name, pfh->fd, offset, len);

    /* Assert direct I/O is aligned and a multiple of the alignment. */
    WT_ASSERT(session,
      !pfh->direct_io || S2C(session)->buffer_alignment == 0 ||
        (!((uintptr_t)buf & (uintptr_t)(S2C(session)->buffer_alignment - 1)) &&
          len >= S2C(session)->buffer_alignment && len % S2C(session)->buffer_alignment == 0));

    /* Break reads larger than 1GB into 1GB chunks. */
    for (addr = buf; len > 0; addr += nr, len -= (size_t)nr, offset += nr) {
        chunk = WT_MIN(len, WT_GIGABYTE);
        if ((nr = pread(pfh->fd, addr, chunk, offset)) <= 0)
            WT_RET_MSG(session, nr == 0 ? WT_ERROR : __wt_errno(),
              "%s: handle-read: pread: failed to read %" WT_SIZET_FMT " bytes at offset %" PRIuMAX,
              file_handle->name, chunk, (uintmax_t)offset);
    }
    WT_STAT_CONN_INCRV(session, block_byte_read_syscall, len);
    return (0);
}

/*
 * __posix_file_read_mmap --
 *     Get the buffer from the mapped region.
 */
static int
__posix_file_read_mmap(
  WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session, wt_off_t offset, size_t len, void *buf)
{
    WT_FILE_HANDLE_POSIX *pfh;
    WT_SESSION_IMPL *session;
    bool mmap_success;

    session = (WT_SESSION_IMPL *)wt_session;
    pfh = (WT_FILE_HANDLE_POSIX *)file_handle;

    if (!pfh->mmap_file_mappable || pfh->mmap_resizing)
        return (__posix_file_read(file_handle, wt_session, offset, len, buf));

    __wt_verbose(session, WT_VERB_READ,
      "read-mmap: %s, fd=%d, offset=%" PRId64 ", len=%" WT_SIZET_FMT
      ", mapped buffer: %p, mapped size = %" PRId64,
      file_handle->name, pfh->fd, offset, len, (void *)pfh->mmap_buf, pfh->mmap_size);

    /* Indicate that we might be using the mapped area */
    (void)__wt_atomic_addv32(&pfh->mmap_usecount, 1);

    /*
     * If the I/O falls inside the mapped buffer, and the buffer is not being re-sized, we will use
     * the mapped buffer.
     */
    mmap_success = false;
    if (pfh->mmap_buf != NULL && pfh->mmap_size >= offset + (wt_off_t)len && !pfh->mmap_resizing) {
        memcpy(buf, (void *)(pfh->mmap_buf + offset), len);
        mmap_success = true;
        WT_STAT_CONN_INCRV(session, block_byte_read_mmap, len);
    }

    /* Signal that we are done using the mapped buffer. */
    (void)__wt_atomic_subv32(&pfh->mmap_usecount, 1);

    if (mmap_success)
        return (0);

    /* We couldn't use mmap for some reason, so use the system call. */
    return (__posix_file_read(file_handle, wt_session, offset, len, buf));
}

/*
 * __posix_file_size --
 *     Get the size of a file in bytes, by file handle.
 */
static int
__posix_file_size(WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session, wt_off_t *sizep)
{
    struct stat sb;
    WT_DECL_RET;
    WT_FILE_HANDLE_POSIX *pfh;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;
    pfh = (WT_FILE_HANDLE_POSIX *)file_handle;

    WT_SYSCALL(fstat(pfh->fd, &sb), ret);
    if (ret == 0) {
        *sizep = sb.st_size;
        return (0);
    }
    WT_RET_MSG(session, ret, "%s: handle-size: fstat", file_handle->name);
}

/*
 * __posix_file_sync --
 *     POSIX fsync.
 */
static int
__posix_file_sync(WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session)
{
    WT_FILE_HANDLE_POSIX *pfh;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;
    pfh = (WT_FILE_HANDLE_POSIX *)file_handle;

    return (__posix_sync(session, pfh->fd, file_handle->name, "handle-sync"));
}

#ifdef HAVE_SYNC_FILE_RANGE
/*
 * __posix_file_sync_nowait --
 *     POSIX fsync.
 */
static int
__posix_file_sync_nowait(WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session)
{
    WT_DECL_RET;
    WT_FILE_HANDLE_POSIX *pfh;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;
    pfh = (WT_FILE_HANDLE_POSIX *)file_handle;

    /* See comment in __posix_sync(): sync cannot be retried or fail. */
    WT_SYSCALL(sync_file_range(pfh->fd, (off64_t)0, (off64_t)0, SYNC_FILE_RANGE_WRITE), ret);
    if (ret == 0)
        return (0);

    WT_RET_PANIC(session, ret, "%s: handle-sync-nowait: sync_file_range", file_handle->name);
}
#endif

#ifdef HAVE_FTRUNCATE
/*
 * __posix_file_truncate --
 *     POSIX ftruncate.
 */
static int
__posix_file_truncate(WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session, wt_off_t len)
{
    WT_DECL_RET;
    WT_FILE_HANDLE_POSIX *pfh;
    WT_SESSION_IMPL *session;
    bool remap;

    session = (WT_SESSION_IMPL *)wt_session;
    pfh = (WT_FILE_HANDLE_POSIX *)file_handle;

    __wt_verbose(session, WT_VERB_FILEOPS,
      "%s, file-truncate: size=%" PRId64 ", mapped size=%" PRId64, file_handle->name, len,
      pfh->mmap_size);

    remap = (len != pfh->mmap_size);
    if (remap)
        __wt_prepare_remap_resize_file(file_handle, wt_session);

    WT_SYSCALL_RETRY(ftruncate(pfh->fd, len), ret);
    if (remap) {
        if (ret == 0)
            __wt_remap_resize_file(file_handle, wt_session);
        else {
            __wt_release_without_remap(file_handle);
            WT_RET_MSG(session, ret, "%s: handle-truncate: ftruncate", file_handle->name);
        }
    }
    return (0);
}
#endif

/*
 * __posix_file_write --
 *     POSIX pwrite.
 */
static int
__posix_file_write(
  WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session, wt_off_t offset, size_t len, const void *buf)
{
    WT_FILE_HANDLE_POSIX *pfh;
    WT_SESSION_IMPL *session;
    size_t chunk;
    ssize_t nw;
    const uint8_t *addr;

    session = (WT_SESSION_IMPL *)wt_session;
    pfh = (WT_FILE_HANDLE_POSIX *)file_handle;

    __wt_verbose(session, WT_VERB_WRITE, "write: %s, fd=%d, offset=%" PRId64 ", len=%" WT_SIZET_FMT,
      file_handle->name, pfh->fd, offset, len);

    /* Assert direct I/O is aligned and a multiple of the alignment. */
    WT_ASSERT(session,
      !pfh->direct_io || S2C(session)->buffer_alignment == 0 ||
        (!((uintptr_t)buf & (uintptr_t)(S2C(session)->buffer_alignment - 1)) &&
          len >= S2C(session)->buffer_alignment && len % S2C(session)->buffer_alignment == 0));

    /* Break writes larger than 1GB into 1GB chunks. */
    for (addr = buf; len > 0; addr += nw, len -= (size_t)nw, offset += nw) {
        chunk = WT_MIN(len, WT_GIGABYTE);
        if ((nw = pwrite(pfh->fd, addr, chunk, offset)) < 0)
            WT_RET_MSG(session, __wt_errno(),
              "%s: handle-write: pwrite: failed to write %" WT_SIZET_FMT
              " bytes at offset %" PRIuMAX,
              file_handle->name, chunk, (uintmax_t)offset);
        if (session->debug_8392 != NULL)
            WT_RET(__wt_msg(session, "pwrite: %s: %s: wrote at offset %" PRIu64 " len %" PRIu64,
              file_handle->name, session->debug_8392, (uint64_t)offset, (uint64_t)len));
    }
    WT_STAT_CONN_INCRV(session, block_byte_write_syscall, len);
    return (0);
}

/*
 * __posix_file_write_mmap --
 *     Write the buffer into the mapped region.
 */
static int
__posix_file_write_mmap(
  WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session, wt_off_t offset, size_t len, const void *buf)
{
    static int remap_opportunities;
    WT_FILE_HANDLE_POSIX *pfh;
    WT_SESSION_IMPL *session;
    bool mmap_success;

    session = (WT_SESSION_IMPL *)wt_session;
    pfh = (WT_FILE_HANDLE_POSIX *)file_handle;

    __wt_verbose(session, WT_VERB_WRITE,
      "write-mmap: %s, fd=%d, offset=%" PRId64 ", len=%" WT_SIZET_FMT
      ", mapped buffer: %p, mapped size = %" PRId64,
      file_handle->name, pfh->fd, offset, len, (void *)pfh->mmap_buf, pfh->mmap_size);

    if (!pfh->mmap_file_mappable || pfh->mmap_resizing)
        return (__posix_file_write(file_handle, wt_session, offset, len, buf));

    /* Indicate that we might be using the mapped area */
    (void)__wt_atomic_addv32(&pfh->mmap_usecount, 1);

    /*
     * If the I/O falls inside the mapped buffer, and the buffer is not being re-sized, we will use
     * the mapped buffer.
     */
    mmap_success = false;
    if (pfh->mmap_buf != NULL && pfh->mmap_size >= offset + (wt_off_t)len && !pfh->mmap_resizing) {
        memcpy((void *)(pfh->mmap_buf + offset), buf, len);
        mmap_success = true;
        WT_STAT_CONN_INCRV(session, block_byte_write_mmap, len);
    }

    /* Signal that we are done using the mapped buffer. */
    (void)__wt_atomic_subv32(&pfh->mmap_usecount, 1);

    if (mmap_success)
        return (0);

    /* We couldn't use mmap for some reason, so use the system call. */
    WT_RET(__posix_file_write(file_handle, wt_session, offset, len, buf));

/*
 * If we wrote the file via a system call, we might have extended its size. If the file is mapped,
 * remap it with the new size. If we are actively extending the file, don't remap it on every write
 * to avoid overhead.
 */
#define WT_REMAP_SKIP 10
    if (pfh->mmap_file_mappable && !pfh->mmap_resizing && pfh->mmap_size < offset + (wt_off_t)len)
        /* If we are actively extending the file, don't remap it on every write. */
        if ((remap_opportunities++) % WT_REMAP_SKIP == 0) {
            __wt_prepare_remap_resize_file(file_handle, wt_session);
            __wt_remap_resize_file(file_handle, wt_session);
            WT_STAT_CONN_INCRV(session, block_remap_file_write, 1);
        }
    return (0);
}

/*
 * __posix_open_file_cloexec --
 *     Prevent child access to file handles.
 */
static inline int
__posix_open_file_cloexec(WT_SESSION_IMPL *session, int fd, const char *name)
{
#if defined(FD_CLOEXEC) && !defined(O_CLOEXEC)
    int f;

    /*
     * Security: The application may spawn a new process, and we don't want another process to have
     * access to our file handles. There's an obvious race between the open and this call, prefer
     * the flag to open if available.
     */
    if ((f = fcntl(fd, F_GETFD)) == -1 || fcntl(fd, F_SETFD, f | FD_CLOEXEC) == -1)
        WT_RET_MSG(session, __wt_errno(), "%s: handle-open: fcntl(FD_CLOEXEC)", name);
    return (0);
#else
    WT_UNUSED(session);
    WT_UNUSED(fd);
    WT_UNUSED(name);
    return (0);
#endif
}

/*
 * __posix_open_file --
 *     Open a file handle.
 */
static int
__posix_open_file(WT_FILE_SYSTEM *file_system, WT_SESSION *wt_session, const char *name,
  WT_FS_OPEN_FILE_TYPE file_type, uint32_t flags, WT_FILE_HANDLE **file_handlep)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_FILE_HANDLE *file_handle;
    WT_FILE_HANDLE_POSIX *pfh;
    WT_SESSION_IMPL *session;
    mode_t mode;
    int advise_flag, f;

    WT_UNUSED(file_system);

    file_handle = NULL;
    *file_handlep = NULL;

    session = (WT_SESSION_IMPL *)wt_session;
    conn = S2C(session);

    WT_RET(__wt_calloc_one(session, &pfh));

    /* Set up error handling. */
    pfh->fd = -1;

    if (file_type == WT_FS_OPEN_FILE_TYPE_DIRECTORY) {
        f = O_RDONLY;
#ifdef O_CLOEXEC
        /*
         * Security: The application may spawn a new process, and we don't want another process to
         * have access to our file handles.
         */
        f |= O_CLOEXEC;
#endif
        WT_SYSCALL_RETRY(((pfh->fd = open(name, f, 0444)) == -1 ? -1 : 0), ret);
        if (ret != 0)
            WT_ERR_MSG(session, ret, "%s: handle-open: open-directory", name);
        WT_ERR(__posix_open_file_cloexec(session, pfh->fd, name));
        goto directory_open;
    }

    f = LF_ISSET(WT_FS_OPEN_READONLY) ? O_RDONLY : O_RDWR;
    if (LF_ISSET(WT_FS_OPEN_CREATE)) {
        f |= O_CREAT;
        if (LF_ISSET(WT_FS_OPEN_EXCLUSIVE))
            f |= O_EXCL;
        mode = 0666;
    } else
        mode = 0;

#ifdef O_BINARY
    /* Windows clones: we always want to treat the file as a binary. */
    f |= O_BINARY;
#endif
#ifdef O_CLOEXEC
    /*
     * Security: The application may spawn a new process, and we don't want another process to have
     * access to our file handles.
     */
    f |= O_CLOEXEC;
#endif
#ifdef O_DIRECT
    /* Direct I/O. */
    if (LF_ISSET(WT_FS_OPEN_DIRECTIO)) {
        f |= O_DIRECT;
        pfh->direct_io = true;
    } else
        pfh->direct_io = false;
#endif
#ifdef O_NOATIME
    /* Avoid updating metadata for read-only workloads. */
    if (file_type == WT_FS_OPEN_FILE_TYPE_DATA)
        f |= O_NOATIME;
#endif

    if (file_type == WT_FS_OPEN_FILE_TYPE_LOG && FLD_ISSET(conn->txn_logsync, WT_LOG_DSYNC)) {
#ifdef O_DSYNC
        f |= O_DSYNC;
#elif defined(O_SYNC)
        f |= O_SYNC;
#else
        WT_ERR_MSG(session, ENOTSUP, "unsupported log sync mode configured");
#endif
    }

    /* Create/Open the file. */
    WT_SYSCALL_RETRY(((pfh->fd = open(name, f, mode)) == -1 ? -1 : 0), ret);
    if (ret != 0)
        WT_ERR_MSG(session, ret,
          pfh->direct_io ? "%s: handle-open: open: failed with direct I/O configured, some "
                           "filesystem types do not support direct I/O" :
                           "%s: handle-open: open",
          name);

#ifdef __linux__
    /*
     * Durability: some filesystems require a directory sync to be confident the file will appear.
     */
    if (LF_ISSET(WT_FS_OPEN_DURABLE)) {
        WT_ERR(__wt_log_printf(session, "OPEN/CREATE: posix_directory_sync %s", name));
        WT_ERR(__posix_directory_sync(session, name));
        WT_ERR(__wt_log_printf(session, "OPEN/CREATE: DONE posix_directory_sync %s", name));
    }
#endif

    WT_ERR(__posix_open_file_cloexec(session, pfh->fd, name));

#if defined(HAVE_POSIX_FADVISE)
    /*
     * If the user set an access pattern hint, call fadvise now. Ignore fadvise when doing direct
     * I/O, the kernel cache isn't interesting.
     */
    if (!pfh->direct_io && file_type == WT_FS_OPEN_FILE_TYPE_DATA &&
      LF_ISSET(WT_FS_OPEN_ACCESS_RAND | WT_FS_OPEN_ACCESS_SEQ)) {
        advise_flag = 0;
        if (LF_ISSET(WT_FS_OPEN_ACCESS_RAND))
            advise_flag = POSIX_FADV_RANDOM;
        if (LF_ISSET(WT_FS_OPEN_ACCESS_SEQ))
            advise_flag = POSIX_FADV_SEQUENTIAL;
        WT_SYSCALL(posix_fadvise(pfh->fd, 0, 0, advise_flag), ret);
        if (ret != 0)
            WT_ERR_MSG(session, ret, "%s: handle-open: posix_fadvise", name);
    }
#else
    WT_UNUSED(advise_flag);
#endif

directory_open:
    /* Initialize public information. */
    file_handle = (WT_FILE_HANDLE *)pfh;
    WT_ERR(__wt_strdup(session, name, &file_handle->name));

    if (conn->mmap_all) {
        /*
         * We are going to use mmap for I/O. So let's mmap the file on opening. If mmap fails, we
         * will just mark the file as not mappable (inside the mapping function) and will use system
         * calls for I/O on this file. We will not crash the database if mmap fails.
         */
        if (file_type == WT_FS_OPEN_FILE_TYPE_DATA || file_type == WT_FS_OPEN_FILE_TYPE_LOG) {
            pfh->mmap_file_mappable = true;
            pfh->mmap_prot = LF_ISSET(WT_FS_OPEN_READONLY) ? PROT_READ : PROT_READ | PROT_WRITE;
            __wt_map_file(file_handle, wt_session);
        }
    }

    file_handle->close = __posix_file_close;
#if defined(HAVE_POSIX_FADVISE)
    /*
     * Ignore fadvise when doing direct I/O, the kernel cache isn't interesting.
     */
    if (!pfh->direct_io)
        file_handle->fh_advise = __posix_file_advise;
#endif
    file_handle->fh_extend = __wt_posix_file_extend;
    file_handle->fh_lock = __posix_file_lock;
#ifdef WORDS_BIGENDIAN
/*
 * The underlying objects are little-endian, mapping objects isn't currently supported on big-endian
 * systems.
 */
#else
    file_handle->fh_map = __wt_posix_map;
#ifdef HAVE_POSIX_MADVISE
    file_handle->fh_map_discard = __wt_posix_map_discard;
    file_handle->fh_map_preload = __wt_posix_map_preload;
#endif
    file_handle->fh_unmap = __wt_posix_unmap;
#endif

    if (pfh->mmap_file_mappable)
        file_handle->fh_read = __posix_file_read_mmap;
    else
        file_handle->fh_read = __posix_file_read;

    file_handle->fh_size = __posix_file_size;
    file_handle->fh_sync = __posix_file_sync;
#ifdef HAVE_SYNC_FILE_RANGE
    file_handle->fh_sync_nowait = __posix_file_sync_nowait;
#endif
#ifdef HAVE_FTRUNCATE
    file_handle->fh_truncate = __posix_file_truncate;
#endif

    if (pfh->mmap_file_mappable)
        file_handle->fh_write = __posix_file_write_mmap;
    else
        file_handle->fh_write = __posix_file_write;

    *file_handlep = file_handle;

    return (0);

err:
    WT_TRET(__posix_file_close((WT_FILE_HANDLE *)pfh, wt_session));
    return (ret);
}

/*
 * __posix_terminate --
 *     Terminate a POSIX configuration.
 */
static int
__posix_terminate(WT_FILE_SYSTEM *file_system, WT_SESSION *wt_session)
{
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;

    __wt_free(session, file_system);
    return (0);
}

/*
 * __wt_os_posix --
 *     Initialize a POSIX configuration.
 */
int
__wt_os_posix(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_FILE_SYSTEM *file_system;

    conn = S2C(session);

    WT_RET(__wt_calloc_one(session, &file_system));

    /* Initialize the POSIX jump table. */
    file_system->fs_directory_list = __wt_posix_directory_list;
    file_system->fs_directory_list_single = __wt_posix_directory_list_single;
    file_system->fs_directory_list_free = __wt_posix_directory_list_free;
    file_system->fs_exist = __posix_fs_exist;
    file_system->fs_open_file = __posix_open_file;
    file_system->fs_remove = __posix_fs_remove;
    file_system->fs_rename = __posix_fs_rename;
    file_system->fs_size = __posix_fs_size;
    file_system->terminate = __posix_terminate;

    /* Switch it into place. */
    conn->file_system = file_system;

    return (0);
}

/*
 * This LWN article (https://lwn.net/Articles/731706/) describes a potential problem when mmap is
 * used over a direct-access (DAX) file system. If a new block is created and then the file is
 * memory-mapped and the client writes to that block via mmap directly into storage (via DAX),
 * the file system may not know that the data was written, so it may not flush the metadata
 * prior to data being written. Therefore, the block may be reallocated or lost upon crash.
 *
 * WiredTiger currently disallows using the mmap option with the direct I/O option. We are relying
 * on the user correctly specifying the direct I/O option if they mount a file system as DAX. If
 * we did not wish to rely on the user supplying the correct flags, we have two options:
 *
 * (1) Use MAP_SYNC flag available on some versions of Linux. The downside is being Linux-specific
 *     and not extensively tested (this is a recent flag).
 *
 * (2) Always fsync when we unmap the file. In our implementation, if a session extends the file by
 *     writing a new block beyond the current file size, we always unmap the file and then re-map it
 *     before allowing any reads or writes via mmap into the new block. If we sync the file upon
 *     unmapping, we will be certain that the metadata is persistent.
 */

/*
 * __wt_map_file --
 *     Map the virtual address region backed by a file into our address space. This is a "best
 *     effort" attempt. If mmap fails for any reason, we silently mark the file as not mappable and
 *     use system calls for it from then on. We do not report the error to the caller: the failure
 *     to mmap is not a show stopper, it is simply a lost performance-enhancement opportunity.
 */
void
__wt_map_file(WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session)
{
    WT_FILE_HANDLE_POSIX *pfh;
    WT_SESSION_IMPL *session;
    wt_off_t file_size;
    void *previous_address;

    session = (WT_SESSION_IMPL *)wt_session;
    pfh = (WT_FILE_HANDLE_POSIX *)file_handle;

    WT_ASSERT(session, pfh->mmap_file_mappable);

    if (__posix_file_size((WT_FILE_HANDLE *)pfh, wt_session, &file_size) != 0) {
        __wt_err(session, __wt_errno(), "%s: __posix_file_size", file_handle->name);
        pfh->mmap_file_mappable = false;
        return;
    }

    if (file_size <= 0) {
        if (pfh->mmap_buf != NULL)
            __wt_unmap_file(file_handle, wt_session);
        return;
    }

    /* If the buffer was previously mapped, try to remap it to the same address */
    previous_address = pfh->mmap_buf;
    if ((pfh->mmap_buf = (uint8_t *)mmap(previous_address, (size_t)file_size, pfh->mmap_prot,
           MAP_SHARED | MAP_FILE, pfh->fd, 0)) == MAP_FAILED) {
        pfh->mmap_size = 0;
        pfh->mmap_buf = NULL;
        pfh->mmap_file_mappable = false;
        __wt_err(
          session, errno, "Could not mmap file %s. Will use system calls.", file_handle->name);
        return;
    }

    pfh->mmap_size = file_size;

    __wt_verbose(session, WT_VERB_FILEOPS,
      "%s: file-mmap: fd=%d, size=%" PRId64 ", mapped buffer=%p", file_handle->name, pfh->fd,
      pfh->mmap_size, (void *)pfh->mmap_buf);
}

/*
 * Here is the synchronization protocol to prevent race conditions when a session is remapping the
 * file while others might be reading or writing it:
 *
 * Every time someone reads or writes from the mapped region, they increment the "use" count via
 * cas. If someone wants to change the file size, they set the "stop" flag. If a session sees the
 * stop flag, it does not read via mmap, but resorts to the regular syscall. The session that set
 * the stop flag spin-waits until the "use" count goes to zero. Then it changes the file size and
 * remaps the region without synchronization. Once all that is done, it resets the "stop" flag.
 */

/*
 * __wt_prepare_remap_resize_file --
 *     Wait until all sessions using the mapped region for I/O are done, so it is safe to remap the
 *     file when it changes size.
 */
void
__wt_prepare_remap_resize_file(WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session)
{
    WT_FILE_HANDLE_POSIX *pfh;
    WT_SESSION_IMPL *session;
    uint64_t sleep_usec, yield_count;

    session = (WT_SESSION_IMPL *)wt_session;
    pfh = (WT_FILE_HANDLE_POSIX *)file_handle;

    sleep_usec = 10;
    yield_count = 0;

    if (!pfh->mmap_file_mappable)
        return;

    __wt_verbose(session, WT_VERB_FILEOPS, "%s, prepare-remap-file: buffer=%p", file_handle->name,
      (void *)pfh->mmap_buf);

wait:
    /* Wait until it looks like no one is resizing the region */
    while (pfh->mmap_resizing == 1)
        __wt_spin_backoff(&yield_count, &sleep_usec);

    if (__wt_atomic_casv32(&pfh->mmap_resizing, 0, 1) == false)
        goto wait;

    /*
     * Wait for any sessions using the region for I/O to finish. Now that we have set the resizing
     * flag, new sessions will not use the region, defaulting to system calls instead.
     */
    while (pfh->mmap_usecount > 0)
        __wt_spin_backoff(&yield_count, &sleep_usec);
}

/*
 * __wt_release_without_remap --
 *     Signal that we are releasing the mapped buffer we wanted to resize, but do not actually remap
 *     the file. If we set the resizing flag earlier, but the operation that tried to resize the
 *     file did not succeed, we will simply reset the flag without resizing.
 */
void
__wt_release_without_remap(WT_FILE_HANDLE *file_handle)
{

    WT_FILE_HANDLE_POSIX *pfh;
    pfh = (WT_FILE_HANDLE_POSIX *)file_handle;

    if (!pfh->mmap_file_mappable)
        return;

    /* Signal that we are done resizing the buffer */
    (void)__wt_atomic_subv32(&pfh->mmap_resizing, 1);
}

/*
 * __wt_remap_resize_file --
 *     After the file size has changed, unmap the file. Then remap it with the new size.
 */
void
__wt_remap_resize_file(WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session)
{
    WT_FILE_HANDLE_POSIX *pfh;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;
    pfh = (WT_FILE_HANDLE_POSIX *)file_handle;

    if (!pfh->mmap_file_mappable)
        return;

    __wt_verbose(session, WT_VERB_FILEOPS, "%s, remap-file: buffer=%p", file_handle->name,
      (void *)pfh->mmap_buf);

    if (pfh->mmap_buf != NULL)
        __wt_unmap_file(file_handle, wt_session);

    __wt_map_file(file_handle, wt_session);
    WT_STAT_CONN_INCRV(session, block_remap_file_resize, 1);

    /* Signal that we are done resizing the buffer */
    (void)__wt_atomic_subv32(&pfh->mmap_resizing, 1);
}

/*
 * __wt_unmap_file --
 *     Unmap the file.
 */
void
__wt_unmap_file(WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session)
{
    WT_DECL_RET;
    WT_FILE_HANDLE_POSIX *pfh;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;
    pfh = (WT_FILE_HANDLE_POSIX *)file_handle;

    __wt_verbose(session, WT_VERB_FILEOPS, "%s, file-unmap: buffer=%p, size=%" PRId64,
      file_handle->name, (void *)pfh->mmap_buf, pfh->mmap_size);

    WT_ASSERT(session, pfh->mmap_file_mappable);

    ret = munmap(pfh->mmap_buf, (size_t)pfh->mmap_size);
    pfh->mmap_buf = NULL;
    pfh->mmap_size = 0;

    if (ret != 0)
        __wt_err(session, ret, "could not unmap file %s", file_handle->name);
}
