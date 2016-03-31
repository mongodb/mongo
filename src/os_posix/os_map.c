/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_posix_map --
 *	Map a file into memory.
 */
int
__wt_posix_map(WT_SESSION_IMPL *session,
    WT_FH *fh, void *mapp, size_t *lenp, void **mappingcookie)
{
	size_t len;
	wt_off_t file_size;
	void *map;

	WT_UNUSED(mappingcookie);

	WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_IN_MEMORY));

	/*
	 * Mapping isn't possible if direct I/O configured for the file, the
	 * Linux open(2) documentation says applications should avoid mixing
	 * mmap(2) of files with direct I/O to the same files.
	 */
	if (fh->direct_io)
		return (ENOTSUP);

	/*
	 * There's no locking here to prevent the underlying file from changing
	 * underneath us, our caller needs to ensure consistency of the mapped
	 * region vs. any other file activity.
	 */
	WT_RET(__wt_filesize(session, fh, &file_size));
	len = (size_t)file_size;

	(void)__wt_verbose(session, WT_VERB_HANDLEOPS,
	    "%s: memory-map: %" WT_SIZET_FMT " bytes", fh->name, len);

	if ((map = mmap(NULL, len,
	    PROT_READ,
#ifdef MAP_NOCORE
	    MAP_NOCORE |
#endif
	    MAP_PRIVATE,
	    fh->fd, (wt_off_t)0)) == MAP_FAILED)
		WT_RET_MSG(session,
		    __wt_errno(), "%s: memory-map: mmap", fh->name);

	*(void **)mapp = map;
	*lenp = len;
	return (0);
}

#ifdef HAVE_POSIX_MADVISE
/*
 * __posix_map_preload_madvise --
 *	Cause a section of a memory map to be faulted in.
 */
static int
__posix_map_preload_madvise(
    WT_SESSION_IMPL *session, WT_FH *fh, const void *p, size_t size)
{
	WT_BM *bm;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	void *blk;

	conn = S2C(session);
	bm = S2BT(session)->bm;

	/* Linux requires the address be aligned to a 4KB boundary. */
	blk = (void *)((uintptr_t)p & ~(uintptr_t)(conn->page_size - 1));
	size += WT_PTRDIFF(p, blk);

	/* XXX proxy for "am I doing a scan?" -- manual read-ahead */
	if (F_ISSET(session, WT_SESSION_NO_CACHE)) {
		/* Read in 2MB blocks every 1MB of data. */
		if (((uintptr_t)((uint8_t *)blk + size) &
		    (uintptr_t)((1<<20) - 1)) < (uintptr_t)blk)
			return (0);
		size = WT_MIN(WT_MAX(20 * size, 2 << 20),
		    WT_PTRDIFF((uint8_t *)bm->map + bm->maplen, blk));
	}

	/*
	 * Manual pages aren't clear on whether alignment is required for the
	 * size, so we will be conservative.
	 */
	size &= ~(size_t)(conn->page_size - 1);

	if (size <= (size_t)conn->page_size ||
	    (ret = posix_madvise(blk, size, POSIX_MADV_WILLNEED)) == 0)
		return (0);
	WT_RET_MSG(session, ret,
	    "%s: memory-map preload: posix_madvise: POSIX_MADV_WILLNEED",
	    fh->name);
}
#endif

/*
 * __wt_posix_map_preload --
 *	Cause a section of a memory map to be faulted in.
 */
int
__wt_posix_map_preload(
    WT_SESSION_IMPL *session, WT_FH *fh, const void *p, size_t size)
{
	WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_IN_MEMORY));

#ifdef HAVE_POSIX_MADVISE
	return (__posix_map_preload_madvise(session, fh, p, size));
#else
	WT_UNUSED(fh);
	WT_UNUSED(p);
	WT_UNUSED(size);
	return (ENOTSUP);
#endif
}

#ifdef HAVE_POSIX_MADVISE
/*
 * __posix_map_discard_madvise --
 *	Discard a chunk of the memory map.
 */
static int
__posix_map_discard_madvise(
    WT_SESSION_IMPL *session, WT_FH *fh, void *p, size_t size)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	void *blk;

	conn = S2C(session);

	/* Linux requires the address be aligned to a 4KB boundary. */
	blk = (void *)((uintptr_t)p & ~(uintptr_t)(conn->page_size - 1));
	size += WT_PTRDIFF(p, blk);

	if ((ret = posix_madvise(blk, size, POSIX_MADV_DONTNEED)) == 0)
		return (0);
	WT_RET_MSG(session, ret,
	    "%s: memory-map discard: posix_madvise: POSIX_MADV_DONTNEED",
	    fh->name);
}
#endif

/*
 * __wt_posix_map_discard --
 *	Discard a chunk of the memory map.
 */
int
__wt_posix_map_discard(
    WT_SESSION_IMPL *session, WT_FH *fh, void *p, size_t size)
{
	WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_IN_MEMORY));

#ifdef HAVE_POSIX_MADVISE
	return (__posix_map_discard_madvise(session, fh, p, size));
#else
	WT_UNUSED(fh);
	WT_UNUSED(p);
	WT_UNUSED(size);
	return (ENOTSUP);
#endif
}

/*
 * __wt_posix_map_unmap --
 *	Remove a memory mapping.
 */
int
__wt_posix_map_unmap(WT_SESSION_IMPL *session,
    WT_FH *fh, void *map, size_t len, void **mappingcookie)
{
	WT_UNUSED(mappingcookie);

	WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_IN_MEMORY));

	(void)__wt_verbose(session, WT_VERB_HANDLEOPS,
	    "%s: memory-unmap: %" WT_SIZET_FMT " bytes", fh->name, len);

	if (munmap(map, len) == 0)
		return (0);

	WT_RET_MSG(session, __wt_errno(), "%s: memory-unmap: munmap", fh->name);
}
