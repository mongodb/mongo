/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_block_read_buf --
 *	Read filesystem cookie referenced block into a buffer.
 */
int
__wt_block_read_buf(WT_SESSION_IMPL *session, WT_BLOCK *block,
    WT_BUF *buf, const uint8_t *addr, uint32_t addr_size)
{
	WT_BUF *tmp;
	off_t offset;
	uint32_t size, cksum;
	int ret;

	ret = 0;

	/* Crack the cookie. */
	WT_RET(__wt_block_buffer_to_addr(block, addr, &offset, &size, &cksum));

	/* Read the block. */
	WT_RET(__wt_block_read(session, block, buf, offset, size, cksum));

	/* Optionally verify the page. */
	if (block->fragbits == NULL)
		return (0);

	WT_RET(__wt_scr_alloc(session, 0, &tmp));
	WT_ERR(__wt_block_addr_string(session, block, tmp, addr, addr_size));
	WT_ERR(__wt_verify_dsk(
	    session, (char *)tmp->data, buf->mem, buf->size));

err:	__wt_scr_free(&tmp);

	return (ret);
}

/*
 * __wt_block_read --
 *	Read an addr/size pair referenced block into a buffer.
 */
int
__wt_block_read(WT_SESSION_IMPL *session, WT_BLOCK *block,
    WT_BUF *buf, off_t offset, uint32_t size, uint32_t cksum)
{
	WT_BLOCK_HEADER *blk;
	WT_BUF *tmp;
	WT_PAGE_HEADER *dsk;
	size_t result_len;
	uint32_t page_cksum;
	int ret;

	tmp = NULL;
	ret = 0;

	WT_VERBOSE(session, read,
	    "off %" PRIuMAX ", size %" PRIu32 ", cksum %" PRIu32,
	    (uintmax_t)offset, size, cksum);

	/*
	 * If we're compressing the file blocks, place the initial read into a
	 * scratch buffer, we're going to have to re-allocate more memory for
	 * decompression.  Else check the caller's buffer size and grow it as
	 * necessary, there will only be one buffer.
	 */
	if (block->compressor == NULL) {
		WT_RET(__wt_buf_init(session, buf, size));
		buf->size = size;
		dsk = buf->mem;
	} else {
		WT_RET(__wt_scr_alloc(session, size, &tmp));
		tmp->size = size;
		dsk = tmp->mem;
	}

	/* Read. */
	WT_ERR(__wt_read(session, block->fh, offset, size, dsk));
	blk = WT_BLOCK_HEADER_REF(dsk);

	/* Validate the checksum. */
	if (block->checksum && cksum != 0 && blk->cksum != 0) {
		blk->cksum = 0;
		page_cksum = __wt_cksum(dsk, size);
		if (cksum != page_cksum) {
			if (!F_ISSET(session, WT_SESSION_SALVAGE_QUIET_ERR))
				__wt_errx(session,
				    "read checksum error [%"
				    PRIu32 "B @ %" PRIuMAX ", %"
				    PRIu32 " != %" PRIu32 "]",
				    size, (uintmax_t)offset, cksum, page_cksum);
			WT_ERR(WT_ERROR);
		}
	}

	/*
	 * If the in-memory block size is larger than the on-disk block size,
	 * the block is compressed.   Size the user's buffer, copy the skipped
	 * bytes of the original image into place, then decompress.
	 *
	 * If the in-memory block size is less than or equal to the on-disk
	 * block size, the block is not compressed.
	 */
	if (blk->disk_size < dsk->size) {
		if (block->compressor == NULL)
			WT_ERR(__wt_illegal_value(session));

		WT_RET(__wt_buf_init(session, buf, dsk->size));
		buf->size = dsk->size;

		/*
		 * Note the source length is NOT the number of compressed bytes,
		 * it's the length of the block we just read (minus the skipped
		 * bytes).  We don't store the number of compressed bytes: some
		 * compression engines need that length stored externally, they
		 * don't have markers in the stream to signal the end of the
		 * compressed bytes.  Those engines must store the compressed
		 * byte length somehow, see the snappy compression extension for
		 * an example.
		 */
		memcpy(buf->mem, tmp->mem, WT_BLOCK_COMPRESS_SKIP);
		WT_ERR(block->compressor->decompress(
		    block->compressor, &session->iface,
		    (uint8_t *)tmp->mem + WT_BLOCK_COMPRESS_SKIP,
		    tmp->size - WT_BLOCK_COMPRESS_SKIP,
		    (uint8_t *)buf->mem + WT_BLOCK_COMPRESS_SKIP,
		    dsk->size - WT_BLOCK_COMPRESS_SKIP,
		    &result_len));
		if (result_len != dsk->size - WT_BLOCK_COMPRESS_SKIP)
			WT_ERR(__wt_illegal_value(session));
	} else
		if (block->compressor == NULL)
			buf->size = dsk->size;
		else
			/*
			 * We guessed wrong: there was a compressor, but this
			 * block was not compressed, and now the page is in the
			 * wrong buffer and the buffer may be of the wrong size.
			 * This should be rare, why configure a compressor that
			 * doesn't work?  Allocate a buffer of the right size
			 * (we used a scratch buffer which might be large), and
			 * copy the data into place.
			 */
			WT_ERR(
			    __wt_buf_set(session, buf, tmp->data, dsk->size));

	WT_BSTAT_INCR(session, page_read);
	WT_CSTAT_INCR(session, block_read);

err:	__wt_scr_free(&tmp);
	return (ret);
}
