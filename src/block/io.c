/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * Don't compress the first 32B of the block (almost all of the WT_PAGE_DISK
 * structure) because we need the block's checksum and on-disk and in-memory
 * sizes to be immediately available without decompression (the checksum and
 * the on-disk block sizes are used during salvage to figure out where the
 * blocks are, and the in-memory page size tells us how large a buffer we need
 * to decompress the file block.  We could take less than 32B, but a 32B
 * boundary is probably better alignment for the underlying compression engine,
 * and skipping 32B won't matter in terms of compression efficiency.
 */
#define	COMPRESS_SKIP    32

/*
 * __wt_block_read --
 *	Read a block into a buffer.
 */
int
__wt_block_read(WT_SESSION_IMPL *session,
    WT_BUF *buf, uint32_t addr, uint32_t size, uint32_t flags)
{
	WT_BTREE *btree;
	WT_BUF *tmp;
	WT_FH *fh;
	WT_ITEM src, dst;
	WT_PAGE_DISK *dsk;
	uint32_t checksum;
	int ret;

	btree = session->btree;
	tmp = NULL;
	fh = btree->fh;
	ret = 0;

	WT_RET(__wt_read(
	    session, fh, WT_ADDR_TO_OFF(btree, addr), size, buf->mem));
	buf->size = size;

	dsk = buf->mem;
	checksum = dsk->checksum;
	dsk->checksum = 0;
	if (checksum != __wt_cksum(dsk, size)) {
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
		memcpy(tmp->mem, buf->mem, COMPRESS_SKIP);
		src.data = (uint8_t *)buf->mem + COMPRESS_SKIP;
		src.size = buf->size - COMPRESS_SKIP;
		dst.data = (uint8_t *)tmp->mem + COMPRESS_SKIP;
		dst.size = dsk->memsize - COMPRESS_SKIP;
		WT_ERR(btree->compressor->decompress(
		    btree->compressor, &session->iface, &src, &dst));
		tmp->size = dsk->memsize;

		__wt_buf_swap(tmp, buf);
		dsk = buf->mem;
	}

	WT_BSTAT_INCR(session, page_read);
	WT_CSTAT_INCR(session, block_read);

	WT_VERBOSE(session, READ,
	    "read addr/size %" PRIu32 "/%" PRIu32 ": %s",
	    addr, size, __wt_page_type_string(dsk->type));

	/* Optionally verify the page: used by salvage and verify. */
	if (LF_ISSET(WT_VERIFY))
		WT_ERR(__wt_verify_dsk(session, buf->mem, addr, buf->size));

err:	__wt_scr_free(&tmp);
	return (ret);
}

/*
 * __wt_block_write --
 *	Write a buffer to disk, returning the addr/size pair for the block.
 */
int
__wt_block_write(
    WT_SESSION_IMPL *session, WT_BUF *buf, uint32_t *addrp, uint32_t *sizep)
{
	WT_ITEM src, dst;
	WT_BTREE *btree;
	WT_BUF *tmp;
	WT_PAGE_DISK *dsk;
	uint32_t addr, align_size, size;
	uint8_t orig_type;
	int compression_failed, ret;

	btree = session->btree;
	tmp = NULL;
	ret = 0;

	/*
	 * We're passed in a WT_BUF that references some chunk of memory that's
	 * a table's page image.  WT_BUF->size is the byte count of the image,
	 * and WT_BUF->data is the image itself.
	 *
	 * Diagnostics: verify the disk page.  We have no disk address to use
	 * for error messages, use 0 (WT_ADDR_INVALID is a big, big number).
	 * This violates layering, but it's the place we can ensure we never
	 * write a corrupted page.
	 */
	WT_ASSERT(session,
	    __wt_verify_dsk(session, buf->mem, 0, buf->size) == 0);

	/*
	 * The WT_PAGE_DISK->type field is after the 32B we leave uncompressed
	 * in the block, so we have to save a copy for our eventual verbose
	 * message.
	 */
	dsk = buf->mem;
	orig_type = dsk->type;

	/* Set the in-memory size, then align it to an allocation unit. */
	dsk->memsize = buf->size;
	align_size = WT_ALIGN(buf->size, btree->allocsize);

	/*
	 * Optionally stream-compress the data, but don't compress blocks that
	 * are already as small as they're going to get.
	 */
	if (btree->compressor == NULL || align_size == btree->allocsize) {
not_compressed:	/*
		 * If not compressing the buffer, we need to zero out any unused
		 * bytes at the end.
		 *
		 * We know the buffer is big enough for us to zero to the next
		 * allocsize boundary: our callers must allocate enough memory
		 * for the buffer so that we can do this operation.  Why don't
		 * our callers just zero out the buffer themselves?  Because we
		 * have to zero out the end of the buffer in the compression
		 * case: so, we can either test compression in our callers and
		 * zero or not-zero based on that test, splitting the code to
		 * zero out the buffer into two parts, or require our callers
		 * allocate enough memory for us to zero here without copying.
		 * Both choices suck.
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
		/*
		 * Allocate a buffer for disk compression; only allocate enough
		 * memory for a copy of the original, if any compressed version
		 * is bigger than the original, we won't use it.
		 */
		WT_RET(__wt_scr_alloc(session, buf->size, &tmp));

		/* Skip the first 32B of the data. */
		src.data = (uint8_t *)buf->mem + COMPRESS_SKIP;
		src.size = buf->size - COMPRESS_SKIP;
		dst.data = (uint8_t *)tmp->mem + COMPRESS_SKIP;
		dst.size = src.size;

		/*
		 * If compression fails, fallback to the original version.  This
		 * isn't unexpected: if compression doesn't work for some chunk
		 * of bytes for some reason (noting there's likely additional
		 * format/header information which compressed output requires),
		 * it just means the uncompressed version is as good as it gets,
		 * and that's what we use.
		 */
		compression_failed = 0;
		WT_ERR(btree->compressor->compress(btree->compressor,
		    &session->iface, &src, &dst, &compression_failed));
		if (compression_failed)
			goto not_compressed;

		/*
		 * Set the final data size and see if compression gave us back
		 * at least one allocation unit (if we don't get at least one
		 * file allocation unit, use the uncompressed version because
		 * it will be faster to read).
		 */
		tmp->size = dst.size + COMPRESS_SKIP;
		size = WT_ALIGN(tmp->size, btree->allocsize);
		if (size >= align_size)
			goto not_compressed;
		align_size = size;

		/* Copy in the skipped header bytes, zero out unused bytes. */
		memcpy(tmp->mem, buf->mem, COMPRESS_SKIP);
		memset(
		    (uint8_t *)tmp->mem + tmp->size, 0, align_size - tmp->size);

		dsk = tmp->mem;
		dsk->size = align_size;
	}

	/* Allocate blocks from the underlying file. */
	WT_ERR(__wt_block_alloc(session, &addr, align_size));

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
	WT_LSN_INCR(btree->lsn);
	dsk->lsn = btree->lsn;

	/*
	 * Update the block's checksum: checksum the compressed contents, not
	 * the uncompressed contents.
	 */
	dsk->checksum = 0;
	dsk->checksum = __wt_cksum(dsk, align_size);
	WT_ERR(__wt_write(
	    session, btree->fh, WT_ADDR_TO_OFF(btree, addr), align_size, dsk));

	WT_BSTAT_INCR(session, page_write);
	WT_CSTAT_INCR(session, block_write);

	WT_VERBOSE(session, WRITE,
	    "write %" PRIu32 " at addr/size %" PRIu32 "/%" PRIu32 ", %s%s",
	    dsk->memsize, addr, dsk->size,
	    dsk->size < dsk->memsize ? "compressed, " : "",
	    __wt_page_type_string(orig_type));

	*addrp = addr;
	*sizep = align_size;

err:	__wt_scr_free(&tmp);
	return (ret);
}
