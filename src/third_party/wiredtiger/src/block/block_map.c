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
	*(void **)mapp = NULL;
	*maplenp = 0;

	/*
	 * Turn off mapping when verifying the file, because we can't perform
	 * checksum validation of mapped segments, and verify has to checksum
	 * pages.
	 */
	if (block->verify)
		return (0);

	/*
	 * Turn off mapping when direct I/O is configured for the file, the
	 * Linux open(2) documentation says applications should avoid mixing
	 * mmap(2) of files with direct I/O to the same files.
	 */
	if (block->fh->direct_io)
		return (0);

	/*
	 * Turn off mapping if the application configured a cache size maximum,
	 * we can't control how much of the cache size we use in that case.
	 */
	if (block->os_cache_max != 0)
		return (0);

	/*
	 * Map the file into memory.
	 * Ignore errors, we'll read the file through the cache if map fails.
	 */
	(void)__wt_mmap(session, block->fh, mapp, maplenp, mappingcookie);

	return (0);
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
	return (__wt_munmap(session, block->fh, map, maplen, mappingcookie));
}
