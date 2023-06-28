/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_posix_map --
 *     Map a file into memory.
 */
int
__wt_posix_map(WT_FILE_HANDLE *fh, WT_SESSION *wt_session, void **mapped_regionp, size_t *lenp,
  void **mapped_cookiep)
{
    WT_FILE_HANDLE_POSIX *pfh;
    WT_SESSION_IMPL *session;
    wt_off_t file_size;
    size_t len;
    void *map;

    WT_UNUSED(mapped_cookiep);

    session = (WT_SESSION_IMPL *)wt_session;
    pfh = (WT_FILE_HANDLE_POSIX *)fh;

    /*
     * Mapping isn't possible if direct I/O configured for the file, the Linux open(2) documentation
     * says applications should avoid mixing mmap(2) of files with direct I/O to the same files.
     */
    if (pfh->direct_io)
        return (__wt_set_return(session, ENOTSUP));

    /*
     * There's no locking here to prevent the underlying file from changing underneath us, our
     * caller needs to ensure consistency of the mapped region vs. any other file activity.
     */
    WT_RET(fh->fh_size(fh, wt_session, &file_size));
    len = (size_t)file_size;

    __wt_verbose(
      session, WT_VERB_HANDLEOPS, "%s: memory-map: %" WT_SIZET_FMT " bytes", fh->name, len);

    if ((map = mmap(NULL, len, PROT_READ,
#ifdef MAP_NOCORE
           MAP_NOCORE |
#endif
             MAP_PRIVATE,
           pfh->fd, (wt_off_t)0)) == MAP_FAILED)
        WT_RET_MSG(session, __wt_errno(), "%s: memory-map: mmap", fh->name);

    *mapped_regionp = map;
    *lenp = len;
    return (0);
}

#ifdef HAVE_POSIX_MADVISE
/*
 * __wt_posix_map_preload --
 *     Cause a section of a memory map to be faulted in.
 */
int
__wt_posix_map_preload(
  WT_FILE_HANDLE *fh, WT_SESSION *wt_session, const void *map, size_t length, void *mapped_cookie)
{
    WT_BM *bm;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    void *blk;

    WT_UNUSED(mapped_cookie);

    session = (WT_SESSION_IMPL *)wt_session;

    conn = S2C(session);
    bm = S2BT(session)->bm;

    /* Linux requires the address be aligned to a 4KB boundary. */
    blk = (void *)((uintptr_t)map & ~(uintptr_t)(conn->page_size - 1));
    length += WT_PTRDIFF(map, blk);

    /* XXX proxy for "am I doing a scan?" -- manual read-ahead */
    if (F_ISSET(session, WT_SESSION_READ_WONT_NEED)) {
        /* Read in 2MB blocks every 1MB of data. */
        if (((uintptr_t)((uint8_t *)blk + length) & (uintptr_t)((1 << 20) - 1)) < (uintptr_t)blk)
            return (0);
        length =
          WT_MIN(WT_MAX(20 * length, 2 << 20), WT_PTRDIFF((uint8_t *)bm->map + bm->maplen, blk));
    }

    /*
     * Manual pages aren't clear on whether alignment is required for the size, so we will be
     * conservative.
     */
    length &= ~(size_t)(conn->page_size - 1);
    if (length <= (size_t)conn->page_size)
        return (0);

    WT_SYSCALL(posix_madvise(blk, length, POSIX_MADV_WILLNEED), ret);
    if (ret == 0)
        return (0);

    WT_RET_MSG(
      session, ret, "%s: memory-map preload: posix_madvise: POSIX_MADV_WILLNEED", fh->name);
}
#endif

#ifdef HAVE_POSIX_MADVISE
/*
 * __wt_posix_map_discard --
 *     Discard a chunk of the memory map.
 */
int
__wt_posix_map_discard(
  WT_FILE_HANDLE *fh, WT_SESSION *wt_session, void *map, size_t length, void *mapped_cookie)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    void *blk;

    WT_UNUSED(mapped_cookie);

    session = (WT_SESSION_IMPL *)wt_session;
    conn = S2C(session);

    /* Linux requires the address be aligned to a 4KB boundary. */
    blk = (void *)((uintptr_t)map & ~(uintptr_t)(conn->page_size - 1));
    length += WT_PTRDIFF(map, blk);

    WT_SYSCALL(posix_madvise(blk, length, POSIX_MADV_DONTNEED), ret);
    if (ret == 0)
        return (0);

    WT_RET_MSG(
      session, ret, "%s: memory-map discard: posix_madvise: POSIX_MADV_DONTNEED", fh->name);
}
#endif

/*
 * __wt_posix_unmap --
 *     Remove a memory mapping.
 */
int
__wt_posix_unmap(
  WT_FILE_HANDLE *fh, WT_SESSION *wt_session, void *mapped_region, size_t len, void *mapped_cookie)
{
    WT_SESSION_IMPL *session;

    WT_UNUSED(mapped_cookie);

    session = (WT_SESSION_IMPL *)wt_session;

    __wt_verbose(
      session, WT_VERB_HANDLEOPS, "%s: memory-unmap: %" WT_SIZET_FMT " bytes", fh->name, len);

    if (munmap(mapped_region, len) == 0)
        return (0);

    WT_RET_MSG(session, __wt_errno(), "%s: memory-unmap: munmap", fh->name);
}
