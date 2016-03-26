/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
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
	size_t len;
	void *map;
	wt_off_t file_size;

	/*
	 * There's no locking here to prevent the underlying file from changing
	 * underneath us, our caller needs to ensure consistency of the mapped
	 * region vs. any other file activity.
	 */
	WT_RET(__wt_filesize(session, fh, &file_size));
	len = (size_t)file_size;

	*mappingcookie =
	    CreateFileMappingA(fh->filehandle, NULL, PAGE_READONLY, 0, 0, NULL);
	if (*mappingcookie == NULL)
		WT_RET_MSG(session, __wt_win32_errno(),
			"%s CreateFileMapping error: failed to map %"
			WT_SIZET_FMT " bytes",
			fh->name, len);

	if ((map = MapViewOfFile(
	    *mappingcookie, FILE_MAP_READ, 0, 0, len)) == NULL) {
		CloseHandle(*mappingcookie);
		*mappingcookie = NULL;

		WT_RET_MSG(session, __wt_win32_errno(),
		    "%s map error: failed to map %" WT_SIZET_FMT " bytes",
		    fh->name, len);
	}
	(void)__wt_verbose(session, WT_VERB_FILEOPS,
	    "%s: MapViewOfFile %p: %" WT_SIZET_FMT " bytes",
	    fh->name, map, len);

	*(void **)mapp = map;
	*lenp = len;
	return (0);
}

/*
 * __wt_mmap_preload --
 *	Cause a section of a memory map to be faulted in.
 */
int
__wt_mmap_preload(
    WT_SESSION_IMPL *session, WT_FH *fh, const void *p, size_t size)
{
	WT_UNUSED(session);
	WT_UNUSED(fh);
	WT_UNUSED(p);
	WT_UNUSED(size);

	return (ENOTSUP);
}

/*
 * __wt_mmap_discard --
 *	Discard a chunk of the memory map.
 */
int
__wt_mmap_discard(WT_SESSION_IMPL *session, WT_FH *fh, void *p, size_t size)
{
	WT_UNUSED(session);
	WT_UNUSED(fh);
	WT_UNUSED(p);
	WT_UNUSED(size);

	return (ENOTSUP);
}

/*
 * __wt_munmap --
 *	Remove a memory mapping.
 */
int
__wt_munmap(WT_SESSION_IMPL *session,
    WT_FH *fh, void *map, size_t len, void **mappingcookie)
{
	WT_RET(__wt_verbose(session, WT_VERB_FILEOPS,
	    "%s: UnmapViewOfFile %p: %" WT_SIZET_FMT " bytes",
	    fh->name, map, len));

	if (UnmapViewOfFile(map) == 0) {
		WT_RET_MSG(session, __wt_win32_errno(),
		    "%s UnmapViewOfFile error: failed to unmap %" WT_SIZET_FMT
		    " bytes",
		    fh->name, len);
	}

	if (CloseHandle(*mappingcookie) == 0) {
		WT_RET_MSG(session, __wt_win32_errno(),
		    "CloseHandle: MapViewOfFile: %s", fh->name);
	}

	*mappingcookie = 0;

	return (0);
}
