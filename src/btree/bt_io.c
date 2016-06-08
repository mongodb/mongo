/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
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
	WT_DECL_ITEM(etmp);
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	WT_ENCRYPTOR *encryptor;
	WT_ITEM *ip;
	const WT_PAGE_HEADER *dsk;
	const char *fail_msg;
	size_t result_len;

	btree = S2BT(session);
	bm = btree->bm;
	fail_msg = NULL;			/* -Wuninitialized */

	/*
	 * If anticipating a compressed or encrypted block, read into a scratch
	 * buffer and decompress into the caller's buffer.  Else, read directly
	 * into the caller's buffer.
	 */
	if (btree->compressor == NULL && btree->kencryptor == NULL) {
		WT_RET(bm->read(bm, session, buf, addr, addr_size));
		dsk = buf->data;
		ip = NULL;
	} else {
		WT_RET(__wt_scr_alloc(session, 0, &tmp));
		WT_ERR(bm->read(bm, session, tmp, addr, addr_size));
		dsk = tmp->data;
		ip = tmp;
	}

	/*
	 * If the block is encrypted, copy the skipped bytes of the original
	 * image into place, then decrypt.
	 */
	if (F_ISSET(dsk, WT_PAGE_ENCRYPTED)) {
		if (btree->kencryptor == NULL ||
		    (encryptor = btree->kencryptor->encryptor) == NULL ||
		    encryptor->decrypt == NULL) {
			fail_msg =
			    "encrypted block in file for which no encryption "
			    "configured";
			goto corrupt;
		}

		WT_ERR(__wt_scr_alloc(session, 0, &etmp));
		if ((ret = __wt_decrypt(session,
		    encryptor, WT_BLOCK_ENCRYPT_SKIP, ip, etmp)) != 0) {
			fail_msg = "block decryption failed";
			goto corrupt;
		}

		ip = etmp;
		dsk = ip->data;
	} else if (btree->kencryptor != NULL) {
		fail_msg =
		    "unencrypted block in file for which encryption configured";
		goto corrupt;
	}

	if (F_ISSET(dsk, WT_PAGE_COMPRESSED)) {
		if (btree->compressor == NULL ||
		    btree->compressor->decompress == NULL) {
			fail_msg =
			    "compressed block in file for which no compression "
			    "configured";
			goto corrupt;
		}

		/*
		 * Size the buffer based on the in-memory bytes we're expecting
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
		memcpy(buf->mem, ip->data, WT_BLOCK_COMPRESS_SKIP);
		ret = btree->compressor->decompress(
		    btree->compressor, &session->iface,
		    (uint8_t *)ip->data + WT_BLOCK_COMPRESS_SKIP,
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
		    result_len != dsk->mem_size - WT_BLOCK_COMPRESS_SKIP) {
			fail_msg = "block decryption failed";
			goto corrupt;
		}
	} else
		/*
		 * If we uncompressed above, the page is in the correct buffer.
		 * If we get here the data may be in the wrong buffer and the
		 * buffer may be the wrong size.  If needed, get the page
		 * into the destination buffer.
		 */
		if (ip != NULL)
			WT_ERR(__wt_buf_set(
			    session, buf, ip->data, dsk->mem_size));

	/* If the handle is a verify handle, verify the physical page. */
	if (F_ISSET(btree, WT_BTREE_VERIFY)) {
		if (tmp == NULL)
			WT_ERR(__wt_scr_alloc(session, 0, &tmp));
		WT_ERR(bm->addr_string(bm, session, tmp, addr, addr_size));
		WT_ERR(__wt_verify_dsk(session, tmp->data, buf));
	}

	WT_STAT_FAST_CONN_INCR(session, cache_read);
	WT_STAT_FAST_DATA_INCR(session, cache_read);
	if (F_ISSET(dsk, WT_PAGE_COMPRESSED))
		WT_STAT_FAST_DATA_INCR(session, compress_read);
	WT_STAT_FAST_CONN_INCRV(session, cache_bytes_read, dsk->mem_size);
	WT_STAT_FAST_DATA_INCRV(session, cache_bytes_read, dsk->mem_size);

	if (0) {
corrupt:	if (ret == 0)
			ret = WT_ERROR;
		if (!F_ISSET(btree, WT_BTREE_VERIFY) &&
		    !F_ISSET(session, WT_SESSION_QUIET_CORRUPT_FILE)) {
			__wt_err(session, ret, "%s", fail_msg);
			ret = __wt_illegal_value(session, btree->dhandle->name);
		}
	}

err:	__wt_scr_free(session, &tmp);
	__wt_scr_free(session, &etmp);
	return (ret);
}

