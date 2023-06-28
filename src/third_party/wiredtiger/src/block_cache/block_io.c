/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * Define a function that increments histogram statistics compression ratios.
 */
WT_STAT_COMPR_RATIO_HIST_INCR_FUNC(ratio)

/*
 * __blkcache_read_corrupt --
 *     Handle a failed read.
 */
static int
__blkcache_read_corrupt(WT_SESSION_IMPL *session, int error, const uint8_t *addr, size_t addr_size,
  const char *fail_msg) WT_GCC_FUNC_ATTRIBUTE((cold))
{
    WT_BM *bm;
    WT_BTREE *btree;
    WT_DECL_RET;

    btree = S2BT(session);
    bm = btree->bm;

    ret = error;
    WT_ASSERT(session, ret != 0);

    F_SET(S2C(session), WT_CONN_DATA_CORRUPTION);
    if (!F_ISSET(btree, WT_BTREE_VERIFY) && !F_ISSET(session, WT_SESSION_QUIET_CORRUPT_FILE)) {
        WT_TRET(bm->corrupt(bm, session, addr, addr_size));
        WT_RET_PANIC(session, ret, "%s: fatal read error: %s", btree->dhandle->name, fail_msg);
    }
    return (ret);
}

/*
 * __wt_blkcache_read --
 *     Read an address-cookie referenced block into a buffer.
 */
