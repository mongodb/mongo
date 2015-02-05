/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
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
	 * Record the current size and only map and set that as the length, it
	 * could change between the map call and when we set the return length.
	 * For the same reason we could actually map past the end of the file;
	 * we don't read bytes past the end of the file though, so as long as
	 * the map call succeeds, it's all OK.
	 */
	orig_size = (size_t)fh->size;
	*mappingcookie =
	    CreateFileMappingA(fh->filehandle, NULL, PAGE_READONLY, 0, 0, NULL);
	if (*mappingcookie == NULL)
		WT_RET_MSG(session, __wt_errno(),
			"%s CreateFileMapping error: failed to map %"
			WT_SIZET_FMT " bytes",
			fh->name, orig_size);

	if ((map = MapViewOfFile(
	    *mappingcookie, FILE_MAP_READ, 0, 0, orig_size)) == NULL) {
		CloseHandle(*mappingcookie);
		*mappingcookie = NULL;

		WT_RET_MSG(session, __wt_errno(),
		    "%s map error: failed to map %" WT_SIZET_FMT " bytes",
		    fh->name, orig_size);
	}
	(void)__wt_verbose(session, WT_VERB_FILEOPS,
	    "%s: MapViewOfFile %p: %" WT_SIZET_FMT " bytes",
	    fh->name, map, orig_size);

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
	    "%s: UnmapViewOfFile %p: %" WT_SIZET_FMT " bytes",
	    fh->name, map, len));

	if (UnmapViewOfFile(map) == 0) {
		WT_RET_MSG(session, __wt_errno(),
		    "%s UnmapViewOfFile error: failed to unmap %" WT_SIZET_FMT
		    " bytes",
		    fh->name, len);
	}

	if (CloseHandle(*mappingcookie) == 0) {
		WT_RET_MSG(session, __wt_errno(),
		    "CloseHandle: MapViewOfFile: %s", fh->name);
	}

	*mappingcookie = 0;

	return (0);
}
