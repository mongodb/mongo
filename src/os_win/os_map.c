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
__wt_mmap(WT_SESSION_IMPL *session, WT_FH *fh, void *mapp, size_t *lenp,
   void** mappingcookie)
{
	void *map;
	size_t orig_size;

	/*
	 * Record the current size and only mmap that and set that as the
	 * length.  It could change between mmap and when we set the lenp.
	 */
	orig_size = (size_t)fh->size;
	*mappingcookie =
	    CreateFileMapping(fh->filehandle, NULL, PAGE_READONLY, 0, 0, NULL);
	if (*mappingcookie == NULL)
		WT_RET_MSG(session, __wt_errno(),
			"%s CreateFileMapping error: failed to map %"
			PRIuMAX " bytes",
			fh->name, (uintmax_t)orig_size);

	if ((map = MapViewOfFile(
	    *mappingcookie, FILE_MAP_READ, 0, 0, orig_size)) == NULL) {
		CloseHandle(*mappingcookie);
		*mappingcookie = NULL;

		WT_RET_MSG(session, __wt_errno(),
		    "%s map error: failed to map %" PRIuMAX " bytes",
		    fh->name, (uintmax_t)orig_size);
	}
	(void)__wt_verbose(session, WT_VERB_FILEOPS,
	    "%s: MapViewOfFile %p: %" PRIuMAX " bytes",
	    fh->name, map, (uintmax_t)orig_size);

	*(void **)mapp = map;
	*lenp = orig_size;
	return (0);
}

/*
 * __wt_mmap_preload --
 *	Cause a section of a memory map to be faulted in.
 */
int
__wt_mmap_preload(WT_SESSION_IMPL *session, const void *p, size_t size)
{
	WT_UNUSED(session);
	WT_UNUSED(p);
	WT_UNUSED(size);

	return (0);
}

/*
 * __wt_mmap_discard --
 *	Discard a chunk of the memory map.
 */
int
__wt_mmap_discard(WT_SESSION_IMPL *session, void *p, size_t size)
{
	WT_UNUSED(session);
	WT_UNUSED(p);
	WT_UNUSED(size);
	return (0);
}

/*
 * __wt_munmap --
 *	Remove a memory mapping.
 */
int
__wt_munmap(WT_SESSION_IMPL *session, WT_FH *fh, void *map, size_t len,
   void** mappingcookie)
{
	WT_RET(__wt_verbose(session, WT_VERB_FILEOPS,
	    "%s: UnmapViewOfFile %p: %" PRIuMAX " bytes",
	    fh->name, map, (uintmax_t)len));

	if (UnmapViewOfFile(map) == 0) {
		WT_RET_MSG(session, __wt_errno(),
		    "%s UnmapViewOfFile error: failed to unmap %"
		    PRIuMAX " bytes",
		    fh->name, (uintmax_t)len);
	}

	CloseHandle(*mappingcookie);

	*mappingcookie = 0;

	return (0);
}
