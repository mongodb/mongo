/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * Don't compress the first 32B of the page (that's the WT_PAGE_DISK
 * structure) because we need a place to store the in-memory size of
 * the page so we know how large a buffer we need to decompress this
 * particular chunk.  (We only really need 4B, but a 32B boundary is
 * probably better alignment for the underlying engine, and skipping
 * 32B won't matter in terms of compression efficiency.)
 */
#define	COMPRESS_SKIP    32

/*
 * __wt_disk_read --
 *	Read a file page.
 */
int
__wt_disk_read(
    WT_SESSION_IMPL *session, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size)
{
	WT_BTREE *btree;
	WT_FH *fh;
	uint32_t checksum;

	btree = session->btree;
	fh = btree->fh;

	WT_RET(__wt_read(session, fh, WT_ADDR_TO_OFF(btree, addr), size, dsk));

	checksum = dsk->checksum;
	dsk->checksum = 0;
	if (checksum != __wt_cksum(dsk, size))
		WT_FAILURE_RET(session, WT_ERROR,
		    "read checksum error: %" PRIu32 "/%" PRIu32, addr, size);

	WT_BSTAT_INCR(session, page_read);
	WT_CSTAT_INCR(session, block_read);

	WT_VERBOSE(session, READ,
	    "read addr/size %" PRIu32 "/%" PRIu32 ": %s",
	    addr, size, __wt_page_type_string(dsk->type));

	return (0);
}

/*
 * __wt_disk_decompress --
 *	Decompress a file page into another buffer.
 */
int
__wt_disk_decompress(
    WT_SESSION_IMPL *session, WT_PAGE_DISK *comp_dsk, WT_PAGE_DISK *mem_dsk)
{
	WT_ITEM source;
	WT_ITEM dest;
	int ret;
	WT_COMPRESSOR *compressor;

	compressor = session->btree->compressor;

	WT_ASSERT(session, comp_dsk->size < comp_dsk->memsize);
	WT_ASSERT(session, compressor != NULL);

	/* Copy the skipped bytes of the original image into place. */
	memcpy(mem_dsk, comp_dsk, COMPRESS_SKIP);

	/* Decompress the buffer. */
	source.data = (uint8_t *)comp_dsk + COMPRESS_SKIP;
	source.size = comp_dsk->size - COMPRESS_SKIP;
	dest.data = (uint8_t *)mem_dsk + COMPRESS_SKIP;
	dest.size = comp_dsk->memsize - COMPRESS_SKIP;
	if ((ret = compressor->decompress(
	    compressor, &session->iface, &source, &dest)) != 0)
		__wt_err(session, ret, "decompress error");
	return (ret);
}

/*
 * __wt_disk_read_scr --
 *	Read a file page into scratch buffer.
 *	If compression is on, the scratch buffer is reallocated and returned.
 *	The size is also passed in and returned.
 */
int
__wt_disk_read_scr(
    WT_SESSION_IMPL *session, WT_BUF *buf, uint32_t addr, uint32_t size)
{
	WT_BUF *newbuf;
	WT_PAGE_DISK *dsk;
	int ret;

	dsk = buf->mem;
	WT_RET(__wt_disk_read(session, dsk, addr, size));

	/*
	 * If the in-memory and on-disk sizes aren't the same, the buffer
	 * is compressed.
	 */
	if (dsk->size != dsk->memsize) {
		/* Get a new scratch buffer and decompress into it */
		WT_RET(__wt_scr_alloc(session, dsk->memsize, &newbuf));
		if ((ret =
		    __wt_disk_decompress(session, dsk, newbuf->mem)) != 0) {
			__wt_scr_release(&newbuf);
			return (ret);
		}

		/* Caller gets the newbuf's data swapped into their orig buf */
		__wt_buf_swap(newbuf, buf);
		__wt_scr_release(&newbuf);
	}

	return (0);
}

/*
 * __wt_disk_read_realloc --
 *	Read a file page into a buffer that was malloced.
 *	If compression is on, a newly malloced buffer is returned
 *	with the uncompressed contents, while the original buffer is freed.
 *	The size is also passed in and returned.
 */
int
__wt_disk_read_realloc(
    WT_SESSION_IMPL *session, WT_PAGE_DISK **pdsk, uint32_t addr,
    uint32_t *psize)
{
	WT_PAGE_DISK *dsk;
	WT_PAGE_DISK *newdsk;
	int ret;

	dsk = *pdsk;

	WT_RET(__wt_disk_read(session, dsk, addr, *psize));

	/* dsk->size is the compressed size, should be what we just read */
	WT_ASSERT(session, dsk->size == *psize);

	/*
	 * If the in-memory and on-disk sizes aren't the same, the buffer
	 * is compressed.
	 */
	if (dsk->size != dsk->memsize) {

		/* Get a new malloc buffer and decompress into it */
		WT_RET(__wt_calloc(session, dsk->memsize,
			sizeof(uint8_t), &newdsk));
		if ((ret = __wt_disk_decompress(session, dsk, newdsk)) != 0) {
			__wt_free(session, newdsk);
			return (ret);
		}

		/* Caller gets the decompressed buffer */
		*psize = dsk->memsize;
		*pdsk = newdsk;
		__wt_free(session, dsk);
	}

	return (0);
}

/*
 * __wt_disk_write --
 *	Write a file page.
 */
int
__wt_disk_write(
    WT_SESSION_IMPL *session, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size)
{
	WT_BTREE *btree;
	WT_FH *fh;

	btree = session->btree;
	fh = btree->fh;

	/*
	 * The disk write function sets a few things in the WT_PAGE_DISK header
	 * simply because it's easy to do it here.  In a transactional store,
	 * things may be a little harder.
	 *
	 * We increment the page LSN in non-transactional stores so it's easy
	 * to identify newer versions of pages during salvage: both pages are
	 * likely to be internally consistent, and might have the same initial
	 * and last keys, so we need a way to know the most recent state of the
	 * page.  Alternatively, we could check to see which leaf is referenced
	 * by the internal page, which implies salvaging internal pages (which
	 * I don't want to do), and it's not quite as good anyway, because the
	 * internal page may not have been written to disk after the leaf page
	 * was updated.
	 */
	WT_LSN_INCR(btree->lsn);
	dsk->lsn = btree->lsn;
	dsk->size = size;

	WT_ASSERT(session, btree->compressor != NULL ||
	    dsk->memsize == 0 || dsk->memsize == dsk->size);
	if (dsk->memsize == 0)
		dsk->memsize = size;

	WT_ASSERT(session, btree->compressor != NULL ||
	    __wt_verify_dsk(session, dsk, addr, size, 0) == 0);

	dsk->checksum = 0;
	dsk->checksum = __wt_cksum(dsk, size);
	WT_RET(
	    __wt_write(session, fh, WT_ADDR_TO_OFF(btree, addr), size, dsk));

	WT_BSTAT_INCR(session, page_write);
	WT_CSTAT_INCR(session, block_write);

	WT_VERBOSE(session, WRITE,
	    "write addr/size %" PRIu32 "/%" PRIu32 ": %s",
	    addr, size, __wt_page_type_string(dsk->type));
	return (0);
}

/*
 * __wt_disk_compress --
 *	Compress a file page.
 */
int
__wt_disk_compress(WT_SESSION_IMPL *session,
    WT_PAGE_DISK *mem_dsk, WT_PAGE_DISK *comp_dsk, uint32_t *psize)
{
	WT_BTREE *btree;
	WT_ITEM source;
	WT_ITEM dest;
	int ret;
	uint32_t size;

	btree = session->btree;

	/*
	 * On input, *psize is the size of both the source mem_dsk, and also
	 * the allocated size of the compressed comp_dsk.  On output, *psize
	 * is set to the size used in comp_dsk.  If the compressed page cannot
	 * fit into the allotted space, there will be no error, but *psize will
	 * be unchanged.
	 */
	size = *psize;

	WT_ASSERT(session, btree->compressor != NULL);
	WT_ASSERT(session, WT_ALIGN(*psize, btree->allocsize) == size);

	/*
	 * Set disk sizes for verify_dsk.  We have no disk address to use for
	 * verify_dsk's error messages, so use zero.
	 */
	mem_dsk->size = mem_dsk->memsize = size;
	WT_ASSERT(session, __wt_verify_dsk(session, mem_dsk, 0, size, 0) == 0);

	source.data = (uint8_t *)mem_dsk + COMPRESS_SKIP;
	dest.data = (uint8_t *)comp_dsk + COMPRESS_SKIP;
	source.size = dest.size = size - COMPRESS_SKIP;

	if ((ret = btree->compressor->compress(
	    btree->compressor, &session->iface, &source, &dest)) == 0 &&
	    dest.size != source.size) {
		memcpy(comp_dsk, mem_dsk, COMPRESS_SKIP);
		comp_dsk->memsize = size;

		/*
		 * Align the returned compressed size to a buffer boundary.
		 * Zero out the trailing end of the buffer.
		 */
		size = WT_ALIGN(dest.size + COMPRESS_SKIP, btree->allocsize);
		memset((uint8_t *)dest.data + dest.size, 0,
		    size - (dest.size + COMPRESS_SKIP));
		*psize = size;

		/* save the compressed size, we'll need that on decompress */
		comp_dsk->size = dest.size + COMPRESS_SKIP;
	} else if (ret == WT_TOOSMALL)
		/* Not a reportable error, don't change *psize */
		ret = 0;
	else if (ret != 0)
		__wt_err(session, ret, "compress error");

	return (ret);
}
