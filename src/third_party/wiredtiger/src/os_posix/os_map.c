/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_mmap --
 *	Map a file into memory.
 */
int
__wt_mmap(WT_SESSION_IMPL *session,
    WT_FH *fh, void *mapp, size_t *lenp, void **mappingcookie)
{
	void *map;
	size_t orig_size;

	WT_UNUSED(mappingcookie);

	/*
	 * Record the current size and only map and set that as the length, it
	 * could change between the map call and when we set the return length.
	 * For the same reason we could actually map past the end of the file;
	 * we don't read bytes past the end of the file though, so as long as
	 * the map call succeeds, it's all OK.
	 */
	orig_size = (size_t)fh->size;
	if ((map = mmap(NULL, orig_size,
	    PROT_READ,
#ifdef MAP_NOCORE
	    MAP_NOCORE |
#endif
	    MAP_PRIVATE,
	    fh->fd, (wt_off_t)0)) == MAP_FAILED) {
		WT_RET_MSG(session, __wt_errno(),
		    "%s map error: failed to map %" WT_SIZET_FMT " bytes",
		    fh->name, orig_size);
	}
	(void)__wt_verbose(session, WT_VERB_FILEOPS,
	    "%s: map %p: %" WT_SIZET_FMT " bytes", fh->name, map, orig_size);

	*(void **)mapp = map;
	*lenp = orig_size;
	return (0);
}

#define	WT_VM_PAGESIZE	4096

/*
 * __wt_mmap_preload --
 *	Cause a section of a memory map to be faulted in.
 */
int
__wt_mmap_preload(WT_SESSION_IMPL *session, const void *p, size_t size)
{
#ifdef HAVE_POSIX_MADVISE
	/* Linux requires the address be aligned to a 4KB boundary. */
	WT_BM *bm = S2BT(session)->bm;
	WT_DECL_RET;
	void *blk = (void *)((uintptr_t)p & ~(uintptr_t)(WT_VM_PAGESIZE - 1));
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
	size &= ~(size_t)(WT_VM_PAGESIZE - 1);

	if (size > WT_VM_PAGESIZE &&
	    (ret = posix_madvise(blk, size, POSIX_MADV_WILLNEED)) != 0)
		WT_RET_MSG(session, ret, "posix_madvise will need");
#else
	WT_UNUSED(session);
	WT_UNUSED(p);
	WT_UNUSED(size);
#endif

	return (0);
}

/*
 * __wt_mmap_discard --
 *	Discard a chunk of the memory map.
 */
int
__wt_mmap_discard(WT_SESSION_IMPL *session, void *p, size_t size)
{
#ifdef HAVE_POSIX_MADVISE
	/* Linux requires the address be aligned to a 4KB boundary. */
	WT_DECL_RET;
	void *blk = (void *)((uintptr_t)p & ~(uintptr_t)(WT_VM_PAGESIZE - 1));
	size += WT_PTRDIFF(p, blk);

	if ((ret = posix_madvise(blk, size, POSIX_MADV_DONTNEED)) != 0)
		WT_RET_MSG(session, ret, "posix_madvise don't need");
#else
	WT_UNUSED(session);
	WT_UNUSED(p);
	WT_UNUSED(size);
#endif
	return (0);
}

/*
 * __wt_munmap --
 *	Remove a memory mapping.
 */
int
__wt_munmap(WT_SESSION_IMPL *session,
    WT_FH *fh, void *map, size_t len, void **mappingcookie)
{
	WT_UNUSED(mappingcookie);

	WT_RET(__wt_verbose(session, WT_VERB_FILEOPS,
	    "%s: unmap %p: %" WT_SIZET_FMT " bytes", fh->name, map, len));

	if (munmap(map, len) == 0)
		return (0);

	WT_RET_MSG(session, __wt_errno(),
	    "%s unmap error: failed to unmap %" WT_SIZET_FMT " bytes",
	    fh->name, len);
}
