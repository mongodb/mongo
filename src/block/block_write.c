/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_block_header --
 *	Return the size of the block-specific header.
 */
u_int
__wt_block_header(WT_BLOCK *block)
{
	WT_UNUSED(block);

	return ((u_int)WT_BLOCK_HEADER_SIZE);
}

/*
 * __wt_block_write_size --
 *	Return the buffer size required to write a block.
 */
int
__wt_block_write_size(WT_SESSION_IMPL *session, WT_BLOCK *block, size_t *sizep)
{
	WT_UNUSED(session);

	*sizep = WT_ALIGN(*sizep + WT_BLOCK_HEADER_BYTE_SIZE, block->allocsize);
	return (0);
}

/*
 * __wt_block_write --
 *	Write a buffer into a block, returning the block's address cookie.
 */
int
__wt_block_write(WT_SESSION_IMPL *session, WT_BLOCK *block,
    WT_ITEM *buf, uint8_t *addr, uint32_t *addr_size, int data_cksum)
{
	off_t offset;
	uint32_t size, cksum;
	uint8_t *endp;

	WT_UNUSED(addr_size);

	WT_RET(__wt_block_write_off(
	    session, block, buf, &offset, &size, &cksum, data_cksum, 0));

	endp = addr;
	WT_RET(__wt_block_addr_to_buffer(block, &endp, offset, size, cksum));
	*addr_size = WT_PTRDIFF32(endp, addr);

#ifdef HAVE_SYNC_FILE_RANGE
	/*
	 * If we're writing through the system's buffer cache, schedule a
	 * write immediately so the next checkpoint's fsync doesn't swamp
	 * an underlying SSD.
	 */
	if (!block->fh->direct_io) {
		WT_DECL_RET;

		if ((ret = sync_file_range(block->fh->fd,
		    (off64_t)0, (off64_t)0, SYNC_FILE_RANGE_WRITE)) != 0)
			WT_RET_MSG(
			    session, ret, "%s: sync_file_range", block->name);
	}
#endif
#ifdef HAVE_POSIX_FADVISE
	/*
	 * If we're writing through the system's buffer cache, discard blocks we
	 * no longer need.  The call should be cheap, allowing us to use a limit
	 * small enough it shouldn't require tuning.
	 *
	 * The counter isn't locked in any way, if we race, we race, all we're
	 * doing is a system call to discard buffers.
	 */
	if (!block->fh->direct_io)
		if (block->cache_discard_counter++ > 1000) {
			block->cache_discard_counter = 0;
			WT_RET(__wt_block_cache_discard(session, block));
		}
#endif
	return (0);
}

/*
 * __wt_block_write_off --
 *	Write a buffer into a block, returning the block's addr/size and
 * checksum.
 */
int
__wt_block_write_off(WT_SESSION_IMPL *session, WT_BLOCK *block,
    WT_ITEM *buf, off_t *offsetp, uint32_t *sizep, uint32_t *cksump,
    int data_cksum, int locked)
{
	WT_BLOCK_HEADER *blk;
	WT_DECL_RET;
	off_t offset;
	uint32_t align_size;

	blk = WT_BLOCK_HEADER_REF(buf->mem);

	/* Buffers should be aligned for writing. */
	if (!F_ISSET(buf, WT_ITEM_ALIGNED)) {
		WT_ASSERT(session, F_ISSET(buf, WT_ITEM_ALIGNED));
		WT_RET_MSG(session, EINVAL,
		    "direct I/O check: write buffer incorrectly allocated");
	}

	/*
	 * Align the size to an allocation unit.
	 *
	 * The buffer must be big enough for us to zero to the next allocsize
	 * boundary, this is one of the reasons the btree layer must find out
	 * from the block-manager layer the maximum size of the eventual write.
	 */
	align_size = WT_ALIGN32(buf->size, block->allocsize);
	if (align_size > buf->memsize) {
		WT_ASSERT(session, align_size <= buf->memsize);
		WT_RET_MSG(session, EINVAL,
		    "buffer size check: write buffer incorrectly allocated");
	}

	/* Zero out any unused bytes at the end of the buffer. */
	memset((uint8_t *)buf->mem + buf->size, 0, align_size - buf->size);

	/*
	 * Set the disk size so we don't have to incrementally read blocks
	 * during salvage.
	 */
	blk->disk_size = align_size;

	/*
	 * Update the block's checksum: if our caller specifies, checksum the
	 * complete data, otherwise checksum the leading WT_BLOCK_COMPRESS_SKIP
	 * bytes.  The assumption is applications with good compression support
	 * turn off checksums and assume corrupted blocks won't decompress
	 * correctly.  However, if compression failed to shrink the block, the
	 * block wasn't compressed, in which case our caller will tell us to
	 * checksum the data to detect corruption.   If compression succeeded,
	 * we still need to checksum the first WT_BLOCK_COMPRESS_SKIP bytes
	 * because they're not compressed, both to give salvage a quick test
	 * of whether a block is useful and to give us a test so we don't lose
	 * the first WT_BLOCK_COMPRESS_SKIP bytes without noticing.
	 */
	blk->flags = 0;
	if (data_cksum)
		F_SET(blk, WT_BLOCK_DATA_CKSUM);
	blk->cksum = 0;
	blk->cksum = __wt_cksum(
	    buf->mem, data_cksum ? align_size : WT_BLOCK_COMPRESS_SKIP);

	if (!locked)
		__wt_spin_lock(session, &block->live_lock);
	ret = __wt_block_alloc(session, block, &offset, (off_t)align_size);
	if (!locked)
		__wt_spin_unlock(session, &block->live_lock);
	WT_RET(ret);

	if ((ret = __wt_write(
	    session, block->fh, offset, align_size, buf->mem)) != 0) {
		if (!locked)
			__wt_spin_lock(session, &block->live_lock);
		WT_TRET(
		    __wt_block_off_free(session, block, offset, align_size));
		if (!locked)
			__wt_spin_unlock(session, &block->live_lock);
		WT_RET(ret);
	}

	WT_CSTAT_INCR(session, block_write);
	WT_CSTAT_INCRV(session, block_byte_write, align_size);

	WT_VERBOSE_RET(session, write,
	    "off %" PRIuMAX ", size %" PRIu32 ", cksum %" PRIu32,
	    (uintmax_t)offset, align_size, blk->cksum);

	*offsetp = offset;
	*sizep = align_size;
	*cksump = blk->cksum;

	return (ret);
}
