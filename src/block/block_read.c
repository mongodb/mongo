/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_bm_preload --
 *	Pre-load a page.
 */
int
__wt_bm_preload(WT_BM *bm,
    WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size)
{
	WT_BLOCK *block;
	WT_DECL_RET;
	wt_off_t offset;
	uint32_t cksum, size;
	bool mapped;

	WT_UNUSED(addr_size);
	block = bm->block;
	ret = EINVAL;		/* Play games due to conditional compilation */

	/* Crack the cookie. */
	WT_RET(__wt_block_buffer_to_addr(block, addr, &offset, &size, &cksum));

	/* Check for a mapped block. */
	mapped = bm->map != NULL && offset + size <= (wt_off_t)bm->maplen;
	if (mapped)
		WT_RET(__wt_mmap_preload(
		    session, (uint8_t *)bm->map + offset, size));
	else {
#ifdef HAVE_POSIX_FADVISE
		ret = posix_fadvise(block->fh->fd,
		    (wt_off_t)offset, (wt_off_t)size, POSIX_FADV_WILLNEED);
#endif
		if (ret != 0) {
			WT_DECL_ITEM(tmp);
			WT_RET(__wt_scr_alloc(session, size, &tmp));
			ret = __wt_block_read_off(
			    session, block, tmp, offset, size, cksum);
			__wt_scr_free(session, &tmp);
			WT_RET(ret);
		}
	}

	WT_STAT_FAST_CONN_INCR(session, block_preload);

	return (0);
}

/*
 * __wt_bm_read --
 *	Map or read address cookie referenced block into a buffer.
 */
