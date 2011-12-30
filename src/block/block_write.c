/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_block_write_buf --
 *	Write a buffer into a block, returning the block's address cookie.
 */
int
__wt_block_write_buf(WT_SESSION_IMPL *session,
    WT_BLOCK *block, WT_BUF *buf, uint8_t *addr, uint32_t *addr_size)
{
	off_t offset;
	uint32_t size, cksum;
	uint8_t *endp;

	WT_UNUSED(addr_size);

	/*
	 * We're passed a table's page image: WT_BUF->{data,size} are the image
	 * and byte count.
	 *
	 * Diagnostics: verify the disk page: this violates layering, but it's
	 * the place we can ensure we never write a corrupted page.
	 */
#ifdef HAVE_DIAGNOSTIC
	{
	int ret;
	if ((ret = __wt_verify_dsk(session,
	    "[write-check]", (WT_PAGE_DISK *)buf->data, buf->size)) != 0)
		return (ret);
	}
#endif

	WT_RET(__wt_block_write(session, block, buf, &offset, &size, &cksum));

	endp = addr;
	WT_RET(__wt_block_addr_to_buffer(block, &endp, offset, size, cksum));
	*addr_size = WT_PTRDIFF32(endp, addr);

	return (0);
}

/*
 * __wt_block_write --
 *	Write a buffer into a block, returning the block's addr/size and
 * checksum.
 */
int
__wt_block_write(WT_SESSION_IMPL *session, WT_BLOCK *block,
    WT_BUF *buf, off_t *offsetp, uint32_t *sizep, uint32_t *cksump)
{
	WT_BUF *tmp;
	WT_PAGE_DISK *dsk;
	off_t offset;
	uint32_t align_size, size;
	int compression_failed, ret;
	uint8_t *src, *dst;
	size_t len, src_len, dst_len, result_len;

	tmp = NULL;
	ret = 0;

	/* Set the in-memory size, then align it to an allocation unit. */
	dsk = buf->mem;
	dsk->memsize = buf->size;
	align_size = WT_ALIGN(buf->size, block->allocsize);

	/*
	 * The buffer must be big enough for us to zero to the next allocsize
	 * boundary: our callers must allocate enough memory for the buffer so
	 * that we can do this operation.  Why don't our callers just zero out
	 * the buffer themselves?  Because we have to zero out the end of the
	 * buffer in the compression case: so, we can either test compression
	 * in our callers and zero or not-zero based on that test, splitting
	 * the code to zero out the buffer into two parts, or require callers
	 * allocate enough memory for us to zero here without copying.  Both
	 * choices suck.
	 */
	if (align_size > buf->memsize) {
		__wt_errx(session, "write buffer was incorrectly allocated");
		return (WT_ERROR);
	}

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
		dsk->size = align_size;
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
		dsk->size = align_size;
	}

	/* Allocate blocks from the underlying file. */
	WT_ERR(__wt_block_alloc(session, block, &offset, align_size));

	/*
	 * The disk write function sets things in the WT_PAGE_DISK header simply
	 * because it's easy to do it here.  In a transactional store, things
	 * may be a little harder.
	 *
	 * We increment the block's LSN in non-transactional stores so it's easy
	 * to identify newer versions of blocks during salvage: both blocks are
	 * likely to be internally consistent, and might have the same initial
	 * and last keys, so we need a way to know the most recent state of the
	 * block.  Alternatively, we could check to see which leaf is referenced
	 * by the internal page, which implies salvaging internal pages (which
	 * I don't want to do), and it's not quite as good anyway, because the
	 * internal page may not have been written to disk after the leaf page
	 * was updated.
	 */
	WT_LSN_INCR(block->lsn);
	dsk->lsn = block->lsn;

	/*
	 * Update the block's checksum: checksum the compressed contents, not
	 * the uncompressed contents.
	 */
	dsk->cksum = 0;
	if (block->checksum)
		dsk->cksum = __wt_cksum(dsk, align_size);
	WT_ERR(__wt_write(session, block->fh, offset, align_size, dsk));

	WT_BSTAT_INCR(session, page_write);
	WT_CSTAT_INCR(session, block_write);

	WT_VERBOSE(session, write,
	    "%" PRIu32 " at offset/size %" PRIuMAX "/%" PRIu32 ", %s",
	    dsk->memsize, (uintmax_t)offset, dsk->size,
	    dsk->size < dsk->memsize ? "compressed, " : "");

	*offsetp = offset;
	*sizep = align_size;
	*cksump = dsk->cksum;

err:	__wt_scr_free(&tmp);
	return (ret);
}