/*
 * __wt_bt_write --
 *	Write a buffer into a block, returning the block's addr/size and
 * checksum.
 */
int
__wt_bt_write(WT_SESSION_IMPL *session, WT_ITEM *buf,
    uint8_t *addr, size_t *addr_sizep, bool checkpoint, bool compressed)
{
	WT_BM *bm;
	WT_BTREE *btree;
	WT_DECL_ITEM(ctmp);
	WT_DECL_ITEM(etmp);
	WT_DECL_RET;
	WT_KEYED_ENCRYPTOR *kencryptor;
	WT_ITEM *ip;
	WT_PAGE_HEADER *dsk;
	size_t dst_len, len, result_len, size, src_len;
	int compression_failed;		/* Extension API, so not a bool. */
	uint8_t *dst, *src;
	bool data_cksum, encrypted;

	btree = S2BT(session);
	bm = btree->bm;
	encrypted = false;

	/* Checkpoint calls are different than standard calls. */
	WT_ASSERT(session,
	    (!checkpoint && addr != NULL && addr_sizep != NULL) ||
	    (checkpoint && addr == NULL && addr_sizep == NULL));

	/* In-memory databases shouldn't write pages. */
	WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_IN_MEMORY));

#ifdef HAVE_DIAGNOSTIC
	/*
	 * We're passed a table's disk image.  Decompress if necessary and
	 * verify the image.  Always check the in-memory length for accuracy.
	 */
	dsk = buf->mem;
	if (compressed) {
		WT_ERR(__wt_scr_alloc(session, dsk->mem_size, &ctmp));

		memcpy(ctmp->mem, buf->data, WT_BLOCK_COMPRESS_SKIP);
		WT_ERR(btree->compressor->decompress(
		    btree->compressor, &session->iface,
		    (uint8_t *)buf->data + WT_BLOCK_COMPRESS_SKIP,
		    buf->size - WT_BLOCK_COMPRESS_SKIP,
		    (uint8_t *)ctmp->data + WT_BLOCK_COMPRESS_SKIP,
		    ctmp->memsize - WT_BLOCK_COMPRESS_SKIP,
		    &result_len));
		WT_ASSERT(session,
		    dsk->mem_size == result_len + WT_BLOCK_COMPRESS_SKIP);
		ctmp->size = (uint32_t)result_len + WT_BLOCK_COMPRESS_SKIP;
		ip = ctmp;
	} else {
		WT_ASSERT(session, dsk->mem_size == buf->size);
		ip = buf;
	}
	WT_ERR(__wt_verify_dsk(session, "[write-check]", ip));
	__wt_scr_free(session, &ctmp);
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
		WT_ERR(__wt_scr_alloc(session, size, &ctmp));

		/* Skip the header bytes of the destination data. */
		dst = (uint8_t *)ctmp->mem + WT_BLOCK_COMPRESS_SKIP;
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
			compressed = true;
			WT_STAT_FAST_DATA_INCR(session, compress_write);

			/*
			 * Copy in the skipped header bytes, set the final data
			 * size.
			 */
			memcpy(ctmp->mem, buf->mem, WT_BLOCK_COMPRESS_SKIP);
			ctmp->size = result_len;
			ip = ctmp;
		}
	}
	/*
	 * Optionally encrypt the data.  We need to add in the original
	 * length, in case both compression and encryption are done.
	 */
	if ((kencryptor = btree->kencryptor) != NULL) {
		/*
		 * Get size needed for encrypted buffer.
		 */
		__wt_encrypt_size(session, kencryptor, ip->size, &size);

		WT_ERR(bm->write_size(bm, session, &size));
		WT_ERR(__wt_scr_alloc(session, size, &etmp));
		WT_ERR(__wt_encrypt(session,
		    kencryptor, WT_BLOCK_ENCRYPT_SKIP, ip, etmp));

		encrypted = true;
		ip = etmp;
	}
	dsk = ip->mem;

	/* If the buffer is compressed, set the flag. */
	if (compressed)
		F_SET(dsk, WT_PAGE_COMPRESSED);
	if (encrypted)
		F_SET(dsk, WT_PAGE_ENCRYPTED);

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
	data_cksum = true;		/* -Werror=maybe-uninitialized */
	switch (btree->checksum) {
	case CKSUM_ON:
		data_cksum = true;
		break;
	case CKSUM_OFF:
		data_cksum = false;
		break;
	case CKSUM_UNCOMPRESSED:
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

err:	__wt_scr_free(session, &ctmp);
	__wt_scr_free(session, &etmp);
	return (ret);
}
