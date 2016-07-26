/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
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
__wt_bm_preload(
    WT_BM *bm, WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size)
{
	WT_BLOCK *block;
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	WT_FILE_HANDLE *handle;
	wt_off_t offset;
	uint32_t cksum, size;
	bool mapped;

	WT_UNUSED(addr_size);

	block = bm->block;

	WT_STAT_FAST_CONN_INCR(session, block_preload);

	/* Crack the cookie. */
	WT_RET(__wt_block_buffer_to_addr(block, addr, &offset, &size, &cksum));

	handle = block->fh->handle;
	mapped = bm->map != NULL && offset + size <= (wt_off_t)bm->maplen;
	if (mapped && handle->fh_map_preload != NULL)
		ret = handle->fh_map_preload(handle, (WT_SESSION *)session,
		    (uint8_t *)bm->map + offset, size, bm->mapped_cookie);
	if (!mapped && handle->fh_advise != NULL)
		ret = handle->fh_advise(handle, (WT_SESSION *)session,
		    (wt_off_t)offset, (wt_off_t)size, WT_FILE_HANDLE_WILLNEED);
	if (ret != EBUSY && ret != ENOTSUP)
		return (ret);

	/* If preload isn't supported, do it the slow way. */
	WT_RET(__wt_scr_alloc(session, 0, &tmp));
	ret = __wt_bm_read(bm, session, tmp, addr, addr_size);
	__wt_scr_free(session, &tmp);

	return (ret);
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
	WT_DECL_RET;
	WT_FILE_HANDLE *handle;
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
	handle = block->fh->handle;
	mapped = bm->map != NULL && offset + size <= (wt_off_t)bm->maplen;
	if (mapped && handle->fh_map_preload != NULL) {
		buf->data = (uint8_t *)bm->map + offset;
		buf->size = size;
		ret = handle->fh_map_preload(handle, (WT_SESSION *)session,
		    buf->data, buf->size,bm->mapped_cookie);

		WT_STAT_FAST_CONN_INCR(session, block_map_read);
		WT_STAT_FAST_CONN_INCRV(session, block_byte_map_read, size);
		return (ret);
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

	/* Optionally discard blocks from the system's buffer cache. */
	WT_RET(__wt_block_discard(session, block, (size_t)size));

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
	__wt_block_header_byteswap(blk);

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
	WT_BLOCK_HEADER *blk, swap;
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

	/*
	 * We incrementally read through the structure before doing a checksum,
	 * do little- to big-endian handling early on, and then select from the
	 * original or swapped structure as needed.
	 */
	blk = WT_BLOCK_HEADER_REF(buf->mem);
	__wt_block_header_byteswap_copy(blk, &swap);
	if (swap.cksum == cksum) {
		blk->cksum = 0;
		page_cksum = __wt_cksum(buf->mem,
		    F_ISSET(&swap, WT_BLOCK_DATA_CKSUM) ?
		    size : WT_BLOCK_COMPRESS_SKIP);
		if (page_cksum == cksum) {
			/*
			 * Swap the page-header as needed; this doesn't belong
			 * here, but it's the best place to catch all callers.
			 */
			__wt_page_header_byteswap(buf->mem);
			return (0);
		}

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
			    size, (uintmax_t)offset, swap.cksum, cksum);

	/* Panic if a checksum fails during an ordinary read. */
	return (block->verify ||
	    F_ISSET(session, WT_SESSION_QUIET_CORRUPT_FILE) ?
	    WT_ERROR : __wt_illegal_value(session, block->name));
}