int
__wt_blkcache_read(WT_SESSION_IMPL *session, WT_ITEM *buf, const uint8_t *addr, size_t addr_size)
{
    WT_BLKCACHE *blkcache;
    WT_BLKCACHE_ITEM *blkcache_item;
    WT_BM *bm;
    WT_BTREE *btree;
    WT_COMPRESSOR *compressor;
    WT_DECL_ITEM(etmp);
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    WT_ENCRYPTOR *encryptor;
    WT_ITEM *ip;
    const WT_PAGE_HEADER *dsk;
    size_t compression_ratio, result_len;
    uint64_t time_diff, time_start, time_stop;
    bool blkcache_found, expect_conversion, found, skip_cache_put, timer;

    blkcache = &S2C(session)->blkcache;
    blkcache_item = NULL;
    btree = S2BT(session);
    bm = btree->bm;
    compressor = btree->compressor;
    encryptor = btree->kencryptor == NULL ? NULL : btree->kencryptor->encryptor;
    blkcache_found = found = false;
    skip_cache_put = (blkcache->type == WT_BLKCACHE_UNCONFIGURED);

    /*
     * If anticipating a compressed or encrypted block, start with a scratch buffer and convert into
     * the caller's buffer. Else, start with the caller's buffer.
     */
    ip = buf;
    expect_conversion = compressor != NULL || encryptor != NULL;
    if (expect_conversion) {
        WT_RET(__wt_scr_alloc(session, 4 * 1024, &tmp));
        ip = tmp;
    }

    /* Check for mapped blocks. */
    WT_RET(__wt_blkcache_map_read(session, ip, addr, addr_size, &found));
    if (found) {
        skip_cache_put = true;
        if (!expect_conversion)
            goto verify;
    }

    /* Check the block cache. */
    if (!found && blkcache->type != WT_BLKCACHE_UNCONFIGURED) {
        __wt_blkcache_get(session, addr, addr_size, &blkcache_item, &found, &skip_cache_put);
        if (found) {
            blkcache_found = true;
            ip->data = blkcache_item->data;
            ip->size = blkcache_item->data_size;
            if (!expect_conversion) {
                /* Copy to the caller's buffer before releasing our reference. */
                WT_ERR(__wt_buf_set(session, buf, ip->data, ip->size));
                goto verify;
            }
        }
    }

    /* Read the block. */
    if (!found) {
        timer = WT_STAT_ENABLED(session) && !F_ISSET(session, WT_SESSION_INTERNAL);
        time_start = timer ? __wt_clock(session) : 0;
        WT_ERR(bm->read(bm, session, ip, addr, addr_size));
        if (timer) {
            time_stop = __wt_clock(session);
            time_diff = WT_CLOCKDIFF_US(time_stop, time_start);
            WT_STAT_CONN_INCR(session, cache_read_app_count);
            WT_STAT_CONN_INCRV(session, cache_read_app_time, time_diff);
            WT_STAT_SESSION_INCRV(session, read_time, time_diff);
        }

        dsk = ip->data;
        WT_STAT_CONN_DATA_INCR(session, cache_read);
        if (F_ISSET(dsk, WT_PAGE_COMPRESSED))
            WT_STAT_DATA_INCR(session, compress_read);
        WT_STAT_CONN_DATA_INCRV(session, cache_bytes_read, dsk->mem_size);
        WT_STAT_SESSION_INCRV(session, bytes_read, dsk->mem_size);
        (void)__wt_atomic_add64(&S2C(session)->cache->bytes_read, dsk->mem_size);
    }

    /*
     * If the block is encrypted, copy the skipped bytes of the image into place, then decrypt. DRAM
     * block-cache blocks are never encrypted.
     */
    dsk = ip->data;
    if (!blkcache_found || blkcache->type != WT_BLKCACHE_DRAM) {
        if (F_ISSET(dsk, WT_PAGE_ENCRYPTED)) {
            if (encryptor == NULL || encryptor->decrypt == NULL)
                WT_ERR(__blkcache_read_corrupt(session, WT_ERROR, addr, addr_size,
                  "encrypted block for which no decryptor configured"));

            /*
             * If checksums were turned off because we're depending on decryption to fail on any
             * corrupted data, we'll end up here on corrupted data.
             */
            WT_ERR(__wt_scr_alloc(session, 0, &etmp));
            if ((ret = __wt_decrypt(session, encryptor, WT_BLOCK_ENCRYPT_SKIP, ip, etmp)) != 0)
                WT_ERR(__blkcache_read_corrupt(
                  session, ret, addr, addr_size, "block decryption failed"));

            ip = etmp;
        } else if (btree->kencryptor != NULL)
            WT_ERR(__blkcache_read_corrupt(session, WT_ERROR, addr, addr_size,
              "unencrypted block for which encryption configured"));
    }

    /* Store the decrypted, possibly compressed, block in the block_cache. */
    if (!skip_cache_put)
        WT_ERR(__wt_blkcache_put(session, ip, addr, addr_size, false));

    dsk = ip->data;
    if (F_ISSET(dsk, WT_PAGE_COMPRESSED)) {
        if (compressor == NULL || compressor->decompress == NULL) {
            ret = __blkcache_read_corrupt(session, WT_ERROR, addr, addr_size,
              "compressed block for which no compression configured");
            /* Odd error handling structure to avoid static analyzer complaints. */
            WT_ERR(ret == 0 ? WT_ERROR : ret);
        }

        /* Size the buffer based on the in-memory bytes we're expecting from decompression. */
        WT_ERR(__wt_buf_initsize(session, buf, dsk->mem_size));

        /*
         * Note the source length is NOT the number of compressed bytes, it's the length of the
         * block we just read (minus the skipped bytes). We don't store the number of compressed
         * bytes: some compression engines need that length stored externally, they don't have
         * markers in the stream to signal the end of the compressed bytes. Those engines must store
         * the compressed byte length somehow, see the snappy compression extension for an example.
         * In other words, the "tmp" in the decompress call isn't a mistake.
         */
        memcpy(buf->mem, ip->data, WT_BLOCK_COMPRESS_SKIP);
        ret = compressor->decompress(btree->compressor, &session->iface,
          (uint8_t *)ip->data + WT_BLOCK_COMPRESS_SKIP, tmp->size - WT_BLOCK_COMPRESS_SKIP,
          (uint8_t *)buf->mem + WT_BLOCK_COMPRESS_SKIP, dsk->mem_size - WT_BLOCK_COMPRESS_SKIP,
          &result_len);
        if (result_len != dsk->mem_size - WT_BLOCK_COMPRESS_SKIP)
            WT_TRET(WT_ERROR);

        /*
         * If checksums were turned off because we're depending on decompression to fail on any
         * corrupted data, we'll end up here on corrupted data.
         */
        if (ret != 0)
            WT_ERR(
              __blkcache_read_corrupt(session, ret, addr, addr_size, "block decompression failed"));

        compression_ratio = result_len / (tmp->size - WT_BLOCK_COMPRESS_SKIP);
        __wt_stat_compr_ratio_hist_incr(session, compression_ratio);

    } else {
        /*
         * If we uncompressed above, the page is in the correct buffer. If we get here the data may
         * be in the wrong buffer and the buffer may be the wrong size. If needed, get the page into
         * the destination buffer.
         */
        if (ip != buf)
            WT_ERR(__wt_buf_set(session, buf, ip->data, dsk->mem_size));
    }

verify:
    /* If the handle is a verify handle, verify the physical page. */
    if (F_ISSET(btree, WT_BTREE_VERIFY)) {
        if (tmp == NULL)
            WT_ERR(__wt_scr_alloc(session, 4 * 1024, &tmp));
        WT_ERR(bm->addr_string(bm, session, tmp, addr, addr_size));
        WT_ERR(__wt_verify_dsk(session, tmp->data, buf));
    }

err:
    /* If we pulled the block from the block cache, decrement its reference count. */
    if (blkcache_found)
        (void)__wt_atomic_subv32(&blkcache_item->ref_count, 1);

    __wt_scr_free(session, &tmp);
    __wt_scr_free(session, &etmp);
    return (ret);
}