int
__wt_bm_read(WT_BM *bm, WT_SESSION_IMPL *session,
    WT_ITEM *buf, const uint8_t *addr, size_t addr_size)
{
	WT_BLOCK *block;
	wt_off_t offset;
	uint32_t cksum, size;
	bool mapped;

	WT_UNUSED(addr_size);
	block = bm->block;

	/* Crack the cookie. */
	WT_RET(__wt_block_buffer_to_addr(block, addr, &offset, &size, &cksum));

	/*
	 * Map the block if it's possible.
	 */
	mapped = bm->map != NULL && offset + size <= (wt_off_t)bm->maplen;
	if (mapped) {
		buf->data = (uint8_t *)bm->map + offset;
		buf->size = size;
		WT_RET(__wt_mmap_preload(session, buf->data, buf->size));

		WT_STAT_FAST_CONN_INCR(session, block_map_read);
		WT_STAT_FAST_CONN_INCRV(session, block_byte_map_read, size);
		return (0);
	}

#ifdef HAVE_DIAGNOSTIC
	/*
	 * In diagnostic mode, verify the block we're about to read isn't on
	 * the available list, or for live systems, the discard list.
	 */
	WT_RET(__wt_block_misplaced(
	    session, block, "read", offset, size, bm->is_live));
#endif
	/* Read the block. */
	WT_RET(__wt_block_read_off(session, block, buf, offset, size, cksum));

#ifdef HAVE_POSIX_FADVISE
	/* Optionally discard blocks from the system's buffer cache. */
	if (block->os_cache_max != 0 &&
	    (block->os_cache += size) > block->os_cache_max) {
		WT_DECL_RET;

		block->os_cache = 0;
		/* Ignore EINVAL - some file systems don't support the flag. */
		if ((ret = posix_fadvise(block->fh->fd,
		    (wt_off_t)0, (wt_off_t)0, POSIX_FADV_DONTNEED)) != 0 &&
		    ret != EINVAL)
			WT_RET_MSG(
			    session, ret, "%s: posix_fadvise", block->name);
	}
#endif
	return (0);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_block_read_off_blind --
 *	Read the block at an offset, try to figure out what it looks like,
 * debugging only.
 */
int
__wt_block_read_off_blind(
    WT_SESSION_IMPL *session, WT_BLOCK *block, WT_ITEM *buf, wt_off_t offset)
{
	WT_BLOCK_HEADER *blk;
	uint32_t cksum, size;

	/*
	 * Make sure the buffer is large enough for the header and read the
	 * the first allocation-size block.
	 */
	WT_RET(__wt_buf_init(session, buf, block->allocsize));
	WT_RET(__wt_read(
	    session, block->fh, offset, (size_t)block->allocsize, buf->mem));
	blk = WT_BLOCK_HEADER_REF(buf->mem);

	/*
	 * Copy out the size and checksum (we're about to re-use the buffer),
	 * and if the size isn't insane, read the rest of the block.
	 */
	size = blk->disk_size;
	cksum = blk->cksum;
	if (__wt_block_offset_invalid(block, offset, size))
		WT_RET_MSG(session, EINVAL,
		    "block at offset %" PRIuMAX " cannot be a valid block, no "
		    "read attempted",
		    (uintmax_t)offset);
	return (__wt_block_read_off(session, block, buf, offset, size, cksum));
}
#endif

/*
 * __wt_block_read_off --
 *	Read an addr/size pair referenced block into a buffer.
 */
int
__wt_block_read_off(WT_SESSION_IMPL *session, WT_BLOCK *block,
    WT_ITEM *buf, wt_off_t offset, uint32_t size, uint32_t cksum)
{
	WT_BLOCK_HEADER *blk;
	size_t bufsize;
	uint32_t page_cksum;

	WT_RET(__wt_verbose(session, WT_VERB_READ,
	    "off %" PRIuMAX ", size %" PRIu32 ", cksum %" PRIu32,
	    (uintmax_t)offset, size, cksum));

	WT_STAT_FAST_CONN_INCR(session, block_read);
	WT_STAT_FAST_CONN_INCRV(session, block_byte_read, size);

	/*
	 * Grow the buffer as necessary and read the block.  Buffers should be
	 * aligned for reading, but there are lots of buffers (for example, file
	 * cursors have two buffers each, key and value), and it's difficult to
	 * be sure we've found all of them.  If the buffer isn't aligned, it's
	 * an easy fix: set the flag and guarantee we reallocate it.  (Most of
	 * the time on reads, the buffer memory has not yet been allocated, so
	 * we're not adding any additional processing time.)
	 */
	if (F_ISSET(buf, WT_ITEM_ALIGNED))
		bufsize = size;
	else {
		F_SET(buf, WT_ITEM_ALIGNED);
		bufsize = WT_MAX(size, buf->memsize + 10);
	}
	WT_RET(__wt_buf_init(session, buf, bufsize));
	WT_RET(__wt_read(session, block->fh, offset, size, buf->mem));
	buf->size = size;

	blk = WT_BLOCK_HEADER_REF(buf->mem);
	if (blk->cksum == cksum) {
		blk->cksum = 0;
		page_cksum = __wt_cksum(buf->mem,
		    F_ISSET(blk, WT_BLOCK_DATA_CKSUM) ?
		    size : WT_BLOCK_COMPRESS_SKIP);
		if (page_cksum == cksum)
			return (0);

		if (!F_ISSET(session, WT_SESSION_QUIET_CORRUPT_FILE))
			__wt_errx(session,
			    "read checksum error for %" PRIu32 "B block at "
			    "offset %" PRIuMAX ": calculated block checksum "
			    "of %" PRIu32 " doesn't match expected checksum "
			    "of %" PRIu32,
			    size, (uintmax_t)offset, page_cksum, cksum);
	} else
		if (!F_ISSET(session, WT_SESSION_QUIET_CORRUPT_FILE))
			__wt_errx(session,
			    "read checksum error for %" PRIu32 "B block at "
			    "offset %" PRIuMAX ": block header checksum "
			    "of %" PRIu32 " doesn't match expected checksum "
			    "of %" PRIu32,
			    size, (uintmax_t)offset, blk->cksum, cksum);

	/* Panic if a checksum fails during an ordinary read. */
	return (block->verify ||
	    F_ISSET(session, WT_SESSION_QUIET_CORRUPT_FILE) ?
	    WT_ERROR : __wt_illegal_value(session, block->name));
}
