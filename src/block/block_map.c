/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_block_map --
 *	Map a segment of the file in, if possible.
 */
int
__wt_block_map(
    WT_SESSION_IMPL *session, WT_BLOCK *block, void *mapp, size_t *maplenp,
    void **mappingcookie)
{
	WT_DECL_RET;

	*(void **)mapp = NULL;
	*maplenp = 0;

#ifdef WORDS_BIGENDIAN
	/*
	 * The underlying objects are little-endian, mapping objects isn't
	 * currently supported on big-endian systems.
	 */
	WT_UNUSED(session);
	WT_UNUSED(block);
	WT_UNUSED(mappingcookie);
#else
	/* Map support is configurable. */
	if (!S2C(session)->mmap)
		return (0);

	/*
	 * Turn off mapping when verifying the file, because we can't perform
	 * checksum validation of mapped segments, and verify has to checksum
	 * pages.
	 */
	if (block->verify)
		return (0);

	/*
	 * Turn off mapping if the application configured a cache size maximum,
	 * we can't control how much of the cache size we use in that case.
	 */
	if (block->os_cache_max != 0)
		return (0);

	/*
	 * Map the file into memory.
	 * Ignore not-supported errors, we'll read the file through the cache
	 * if map fails.
	 */
	ret = block->fh->fh_map(
	    session, block->fh, mapp, maplenp, mappingcookie);
	if (ret == ENOTSUP)
		ret = 0;
#endif

	return (ret);
}

/*
 * __wt_block_unmap --
 *	Unmap any mapped-in segment of the file.
 */
int
__wt_block_unmap(
    WT_SESSION_IMPL *session, WT_BLOCK *block, void *map, size_t maplen,
    void **mappingcookie)
{
	/* Unmap the file from memory. */
	return (block->fh->fh_map_unmap(
	    session, block->fh, map, maplen, mappingcookie));
}