/*
 * __wt_blkcache_write --
 *     Write a buffer into a block, returning the block's address cookie.
 */
int
__wt_blkcache_write(WT_SESSION_IMPL *session, WT_ITEM *buf, uint8_t *addr, size_t *addr_sizep,
  size_t *compressed_sizep, bool checkpoint, bool checkpoint_io, bool compressed)
{
    WT_BLKCACHE *blkcache;
    WT_BM *bm;
    WT_BTREE *btree;
    WT_DECL_ITEM(ctmp);
    WT_DECL_ITEM(etmp);
    WT_DECL_RET;
    WT_ITEM *ip;
    WT_KEYED_ENCRYPTOR *kencryptor;
    WT_PAGE_HEADER *dsk;
    size_t dst_len, len, result_len, size, src_len;
    uint64_t time_diff, time_start, time_stop;
    uint8_t *dst, *src;
    int compression_failed; /* Extension API, so not a bool. */
    bool data_checksum, encrypted, timer;

    if (compressed_sizep != NULL)
        *compressed_sizep = 0;

    blkcache = &S2C(session)->blkcache;
    btree = S2BT(session);
    bm = btree->bm;
    encrypted = false;

    /*
     * Optionally stream-compress the data, but don't compress blocks that are already as small as
     * they're going to get.
     */
    if (btree->compressor == NULL || btree->compressor->compress == NULL || compressed)
        ip = buf;
    else if (buf->size <= btree->allocsize) {
        ip = buf;
        WT_STAT_DATA_INCR(session, compress_write_too_small);
    } else {
        /* Skip the header bytes of the source data. */
        src = (uint8_t *)buf->mem + WT_BLOCK_COMPRESS_SKIP;
        src_len = buf->size - WT_BLOCK_COMPRESS_SKIP;

        /*
         * Compute the size needed for the destination buffer. We only allocate enough memory for a
         * copy of the original by default, if any compressed version is bigger than the original,
         * we won't use it. However, some compression engines (snappy is one example), may need more
         * memory because they don't stop just because there's no more memory into which to
         * compress.
         */
        if (btree->compressor->pre_size == NULL)
            len = src_len;
        else
            WT_ERR(
              btree->compressor->pre_size(btree->compressor, &session->iface, src, src_len, &len));

        size = len + WT_BLOCK_COMPRESS_SKIP;
        WT_ERR(bm->write_size(bm, session, &size));
        WT_ERR(__wt_scr_alloc(session, size, &ctmp));

        /* Skip the header bytes of the destination data. */
        dst = (uint8_t *)ctmp->mem + WT_BLOCK_COMPRESS_SKIP;
        dst_len = len;

        compression_failed = 0;
        WT_ERR(btree->compressor->compress(btree->compressor, &session->iface, src, src_len, dst,
          dst_len, &result_len, &compression_failed));
        result_len += WT_BLOCK_COMPRESS_SKIP;

        /*
         * If compression fails, or doesn't gain us at least one unit of allocation, fallback to the
         * original version. This isn't unexpected: if compression doesn't work for some chunk of
         * data for some reason (noting likely additional format/header information which compressed
         * output requires), it just means the uncompressed version is as good as it gets, and
         * that's what we use.
         */
        if (compression_failed || buf->size / btree->allocsize <= result_len / btree->allocsize) {
            ip = buf;
            WT_STAT_DATA_INCR(session, compress_write_fail);
        } else {
            compressed = true;
            WT_STAT_DATA_INCR(session, compress_write);

            /* Copy in the skipped header bytes and set the final data size. */
            memcpy(ctmp->mem, buf->mem, WT_BLOCK_COMPRESS_SKIP);
            ctmp->size = result_len;
            ip = ctmp;

            /* Set the disk header flags. */
            dsk = ip->mem;
            F_SET(dsk, WT_PAGE_COMPRESSED);

            /* Optionally return the compressed size. */
            if (compressed_sizep != NULL)
                *compressed_sizep = result_len;
        }
    }

    /*
     * Optionally encrypt the data. We need to add in the original length, in case both compression
     * and encryption are done.
     */
    if ((kencryptor = btree->kencryptor) != NULL) {
        /*
         * Get size needed for encrypted buffer.
         */
        __wt_encrypt_size(session, kencryptor, ip->size, &size);

        WT_ERR(bm->write_size(bm, session, &size));
        WT_ERR(__wt_scr_alloc(session, size, &etmp));
        WT_ERR(__wt_encrypt(session, kencryptor, WT_BLOCK_ENCRYPT_SKIP, ip, etmp));

        encrypted = true;
        ip = etmp;

        /* Set the disk header flags. */
        dsk = ip->mem;
        if (compressed)
            F_SET(dsk, WT_PAGE_COMPRESSED);
        F_SET(dsk, WT_PAGE_ENCRYPTED);
    }

    /* Determine if the data requires a checksum. */
    data_checksum = true;
    switch (btree->checksum) {
    case CKSUM_ON:
        /* Set outside the switch to avoid compiler and analyzer complaints. */
        break;
    case CKSUM_OFF:
        data_checksum = false;
        break;
    case CKSUM_UNCOMPRESSED:
        data_checksum = !compressed;
        break;
    case CKSUM_UNENCRYPTED:
        data_checksum = !encrypted;
        break;
    }

    /* Call the block manager to write the block. */
    timer = WT_STAT_ENABLED(session) && !F_ISSET(session, WT_SESSION_INTERNAL);
    time_start = timer ? __wt_clock(session) : 0;
    WT_ERR(checkpoint ? bm->checkpoint(bm, session, ip, btree->ckpt, data_checksum) :
                        bm->write(bm, session, ip, addr, addr_sizep, data_checksum, checkpoint_io));
    if (timer) {
        time_stop = __wt_clock(session);
        time_diff = WT_CLOCKDIFF_US(time_stop, time_start);
        WT_STAT_CONN_INCR(session, cache_write_app_count);
        WT_STAT_CONN_INCRV(session, cache_write_app_time, time_diff);
        WT_STAT_SESSION_INCRV(session, write_time, time_diff);
    }

    /*
     * The page image must have a proper write generation number before writing it to disk. The page
     * images that are created during recovery may have the write generation number less than the
     * btree base write generation number, so don't verify it.
     */
    dsk = ip->mem;
    WT_ASSERT(session, dsk->write_gen != 0);

    WT_STAT_CONN_DATA_INCR(session, cache_write);
    WT_STAT_CONN_DATA_INCRV(session, cache_bytes_write, dsk->mem_size);
    WT_STAT_SESSION_INCRV(session, bytes_write, dsk->mem_size);
    (void)__wt_atomic_add64(&S2C(session)->cache->bytes_written, dsk->mem_size);

    /*
     * Store a copy of the compressed buffer in the block cache.
     *
     * Optional if the write is part of a checkpoint. Hot blocks get written and over-written a lot
     * as part of checkpoint, so we don't want to cache them, because (a) they are in the in-memory
     * cache anyway, and (b) they are likely to be overwritten again in the next checkpoint. Writes
     * that are not part of checkpoint I/O are done in the service of eviction. Those are the blocks
     * that the in-memory cache would like to keep but can't, and we definitely want to keep them.
     *
     * Optional on normal writes (vs. reads) if the no-write-allocate setting is on.
     *
     * Ignore the final checkpoint writes.
     */
    if (blkcache->type == WT_BLKCACHE_UNCONFIGURED)
        ;
    else if (!blkcache->cache_on_checkpoint && checkpoint_io)
        WT_STAT_CONN_INCR(session, block_cache_bypass_chkpt);
    else if (!blkcache->cache_on_writes)
        WT_STAT_CONN_INCR(session, block_cache_bypass_writealloc);
    else if (!checkpoint)
        WT_ERR(__wt_blkcache_put(session, compressed ? ctmp : buf, addr, *addr_sizep, true));

err:
    __wt_scr_free(session, &ctmp);
    __wt_scr_free(session, &etmp);
    return (ret);
}
