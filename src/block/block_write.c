/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
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
__wt_block_header(WT_SESSION_IMPL *session)
{
	WT_UNUSED(session);

	return ((u_int)WT_BLOCK_HEADER_SIZE);
}

/*
 * __wt_block_write_size --
 *	Return the buffer size required to write a block.
 */
int
__wt_block_write_size(
    WT_SESSION_IMPL *session, WT_BLOCK *block, uint32_t *sizep)
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
__wt_block_write(WT_SESSION_IMPL *session,
    WT_BLOCK *block, WT_ITEM *buf, uint8_t *addr, uint32_t *addr_size)
{
	off_t offset;
	uint32_t size, cksum;
	uint8_t *endp;

	WT_UNUSED(addr_size);

	WT_RET(__wt_block_write_off(
	    session, block, buf, &offset, &size, &cksum, 0));

	endp = addr;
	WT_RET(__wt_block_addr_to_buffer(block, &endp, offset, size, cksum));
	*addr_size = WT_PTRDIFF32(endp, addr);

	return (0);
}

/*
 * __wt_block_write_off --
 *	Write a buffer into a block, returning the block's addr/size and
 * checksum.
 */
int
__wt_block_write_off(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_ITEM *buf,
    off_t *offsetp, uint32_t *sizep, uint32_t *cksump, int force_extend)
{
	WT_BLOCK_HEADER *blk;
	WT_PAGE_HEADER *dsk;
	WT_ITEM *tmp;
	off_t offset;
	uint32_t align_size, size;
	int compression_failed, ret;
	uint8_t *src, *dst;
	size_t len, src_len, dst_len, result_len;

	tmp = NULL;
	ret = 0;

	/*
	 * Set the block's in-memory size.
	 *
	 * XXX
	 * Should be set by our caller, it's part of the WT_PAGE_HEADER?
	 */
	dsk = buf->mem;
	dsk->size = buf->size;

	/*
	 * We're passed a table's page image: WT_ITEM->{mem,size} are the image
	 * and byte count.
	 *
	 * Diagnostics: verify the disk page: this violates layering, but it's
	 * the place we can ensure we never write a corrupted page.  Note that
	 * we are verifying the extent list pages, too.  (We created a "page"
	 * type for the extent lists, it was simpler than creating another type
	 * of object in the file.)
	 */
#ifdef HAVE_DIAGNOSTIC
	WT_RET(__wt_verify_dsk(session, "[write-check]", buf));
#endif

	/*
	 * Align the size to an allocation unit.
	 *
	 * The buffer must be big enough for us to zero to the next allocsize
	 * boundary, this is one of the reasons the btree layer must find out
	 * from the block-manager layer the maximum size of the eventual write.
	 */
	align_size = WT_ALIGN(buf->size, block->allocsize);
	if (align_size > buf->memsize)
		WT_RET_MSG(session, EINVAL,
		    "write buffer was incorrectly allocated");

	/*
	 * Optionally stream-compress the data, but don't compress blocks that
	 * are already as small as they're going to get.
	 */
	if (block->compressor == NULL || align_size == block->allocsize) {
not_compressed:	/*
		 * If not compressing the buffer, we need to zero out any unused
		 * bytes at the end.
		 */
		memset(
		    (uint8_t *)buf->mem + buf->size, 0, align_size - buf->size);
		buf->size = align_size;

		/*
		 * Set the in-memory size to the on-page size (we check the size
		 * to decide if a block is compressed: if the sizes match, the
		 * block is NOT compressed).
		 */
		dsk = buf->mem;
	} else {
		/* Skip the first 32B of the source data. */
		src = (uint8_t *)buf->mem + WT_BLOCK_COMPRESS_SKIP;
		src_len = buf->size - WT_BLOCK_COMPRESS_SKIP;

		/*
		 * Compute the size needed for the destination buffer.  We only
		 * allocate enough memory for a copy of the original by default,
		 * if any compressed version is bigger than the original, we
		 * won't use it.  However, some compression engines (snappy is
		 * one example), may need more memory because they don't stop
		 * just because there's no more memory into which to compress.
		 */
		if (block->compressor->pre_size == NULL)
			len = src_len;
		else
			WT_ERR(block->compressor->pre_size(block->compressor,
			    &session->iface, src, src_len, &len));
		WT_RET(__wt_scr_alloc(
		    session, (uint32_t)len + WT_BLOCK_COMPRESS_SKIP, &tmp));

		/* Skip the first 32B of the destination data. */
		dst = (uint8_t *)tmp->mem + WT_BLOCK_COMPRESS_SKIP;
		dst_len = len;

		/*
		 * If compression fails, fallback to the original version.  This
		 * isn't unexpected: if compression doesn't work for some chunk
		 * of bytes for some reason (noting there's likely additional
		 * format/header information which compressed output requires),
		 * it just means the uncompressed version is as good as it gets,
		 * and that's what we use.
		 */
		compression_failed = 0;
		WT_ERR(block->compressor->compress(block->compressor,
		    &session->iface,
		    src, src_len,
		    dst, dst_len,
		    &result_len, &compression_failed));
		if (compression_failed)
			goto not_compressed;

		/*
		 * Set the final data size and see if compression gave us back
		 * at least one allocation unit (if we don't get at least one
		 * file allocation unit, use the uncompressed version because
		 * it will be faster to read).
		 */
		tmp->size = (uint32_t)result_len + WT_BLOCK_COMPRESS_SKIP;
		size = WT_ALIGN(tmp->size, block->allocsize);
		if (size >= align_size)
			goto not_compressed;
		align_size = size;

		/* Copy in the skipped header bytes, zero out unused bytes. */
		memcpy(tmp->mem, buf->mem, WT_BLOCK_COMPRESS_SKIP);
		memset(
		    (uint8_t *)tmp->mem + tmp->size, 0, align_size - tmp->size);

		dsk = tmp->mem;
	}

	blk = WT_BLOCK_HEADER_REF(dsk);

	/*
	 * We increment the block's write generation so it's easy to identify
	 * newer versions of blocks during salvage: it's common in WiredTiger
	 * for multiple blocks to be internally consistent with identical
	 * first and last keys, so we need a way to know the most recent state
	 * of the block.  (We could check to see which leaf is referenced by
	 * by the internal page, which implies salvaging internal pages (which
	 * I don't want to do), and it's not quite as good anyway, because the
	 * internal page may not have been written to disk after the leaf page
	 * was updated.  So, write generations it is.)
	 */
	blk->write_gen = ++block->live.write_gen;

	blk->disk_size = align_size;

	/*
	 * Update the block's checksum: checksum the compressed contents, not
	 * the uncompressed contents.  If the computed checksum happens to be
	 * equal to the special "not set" value, increment it.  We do the same
	 * on the checking side.
	 */
	if (block->checksum) {
		blk->cksum = 0;
		blk->cksum = __wt_cksum(dsk, align_size);
		if (blk->cksum == WT_BLOCK_CHECKSUM_NOT_SET)
			++blk->cksum;
	} else
		blk->cksum = WT_BLOCK_CHECKSUM_NOT_SET;

	/*
	 * Allocate space from the underlying file and write the block.  Always
	 * extend the file when writing snapshot extents, that's easier than
	 * distinguishing between extents allocated from the live avail list,
	 * and those which can't be allocated from the live avail list such as
	 * blocks for writing the live avail list itself.
	 */
	if (force_extend)
		WT_ERR(__wt_block_extend(
		    session, block, &offset, (off_t)align_size));
	else
		WT_ERR(__wt_block_alloc(
		    session, block, &offset, (off_t)align_size));
	WT_ERR(__wt_write(session, block->fh, offset, align_size, dsk));

	WT_BSTAT_INCR(session, page_write);
	WT_CSTAT_INCR(session, block_write);

	WT_VERBOSE(session, write,
	    "off %" PRIuMAX ", size %" PRIu32 ", cksum %" PRIu32,
	    (uintmax_t)offset, align_size, blk->cksum);

	*offsetp = offset;
	*sizep = align_size;
	*cksump = blk->cksum;

err:	__wt_scr_free(&tmp);
	return (ret);
}
