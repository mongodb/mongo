/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#ifdef HAVE_POSIX_FADVISE
/*
 * __wt_block_map_add --
 *	Add a mapped segment to the block's list.
 */
void
__wt_block_map_add(
    WT_SESSION_IMPL *session, WT_BLOCK *block, void *map, size_t maplen)
{
	WT_BLOCK_MAP_ENTRY *p;
	WT_CONNECTION_IMPL *conn;
	size_t bytes;
	uint32_t i;

	conn = S2C(session);

	/* Add the mapped region to the block's list. */
	__wt_spin_lock(session, &conn->block_lock);
	for (i = 0, p = block->map; i < block->map_entries; ++i, ++p)
		if (p->map == NULL)
			goto slot;
	bytes = (block->map_entries + 20) * sizeof(WT_BLOCK_MAP_ENTRY);
	if (__wt_realloc(session, NULL, bytes, &block->map) == 0) {
		block->map_entries += 20;
		for (i = 0, p = block->map; i < block->map_entries; ++i, ++p)
			if (p->map == NULL) {
slot:				p->map = map;
				p->maplen = maplen;
				break;
			}
	}
	__wt_spin_unlock(session, &conn->block_lock);

	/*
	 * Ignore failure: it only means we might discard too many buffers from
	 * the system's buffer cache.
	 */
}

/*
 * __wt_block_map_del --
 *	Remove a mapped segment from the block's list.
 */
void
__wt_block_map_del(
    WT_SESSION_IMPL *session, WT_BLOCK *block, void *map, size_t maplen)
{
	WT_BLOCK_MAP_ENTRY *p;
	WT_CONNECTION_IMPL *conn;
	uint32_t i;

	conn = S2C(session);

	/* Remove the mapped region from the block's list. */
	__wt_spin_lock(session, &conn->block_lock);
	for (i = 0, p = block->map; i < block->map_entries; ++i, ++p)
		if (p->map == map && p->maplen == maplen) {
			p->map = NULL;
			p->maplen = 0;
			break;
		}
	__wt_spin_unlock(session, &conn->block_lock);
}

/*
 * __wt_block_cache_discard --
 *	Discard any unused portions of the file from the system buffer cache.
 */
int
__wt_block_cache_discard(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_BLOCK_MAP_ENTRY *p;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	size_t maxlen;
	uint32_t i;

	conn = S2C(session);
	maxlen = 0;

	/* Walk the list of mapped regions, find the one we can discard. */
	__wt_spin_lock(session, &conn->block_lock);
	for (i = 0, p = block->map; i < block->map_entries; ++i, ++p)
		if (p->map != NULL && p->maplen > maxlen)
			maxlen = p->maplen;
	__wt_spin_unlock(session, &conn->block_lock);

	if ((ret = posix_fadvise(
	    block->fh->fd, (off_t)maxlen, 0, POSIX_FADV_DONTNEED)) != 0)
		WT_RET_MSG(session, ret, "%s: posix_fadvise", block->name);
	return (0);
}
#endif

/*
 * __wt_block_map --
 *	Map a segment of the file in, if possible.
 */
int
__wt_block_map(
    WT_SESSION_IMPL *session, WT_BLOCK *block, void *mapp, size_t *maplenp)
{
	/*
	 * Map the file into memory.
	 *
	 * Ignore errors, we'll read the file through the cache if map fails.
	 */
	if (__wt_mmap(session, block->fh, mapp, maplenp))
		return (0);

#ifdef HAVE_POSIX_FADVISE
	__wt_block_map_add(session, block, *(void **)mapp, *maplenp);
#endif
	return (0);
}

/*
 * __wt_block_unmap --
 *	Unmap any mapped-in segment of the file.
 */
int
__wt_block_unmap(
    WT_SESSION_IMPL *session, WT_BLOCK *block, void *map, size_t maplen)
{
#ifdef HAVE_POSIX_FADVISE
	__wt_block_map_del(session, block, map, maplen);
#endif
	/* Unmap the file from memory. */
	return (__wt_munmap(session, block->fh, map, maplen));
}
