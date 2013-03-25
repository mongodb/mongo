/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
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
__wt_mmap(WT_SESSION_IMPL *session, WT_FH *fh, void *mapp, size_t *lenp)
{
	void *map;

	WT_VERBOSE_RET(session, fileops,
	    "%s: map %" PRIuMAX " bytes", fh->name, (uintmax_t)fh->file_size);

	if ((map = mmap(NULL, (size_t)fh->file_size,
	    PROT_READ,
#ifdef MAP_NOCORE
	    MAP_NOCORE |
#endif
	    MAP_PRIVATE,
	    fh->fd, (off_t)0)) == MAP_FAILED) {
		WT_RET_MSG(session, __wt_errno(),
		    "%s map error: failed to map %" PRIuMAX " bytes",
		    fh->name, (uintmax_t)fh->file_size);
	}

	*(void **)mapp = map;
	*lenp = (size_t)fh->file_size;
	return (0);
}

/*
 * __wt_munmap --
 *	Remove a memory mapping.
 */
int
__wt_munmap(WT_SESSION_IMPL *session, WT_FH *fh, void *map, size_t len)
{
	WT_VERBOSE_RET(session, fileops,
	    "%s: unmap %" PRIuMAX " bytes", fh->name, (uintmax_t)len);

	if (munmap(map, len) == 0)
		return (0);

	WT_RET_MSG(session, __wt_errno(),
	    "%s unmap error: failed to unmap %" PRIuMAX " bytes",
	    fh->name, (uintmax_t)len);
}
