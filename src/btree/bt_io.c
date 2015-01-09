/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_bt_read --
 *	Read a cookie referenced block into a buffer.
 */
int
__wt_bt_read(WT_SESSION_IMPL *session,
    WT_ITEM *buf, const uint8_t *addr, size_t addr_size)
{
	WT_BM *bm;
	WT_BTREE *btree;
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	const WT_PAGE_HEADER *dsk;
	size_t result_len;

	btree = S2BT(session);
	bm = btree->bm;

	/*
	 * If anticipating a compressed block, read into a scratch buffer and
	 * decompress into the caller's buffer.  Else, read directly into the
	 * caller's buffer.
	 */
	if (btree->compressor == NULL) {
		WT_RET(bm->read(bm, session, buf, addr, addr_size));
		dsk = buf->data;
	} else {
		WT_RET(__wt_scr_alloc(session, 0, &tmp));
		WT_ERR(bm->read(bm, session, tmp, addr, addr_size));
		dsk = tmp->data;
	}

	/*
	 * If the block is compressed, copy the skipped bytes of the original
	 * image into place, then decompress.
	 */
	if (F_ISSET(dsk, WT_PAGE_COMPRESSED)) {
		if (btree->compressor == NULL ||
		    btree->compressor->decompress == NULL)
			WT_ERR_MSG(session, WT_ERROR,
			    "read compressed block where no compression engine "
			    "configured");

		/*
		 * We're allocating the exact number of bytes we're expecting
		 * from decompression.
		 */
		WT_ERR(__wt_buf_initsize(session, buf, dsk->mem_size));

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
		memcpy(buf->mem, tmp->data, WT_BLOCK_COMPRESS_SKIP);
		ret = btree->compressor->decompress(
		    btree->compressor, &session->iface,
		    (uint8_t *)tmp->data + WT_BLOCK_COMPRESS_SKIP,
		    tmp->size - WT_BLOCK_COMPRESS_SKIP,
		    (uint8_t *)buf->mem + WT_BLOCK_COMPRESS_SKIP,
		    dsk->mem_size - WT_BLOCK_COMPRESS_SKIP, &result_len);

		/*
		 * If checksums were turned off because we're depending on the
		 * decompression to fail on any corrupted data, we'll end up
		 * here after corruption happens.  If we're salvaging the file,
		 * it's OK, otherwise it's really, really bad.
		 */
		if (ret != 0 ||
		    result_len != dsk->mem_size - WT_BLOCK_COMPRESS_SKIP)
			WT_ERR(
			    F_ISSET(btree, WT_BTREE_VERIFY) ||
			    F_ISSET(session, WT_SESSION_SALVAGE_CORRUPT_OK) ?
			    WT_ERROR :
			    __wt_illegal_value(session, btree->dhandle->name));
	} else
		if (btree->compressor == NULL)
			buf->size = dsk->mem_size;
		else
			/*
			 * We guessed wrong: there was a compressor, but this
			 * block was not compressed, and now the page is in the
			 * wrong buffer and the buffer may be of the wrong size.
			 * This should be rare, but happens with small blocks
			 * that aren't worth compressing.
			 */
			WT_ERR(__wt_buf_set(
			    session, buf, tmp->data, dsk->mem_size));

	/* If the handle is a verify handle, verify the physical page. */
	if (F_ISSET(btree, WT_BTREE_VERIFY)) {
		if (tmp == NULL)
			WT_ERR(__wt_scr_alloc(session, 0, &tmp));
		WT_ERR(bm->addr_string(bm, session, tmp, addr, addr_size));
		WT_ERR(__wt_verify_dsk(session, (const char *)tmp->data, buf));
	}

	WT_STAT_FAST_CONN_INCR(session, cache_read);
	WT_STAT_FAST_DATA_INCR(session, cache_read);
	if (F_ISSET(dsk, WT_PAGE_COMPRESSED))
		WT_STAT_FAST_DATA_INCR(session, compress_read);
	WT_STAT_FAST_CONN_INCRV(session, cache_bytes_read, dsk->mem_size);
	WT_STAT_FAST_DATA_INCRV(session, cache_bytes_read, dsk->mem_size);

err:	__wt_scr_free(session, &tmp);
	return (ret);
}

/*
 * __wt_bt_write --
 *	Write a buffer into a block, returning the block's addr/size and
 * checksum.
 */
