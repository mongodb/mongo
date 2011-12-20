/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_block_read --
 *	Read a block into a buffer.
 */
int
__wt_block_read(WT_SESSION_IMPL *session,
    WT_BUF *buf, uint32_t addr, uint32_t size, uint32_t cksum)
{
	WT_BTREE *btree;
	WT_BUF *tmp;
	WT_FH *fh;
	WT_ITEM src, dst;
	WT_PAGE_DISK *dsk;
	int ret;

	btree = session->btree;
	tmp = NULL;
	fh = btree->fh;
	ret = 0;

	WT_VERBOSE(
	    session, read, "addr/size %" PRIu32 "/%" PRIu32, addr, size);
	WT_RET(__wt_read(
	    session, fh, WT_ADDR_TO_OFF(btree, addr), size, buf->mem));
	buf->size = size;

	dsk = buf->mem;
	cksum = dsk->cksum;
	dsk->cksum = 0;
	if (cksum != __wt_cksum(dsk, size)) {
		if (!F_ISSET(session, WT_SESSION_SALVAGE_QUIET_ERR))
			__wt_errx(session,
			    "read checksum error: %" PRIu32 "/%" PRIu32,
			    addr, size);
		return (WT_ERROR);
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
		WT_ERR(btree->compressor->decompress(
		    btree->compressor, &session->iface, &src, &dst));
		tmp->size = dsk->memsize;

		__wt_buf_swap(tmp, buf);
		dsk = buf->mem;
	}

	WT_BSTAT_INCR(session, page_read);
	WT_CSTAT_INCR(session, block_read);

err:	__wt_scr_free(&tmp);
	return (ret);
}
