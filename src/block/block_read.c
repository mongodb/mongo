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
    WT_BUF *buf, const uint8_t *addrbuf, uint32_t addrbuf_size)
{
	WT_BUF *tmp;
	uint32_t addr, size, cksum;
	int ret;

	ret = 0;

	/* Crack the cookie. */
	WT_RET(__wt_block_buffer_to_addr(addrbuf, &addr, &size, &cksum));

	/* Re-size the buffer as necessary. */
	WT_RET(__wt_buf_initsize(session, buf, size));

	/* Read the block. */
	WT_RET(__wt_block_read(session, block, buf, addr, size, cksum));

	/* Optionally verify the page. */
	if (block->fragbits == NULL)
		return (0);

	WT_RET(__wt_scr_alloc(session, 0, &tmp));
	WT_ERR(
	    __wt_block_addr_string(session, block, tmp, addrbuf, addrbuf_size));
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
    WT_BUF *buf, uint32_t addr, uint32_t size, uint32_t cksum)
{
	WT_BUF *tmp;
	WT_ITEM src, dst;
	WT_PAGE_DISK *dsk;
	uint32_t page_cksum;
	int ret;

	tmp = NULL;
	ret = 0;

	WT_VERBOSE(
	    session, read, "addr/size %" PRIu32 "/%" PRIu32, addr, size);

	/* Do the read, validate the checksum. */
	WT_RET(__wt_read(
	    session, block->fh, WT_ADDR_TO_OFF(block, addr), size, buf->mem));
	buf->size = size;

	dsk = buf->mem;
	if (block->checksum && cksum != 0 && dsk->cksum != 0) {
		dsk->cksum = 0;
		page_cksum = __wt_cksum(dsk, size);
		if (cksum != page_cksum) {
			if (!F_ISSET(session, WT_SESSION_SALVAGE_QUIET_ERR))
				__wt_errx(session,
				    "read checksum error [%"
				    PRIu32 "-%" PRIu32 ", %" PRIu32 ", %"
				    PRIu32 " != %" PRIu32 "]",
				    addr, addr + (size / 512 - 1),
				    size, cksum, page_cksum);
			return (WT_ERROR);
		}
	}

	/*
	 * If the in-memory page size is larger than the on-disk page size, the
	 * buffer is compressed: allocate a temporary buffer, copy the skipped
	 * bytes of the original image into place, then decompress.
	 */
	if (dsk->size < dsk->memsize) {
		WT_RET(__wt_scr_alloc(session, dsk->memsize, &tmp));
		memcpy(tmp->mem, buf->mem, WT_COMPRESS_SKIP);
		src.data = (uint8_t *)buf->mem + WT_COMPRESS_SKIP;
		src.size = buf->size - WT_COMPRESS_SKIP;
		dst.data = (uint8_t *)tmp->mem + WT_COMPRESS_SKIP;
		dst.size = dsk->memsize - WT_COMPRESS_SKIP;
		WT_ERR(block->compressor->decompress(
		    block->compressor, &session->iface, &src, &dst));
		tmp->size = dsk->memsize;

		__wt_buf_swap(tmp, buf);
		dsk = buf->mem;
	}

	WT_BSTAT_INCR(session, page_read);
	WT_CSTAT_INCR(session, block_read);

err:	__wt_scr_free(&tmp);
	return (ret);
}