int
__wt_bt_write(WT_SESSION_IMPL *session, WT_ITEM *buf,
    uint8_t *addr, size_t *addr_sizep, int checkpoint, int compressed)
{
	WT_BM *bm;
	WT_BTREE *btree;
	WT_ITEM *ip;
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	WT_PAGE_HEADER *dsk;
	size_t len, src_len, dst_len, result_len, size;
	int data_cksum, compression_failed;
	uint8_t *src, *dst;

	btree = S2BT(session);
	bm = btree->bm;

	/* Checkpoint calls are different than standard calls. */
	WT_ASSERT(session,
	    (checkpoint == 0 && addr != NULL && addr_sizep != NULL) ||
	    (checkpoint == 1 && addr == NULL && addr_sizep == NULL));

#ifdef HAVE_DIAGNOSTIC
	/*
	 * We're passed a table's disk image.  Decompress if necessary and
	 * verify the image.  Always check the in-memory length for accuracy.
	 */
	dsk = buf->mem;
	if (compressed) {
		WT_ERR(__wt_scr_alloc(session, dsk->mem_size, &tmp));

		memcpy(tmp->mem, buf->data, WT_BLOCK_COMPRESS_SKIP);
		WT_ERR(btree->compressor->decompress(
		    btree->compressor, &session->iface,
		    (uint8_t *)buf->data + WT_BLOCK_COMPRESS_SKIP,
		    buf->size - WT_BLOCK_COMPRESS_SKIP,
		    (uint8_t *)tmp->data + WT_BLOCK_COMPRESS_SKIP,
		    tmp->memsize - WT_BLOCK_COMPRESS_SKIP,
		    &result_len));
		WT_ASSERT(session,
		    dsk->mem_size == result_len + WT_BLOCK_COMPRESS_SKIP);
		tmp->size = (uint32_t)result_len + WT_BLOCK_COMPRESS_SKIP;
		ip = tmp;
	} else {
		WT_ASSERT(session, dsk->mem_size == buf->size);
		ip = buf;
	}
	WT_ERR(__wt_verify_dsk(session, "[write-check]", ip));
	__wt_scr_free(session, &tmp);
#endif

	/*
	 * Optionally stream-compress the data, but don't compress blocks that
	 * are already as small as they're going to get.
	 */
	if (btree->compressor == NULL ||
	    btree->compressor->compress == NULL || compressed)
		ip = buf;
	else if (buf->size <= btree->allocsize) {
		ip = buf;
		WT_STAT_FAST_DATA_INCR(session, compress_write_too_small);
	} else {
		/* Skip the header bytes of the source data. */
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
		if (btree->compressor->pre_size == NULL)
			len = src_len;
		else
			WT_ERR(btree->compressor->pre_size(btree->compressor,
			    &session->iface, src, src_len, &len));

		size = len + WT_BLOCK_COMPRESS_SKIP;
		WT_ERR(bm->write_size(bm, session, &size));
		WT_ERR(__wt_scr_alloc(session, size, &tmp));

		/* Skip the header bytes of the destination data. */
		dst = (uint8_t *)tmp->mem + WT_BLOCK_COMPRESS_SKIP;
		dst_len = len;

		compression_failed = 0;
		WT_ERR(btree->compressor->compress(btree->compressor,
		    &session->iface,
		    src, src_len,
		    dst, dst_len,
		    &result_len, &compression_failed));
		result_len += WT_BLOCK_COMPRESS_SKIP;

		/*
		 * If compression fails, or doesn't gain us at least one unit of
		 * allocation, fallback to the original version.  This isn't
		 * unexpected: if compression doesn't work for some chunk of
		 * data for some reason (noting likely additional format/header
		 * information which compressed output requires), it just means
		 * the uncompressed version is as good as it gets, and that's
		 * what we use.
		 */
		if (compression_failed ||
		    buf->size / btree->allocsize <=
		    result_len / btree->allocsize) {
			ip = buf;
			WT_STAT_FAST_DATA_INCR(session, compress_write_fail);
		} else {
			compressed = 1;
			WT_STAT_FAST_DATA_INCR(session, compress_write);

			/*
			 * Copy in the skipped header bytes, set the final data
			 * size.
			 */
			memcpy(tmp->mem, buf->mem, WT_BLOCK_COMPRESS_SKIP);
			tmp->size = result_len;
			ip = tmp;
		}
	}
	dsk = ip->mem;

	/* If the buffer is compressed, set the flag. */
	if (compressed)
		F_SET(dsk, WT_PAGE_COMPRESSED);

	/*
	 * We increment the block's write generation so it's easy to identify
	 * newer versions of blocks during salvage.  (It's common in WiredTiger,
	 * at least for the default block manager, for multiple blocks to be
	 * internally consistent with identical first and last keys, so we need
	 * a way to know the most recent state of the block.  We could check
	 * which leaf is referenced by a valid internal page, but that implies
	 * salvaging internal pages, which I don't want to do, and it's not
	 * as good anyway, because the internal page may not have been written
	 * after the leaf page was updated.  So, write generations it is.
	 *
	 * Nothing is locked at this point but two versions of a page with the
	 * same generation is pretty unlikely, and if we did, they're going to
	 * be roughly identical for the purposes of salvage, anyway.
	 */
	dsk->write_gen = ++btree->write_gen;

	/*
	 * Checksum the data if the buffer isn't compressed or checksums are
	 * configured.
	 */
	switch (btree->checksum) {
	case CKSUM_ON:
		data_cksum = 1;
		break;
	case CKSUM_OFF:
		data_cksum = 0;
		break;
	case CKSUM_UNCOMPRESSED:
	default:
		data_cksum = !compressed;
		break;
	}

	/* Call the block manager to write the block. */
	WT_ERR(checkpoint ?
	    bm->checkpoint(bm, session, ip, btree->ckpt, data_cksum) :
	    bm->write(bm, session, ip, addr, addr_sizep, data_cksum));

	WT_STAT_FAST_CONN_INCR(session, cache_write);
	WT_STAT_FAST_DATA_INCR(session, cache_write);
	WT_STAT_FAST_CONN_INCRV(session, cache_bytes_write, dsk->mem_size);
	WT_STAT_FAST_DATA_INCRV(session, cache_bytes_write, dsk->mem_size);

err:	__wt_scr_free(session, &tmp);
	return (ret);
}
