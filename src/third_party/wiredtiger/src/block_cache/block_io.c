/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * Define functions that increment histogram statistics compression ratios for block reads and block
 * writes.
 */
WT_STAT_COMPR_RATIO_READ_HIST_INCR_FUNC(ratio)
WT_STAT_COMPR_RATIO_WRITE_HIST_INCR_FUNC(ratio)

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

    F_SET_ATOMIC_32(S2C(session), WT_CONN_DATA_CORRUPTION);
    if (!F_ISSET(btree, WT_BTREE_VERIFY) && !F_ISSET(session, WT_SESSION_QUIET_CORRUPT_FILE)) {
        WT_TRET(bm->corrupt(bm, session, addr, addr_size));
        WT_RET_PANIC(session, ret, "%s: fatal read error: %s", btree->dhandle->name, fail_msg);
    }
    return (ret);
}

/*
 * __blkcache_read_decrypt --
 *     Decrypt the content of one item into another.
 *
 * This uses the decryptor on the btree, and requires that the output item is already backed by a
 *     scratch buffer that can be grown as needed.
 */
static int
__blkcache_read_decrypt(
  WT_SESSION_IMPL *session, WT_ITEM *in, WT_ITEM *out, const uint8_t *addr, size_t addr_size)
{
    WT_BM *bm;
    WT_BTREE *btree;
    WT_DECL_RET;
    WT_ENCRYPTOR *encryptor;

    btree = S2BT(session);
    bm = btree->bm;
    encryptor = btree->kencryptor == NULL ? NULL : btree->kencryptor->encryptor;

    if (encryptor == NULL || encryptor->decrypt == NULL)
        WT_RET(__blkcache_read_corrupt(
          session, WT_ERROR, addr, addr_size, "encrypted block for which no decryptor configured"));

    if ((ret = __wt_decrypt(session, encryptor, bm->encrypt_skip(bm, session), in, out)) != 0)
        WT_RET(__blkcache_read_corrupt(session, ret, addr, addr_size, "block decryption failed"));

    /* Clear the ENCRYPTED flag. */
    F_CLR(((WT_PAGE_HEADER *)out->data), WT_PAGE_ENCRYPTED);

    return (0);
}

/*
 * __blkcache_cache_wants_encrypted_data --
 *     Return if the configured block cache wants to store encrypted blocks.
 */
static bool
__blkcache_cache_wants_encrypted_data(WT_SESSION_IMPL *session)
{
    /*!!!
     * Guidance for adding new block cache types here:
     *  - If cache only stores pages in RAM, we save CPU by storing unencrypted pages.
     *  - If cache can store pages on external media, we store encrypted pages if encryption is
     * configured.
     */
    u_int type = S2C(session)->blkcache.type;
    return (type == WT_BLKCACHE_NVRAM);
}

/*
 * __wt_blkcache_read --
 *     Read an address-cookie referenced block into a buffer.
 */
int
__wt_blkcache_read(WT_SESSION_IMPL *session, WT_ITEM *buf, WT_PAGE_BLOCK_META *block_meta,
  const uint8_t *addr, size_t addr_size)
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
    WT_ITEM *ip, *ip_orig;
    WT_ITEM results[WT_DELTA_LIMIT + 1];
    WT_PAGE_BLOCK_META block_meta_tmp;
    const WT_PAGE_HEADER *dsk;
    size_t compression_ratio, result_len;
    uint64_t time_diff, time_start, time_stop;
    u_int count, i, results_count;
    bool blkcache_found, expect_conversion, found, skip_cache_put, timer;

    blkcache = &S2C(session)->blkcache;
    blkcache_item = NULL;
    btree = S2BT(session);
    bm = btree->bm;
    compressor = btree->compressor;
    encryptor = btree->kencryptor == NULL ? NULL : btree->kencryptor->encryptor;
    blkcache_found = found = false;
    skip_cache_put = (blkcache->type == WT_BLKCACHE_UNCONFIGURED);
    results_count = 0;

    WT_ASSERT_ALWAYS(session, session->dhandle != NULL, "The block cache requires a dhandle");
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
    WT_RET(__wti_blkcache_map_read(session, ip, addr, addr_size, &found));
    if (found) {
        skip_cache_put = true;
        if (!expect_conversion)
            goto verify;
    }

    /* Check the block cache. */
    if (!found && blkcache->type != WT_BLKCACHE_UNCONFIGURED) {
        __wti_blkcache_get(session, addr, addr_size, &blkcache_item, &found, &skip_cache_put);
        if (found) {
            blkcache_found = true;
            ip->data = blkcache_item->data;
            ip->size = blkcache_item->data_size;
            if (block_meta != NULL) {
                if (blkcache_item->block_meta == NULL)
                    WT_CLEAR(*block_meta);
                else
                    *block_meta = *blkcache_item->block_meta;
            }
            /* We don't expect to have deltas when using this variant of the read call. */
            WT_ASSERT(session, blkcache_item->num_deltas == 0);
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

        if (bm->read_multiple != NULL) {
            count = WT_ELEMENTS(results);
            WT_ERR(
              bm->read_multiple(bm, session, &block_meta_tmp, addr, addr_size, results, &count));

            /*
             * FIXME-WT-14608: we're choosing not to handle deltas here, but that's not going to
             * work longer-term.
             */
            WT_ASSERT(session, count == 1);
            results_count = count;
            ip = &results[0];
        } else
            WT_ERR(bm->read(bm, session, ip, &block_meta_tmp, addr, addr_size));
        if (timer) {
            time_stop = __wt_clock(session);
            time_diff = WT_CLOCKDIFF_US(time_stop, time_start);
            WT_STAT_CONN_INCR(session, cache_read_app_count);
            WT_STAT_CONN_INCRV(session, cache_read_app_time, time_diff);
            WT_STAT_SESSION_INCRV(session, read_time, time_diff);
        }

        if (block_meta != NULL)
            *block_meta = block_meta_tmp;

        dsk = ip->data;

        /*
         * Disallow reading an unencrypted block from original source when encryption is configured.
         */
        if (!F_ISSET(dsk, WT_PAGE_ENCRYPTED) && btree->kencryptor != NULL)
            WT_ERR(__blkcache_read_corrupt(session, WT_ERROR, addr, addr_size,
              "read unencrypted block for which encryption configured"));

        /*
         * Increment statistics before we do anymore processing such as decompression or decryption
         * on the data.
         */
        if (dsk->type == WT_PAGE_COL_INT || dsk->type == WT_PAGE_ROW_INT)
            WT_STAT_CONN_INCRV(session, block_byte_read_intl_disk, ip->size);
        else
            WT_STAT_CONN_INCRV(session, block_byte_read_leaf_disk, ip->size);

        WT_STAT_CONN_DSRC_INCR(session, cache_read);
        if (WT_SESSION_IS_CHECKPOINT(session))
            WT_STAT_CONN_DSRC_INCR(session, cache_read_checkpoint);
        if (F_ISSET(dsk, WT_PAGE_COMPRESSED))
            WT_STAT_DSRC_INCR(session, compress_read);
        WT_STAT_CONN_DSRC_INCRV(session, cache_bytes_read, dsk->mem_size);
        WT_STAT_SESSION_INCRV(session, bytes_read, dsk->mem_size);
        (void)__wt_atomic_add64(&S2C(session)->cache->bytes_read, dsk->mem_size);
    }

    /*
     * If the block is encrypted, copy the skipped bytes of the image into place, then decrypt. DRAM
     * block-cache blocks are never encrypted.
     */
    dsk = ip->data;
    ip_orig = ip;
    if (F_ISSET(dsk, WT_PAGE_ENCRYPTED)) {
        WT_ERR(__wt_scr_alloc(session, 0, &etmp));
        WT_ERR(__blkcache_read_decrypt(session, ip, etmp, addr, addr_size));
        ip = etmp;
    }

    /*
     * Ignore the cache if we have deltas. We don't expect to have deltas in this type of read call
     * anyways.
     */
    if (results_count > 1)
        skip_cache_put = true;

    if (!skip_cache_put) {
        /* Choose either the encrypted or decrypted data for the cache. */
        WT_ITEM *cache_item = __blkcache_cache_wants_encrypted_data(session) ? ip_orig : ip;
        /* Use a local variable for block metadata, because the passed-in pointer could be NULL. */
        WT_ERR(__wti_blkcache_put(
          session, cache_item, NULL, 0, &block_meta_tmp, addr, addr_size, false));
    }

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
        __wt_stat_compr_ratio_read_hist_incr(session, compression_ratio);

    } else {
        /*
         * If we uncompressed above, the page is in the correct buffer. If we get here the data may
         * be in the wrong buffer and the buffer may be the wrong size. If needed, get the page into
         * the destination buffer.
         */
        if (ip != buf)
            WT_ERR(__wt_buf_set(session, buf, ip->data, dsk->mem_size));
    }

    /*
     * These statistics are increased when the block is not found in the block cache and we need to
     * read from disk.
     */
    if (dsk != NULL) {
        if (dsk->type == WT_PAGE_COL_INT || dsk->type == WT_PAGE_ROW_INT)
            WT_STAT_CONN_INCRV(session, block_byte_read_intl, dsk->mem_size);
        else
            WT_STAT_CONN_INCRV(session, block_byte_read_leaf, dsk->mem_size);
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

    /* Free the temporary buffers allocated for disagg. */
    for (i = 0; i < results_count; i++)
        __wt_buf_free(session, &results[i]);

    __wt_scr_free(session, &tmp);
    __wt_scr_free(session, &etmp);
    return (ret);
}

/*
 * __read_decompress --
 *     Decompress data into a WT_ITEM.
 *
 * This uses the decompressor on the btree, and does not require that the output item is already
 *     allocated. The caller is responsible for freeing the output buffer.
 */
static int
__read_decompress(WT_SESSION_IMPL *session, const void *in, size_t mem_sz, WT_ITEM *out,
  const uint8_t *addr, size_t addr_size)
{
    WT_BTREE *btree;
    WT_COMPRESSOR *compressor;
    WT_DECL_RET;
    size_t compression_ratio, result_len;

    btree = S2BT(session);
    compressor = btree->compressor;

    if (compressor == NULL || compressor->decompress == NULL)
        WT_RET(__blkcache_read_corrupt(session, WT_ERROR, addr, addr_size,
          "compressed block for which no compression configured"));

    WT_RET(__wt_buf_initsize(session, out, mem_sz));

    memcpy(out->mem, in, WT_BLOCK_COMPRESS_SKIP);

    /*
     * FIXME-WT-14716 Stop casting away the const. The compressor interface marks it as non-const.
     */
    ret =
      compressor->decompress(compressor, &session->iface, (uint8_t *)in + WT_BLOCK_COMPRESS_SKIP,
        mem_sz - WT_BLOCK_COMPRESS_SKIP, (uint8_t *)out->mem + WT_BLOCK_COMPRESS_SKIP,
        out->memsize - WT_BLOCK_COMPRESS_SKIP, &result_len);
    if (result_len != mem_sz - WT_BLOCK_COMPRESS_SKIP)
        WT_TRET(WT_ERROR);

    if (ret != 0)
        WT_ERR(
          __blkcache_read_corrupt(session, ret, addr, addr_size, "block decompression failed"));

    compression_ratio = result_len / (out->size - WT_BLOCK_COMPRESS_SKIP);
    __wt_stat_compr_ratio_read_hist_incr(session, compression_ratio);

    if (0) {
err:
        __wt_buf_free(session, out);
    }
    return (ret);
}

/*
 * __wt_blkcache_read_multi --
 *     Read an address-cookie referenced block with its deltas into a set of buffers.
 */
int
__wt_blkcache_read_multi(WT_SESSION_IMPL *session, WT_ITEM **buf, size_t *buf_count,
  WT_PAGE_BLOCK_META *block_meta, const uint8_t *addr, size_t addr_size)
{
    WT_BLKCACHE *blkcache;
    WT_BLKCACHE_ITEM *blkcache_item;
    WT_BLOCK_DISAGG_HEADER *blk;
    WT_BM *bm;
    WT_BTREE *btree;
    WT_DECL_ITEM(ctmp);
    WT_DECL_ITEM(etmp);
    WT_DECL_RET;
    WT_ITEM results[WT_DELTA_LIMIT + 1];
    WT_ITEM *tmp, *ip, *ip_orig;
    WT_PAGE_BLOCK_META block_meta_tmp;
    const WT_PAGE_HEADER *dsk;
    uint32_t count, i;
    uint8_t type;
    bool blkcache_found, found, skip_cache_put;

    WT_CLEAR(block_meta_tmp);
    WT_CLEAR(results);

    blkcache = &S2C(session)->blkcache;
    blkcache_found = false;
    blkcache_item = NULL;
    btree = S2BT(session);
    bm = btree->bm;
    dsk = NULL;
    found = false;
    ip = NULL;
    skip_cache_put = (blkcache->type == WT_BLKCACHE_UNCONFIGURED);
    tmp = NULL;
    type = 0;

    /* Skip block cache for M2, just read the base + delta pack. */
    count = WT_ELEMENTS(results);

    /* TODO clean up tmp usage? */
    if (bm->read_multiple == NULL) {
        WT_RET(__wt_calloc_def(session, 1, &tmp));
        WT_CLEAR(tmp[0]);
        /*
         * FIXME-WT-14717: we used to read garbage values for block meta from the block cache for
         * non-disaggregated case. It's unclear if we still do -- pass a NULL for now.
         */
        WT_ERR(__wt_blkcache_read(session, &tmp[0], NULL, addr, addr_size));
        *buf_count = 1;
        *buf = tmp;
        return (0);
    }

    /* Check the block cache. */
    if (blkcache->type != WT_BLKCACHE_UNCONFIGURED) {
        __wti_blkcache_get(session, addr, addr_size, &blkcache_item, &found, &skip_cache_put);
        if (found) {
            blkcache_found = true;
            WT_ASSERT_ALWAYS(session, blkcache_item->num_deltas <= WT_DELTA_LIMIT,
              "block cache item has too many deltas");
            results[0].data = blkcache_item->data;
            results[0].size = blkcache_item->data_size;
            for (i = 0; i < blkcache_item->num_deltas; i++) {
                results[i + 1].data = blkcache_item->deltas[i].data;
                results[i + 1].size = blkcache_item->deltas[i].data_size;
            }
            count = blkcache_item->num_deltas + 1;
            if (blkcache_item->block_meta != NULL)
                block_meta_tmp = *blkcache_item->block_meta;

            ip = &results[0];
            dsk = ip->data;
            type = dsk->type;
        }
    }

    if (!found) {
        WT_ERR(
          bm->read_multiple(bm, session, &block_meta_tmp, addr, addr_size, &results[0], &count));
        WT_ASSERT(session, count > 0);
        found = true;

        /*
         * For the base image, we have a structure like this:
         *
         * ------------------------
         * | page header          |
         * ------------------------
         * | block header         |
         * ------------------------
         * | data                 |
         * ------------------------
         *
         * In this case, the encryption/compression flags live in the page header.
         */
        ip = &results[0];
        dsk = ip->data;
        type = dsk->type;

        /*
         * Disallow reading an unencrypted block from original source when encryption is configured.
         */
        if (!F_ISSET(dsk, WT_PAGE_ENCRYPTED) && btree->kencryptor != NULL)
            WT_ERR(__blkcache_read_corrupt(session, WT_ERROR, addr, addr_size,
              "multi_read unencrypted block for which encryption configured"));

        /*
         * Increment statistics before we do any more processing such as decompression or decryption
         * on the base image.
         */
        if (type == WT_PAGE_COL_INT || type == WT_PAGE_ROW_INT)
            WT_STAT_CONN_INCRV(session, block_byte_read_intl_disk, ip->size);
        else
            WT_STAT_CONN_INCRV(session, block_byte_read_leaf_disk, ip->size);

        WT_STAT_CONN_DSRC_INCR(session, cache_read);
        if (WT_SESSION_IS_CHECKPOINT(session))
            WT_STAT_CONN_DSRC_INCR(session, cache_read_checkpoint);
        if (F_ISSET(dsk, WT_PAGE_COMPRESSED))
            WT_STAT_DSRC_INCR(session, compress_read);

        WT_STAT_CONN_DSRC_INCRV(session, cache_bytes_read, dsk->mem_size);
        WT_STAT_SESSION_INCRV(session, bytes_read, dsk->mem_size);
        (void)__wt_atomic_add64(&S2C(session)->cache->bytes_read, dsk->mem_size);
    }

    /* Decrypt. */
    ip_orig = ip;
    if (F_ISSET(dsk, WT_PAGE_ENCRYPTED)) {
        WT_ERR(__wt_scr_alloc(session, 0, &etmp));
        WT_ERR(__blkcache_read_decrypt(session, ip, etmp, addr, addr_size));
        ip = etmp;
    }

    /* Store the compressed block in the block_cache. */
    if (!skip_cache_put) {
        WT_ITEM *cache_item = __blkcache_cache_wants_encrypted_data(session) ? ip_orig : ip;
        WT_ERR(__wti_blkcache_put(
          session, cache_item, &results[1], count - 1, &block_meta_tmp, addr, addr_size, false));
    }

    /*
     * It might be possible to get a cleaner handover between the decryption and decompression
     * sections, possibly without a second item for the decompression. But that's a problem for
     * later.
     */
    dsk = ip->data;
    if (F_ISSET(dsk, WT_PAGE_COMPRESSED)) {
        WT_ERR(__wt_scr_alloc(session, 0, &ctmp));
        WT_ERR(__read_decompress(session, dsk, dsk->mem_size, ctmp, addr, addr_size));
        ip = ctmp;
    }
    if (ip != &results[0]) {
        __wt_buf_free(session, &results[0]);
        WT_ITEM_MOVE(results[0], *ip);
    }
    if (etmp != NULL && WT_DATA_IN_ITEM(etmp))
        __wt_scr_free(session, &etmp);

    if (type == WT_PAGE_COL_INT || type == WT_PAGE_ROW_INT)
        WT_STAT_CONN_INCRV(session, block_byte_read_intl, ip->size);
    else
        WT_STAT_CONN_INCRV(session, block_byte_read_leaf, ip->size);

    /*
     * Now do deltas. Here, the structure looks like:
     *
     * ------------------------
     * | page header          |
     * ------------------------
     * | block header         |
     * ------------------------
     * | data                 |
     * ------------------------
     *
     * In this case, the block header is what contains the encryption/compression
     * flags so we need to skip over the page header for the delta. TODO if the block header can
     * be moved in front of the page header, then we can get rid of the block
     * manager's encrypt_skip function.
     */
    for (i = 1; i < count; i++) {
        ip = &results[i];

        blk = WT_BLOCK_HEADER_REF(results[i].data);

        /*
         * For each delta, increment statistics before we do any more processing such as
         * decompression or decryption.
         */
        if (type == WT_PAGE_COL_INT || type == WT_PAGE_ROW_INT)
            WT_STAT_CONN_INCRV(session, block_byte_read_intl_disk, ip->size);
        else
            WT_STAT_CONN_INCRV(session, block_byte_read_leaf_disk, ip->size);

        if (F_ISSET(blk, WT_BLOCK_DISAGG_ENCRYPTED)) {
            WT_ERR(__wt_scr_alloc(session, 0, &etmp));
            WT_ERR(__blkcache_read_decrypt(session, ip, etmp, addr, addr_size));
            ip = etmp;
        }
        if (F_ISSET(blk, WT_BLOCK_DISAGG_COMPRESSED)) {
            dsk = ip->data;
            WT_ERR(__wt_scr_alloc(session, 0, &ctmp));
            WT_ERR(__read_decompress(session, ip->data, dsk->mem_size, ctmp, addr, addr_size));
            ip = ctmp;
        }
        if (ip != &results[i]) {
            __wt_buf_free(session, &results[i]);
            WT_ITEM_MOVE(results[i], *ip);
        }
        if (etmp != NULL && WT_DATA_IN_ITEM(etmp))
            __wt_scr_free(session, &etmp);

        if (type == WT_PAGE_COL_INT || type == WT_PAGE_ROW_INT)
            WT_STAT_CONN_INCRV(session, block_byte_read_intl, ip->size);
        else
            WT_STAT_CONN_INCRV(session, block_byte_read_leaf, ip->size);
    }

    /* Finalize our return list. */
    WT_ERR(__wt_calloc_def(session, count, &tmp));
    for (i = 0; i < count; i++)
        memcpy(&tmp[i], &results[i], sizeof(WT_ITEM));
    *buf = tmp;
    *buf_count = count;

    if (block_meta != NULL)
        *block_meta = block_meta_tmp;

    if (0) {
err:
        __wt_free(session, tmp);
        __wt_scr_free(session, &etmp);
        __wt_scr_free(session, &ctmp);
    }

    /* If we pulled the block from the block cache, decrement its reference count. */
    if (blkcache_found)
        (void)__wt_atomic_subv32(&blkcache_item->ref_count, 1);

    return (ret);
}

/*
 * __wt_blkcache_write --
 *     Write a buffer into a block, returning the block's address cookie.
 */
int
__wt_blkcache_write(WT_SESSION_IMPL *session, WT_ITEM *buf, WT_PAGE_BLOCK_META *block_meta,
  uint8_t *addr, size_t *addr_sizep, size_t *compressed_sizep, bool checkpoint, bool checkpoint_io,
  bool compressed)
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
    size_t compression_ratio, dst_len, len, result_len, size, src_len;
    uint64_t time_diff, time_start, time_stop;
    uint32_t delta_count, mem_size;
    uint8_t *dst, *src;
    int compression_failed; /* Extension API, so not a bool. */
    bool data_checksum, encrypted, timer;

    if (compressed_sizep != NULL)
        *compressed_sizep = 0;

    blkcache = &S2C(session)->blkcache;
    btree = S2BT(session);
    bm = btree->bm;
    delta_count = (block_meta == NULL) ? 0 : block_meta->delta_count;
    dsk = NULL;
    encrypted = false;

    /*
     * Optionally stream-compress the data, but don't compress blocks that are already as small as
     * they're going to get.
     */
    if (btree->compressor == NULL || btree->compressor->compress == NULL || compressed)
        ip = buf;
    else if (buf->size <= btree->allocsize) {
        ip = buf;
        WT_STAT_DSRC_INCR(session, compress_write_too_small);
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
            WT_STAT_DSRC_INCR(session, compress_write_fail);
        } else {
            compressed = true;
            WT_STAT_DSRC_INCR(session, compress_write);

            compression_ratio = src_len / (result_len - WT_BLOCK_COMPRESS_SKIP);
            __wt_stat_compr_ratio_write_hist_incr(session, compression_ratio);

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
        WT_ASSERT(session, ip->size > 0);
        WT_ERR(__wt_encrypt(session, kencryptor, bm->encrypt_skip(bm, session), ip, etmp));

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
    WT_ERR(checkpoint ?
        bm->checkpoint(bm, session, ip, block_meta, btree->ckpt, data_checksum) :
        bm->write(bm, session, ip, block_meta, addr, addr_sizep, data_checksum, checkpoint_io));
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
    mem_size = dsk->mem_size;

    WT_STAT_CONN_DSRC_INCR(session, cache_write);
    WT_STAT_CONN_DSRC_INCRV(session, cache_bytes_write, mem_size);
    WT_STAT_SESSION_INCRV(session, bytes_write, mem_size);
    (void)__wt_atomic_add64(&S2C(session)->cache->bytes_written, mem_size);

    if (dsk != NULL) {
        if (dsk->type == WT_PAGE_COL_INT || dsk->type == WT_PAGE_ROW_INT) {
            WT_STAT_CONN_INCRV(session, block_byte_write_intl, dsk->mem_size);
            WT_STAT_CONN_INCRV(session, block_byte_write_intl_disk, ip->size);
        } else {
            WT_STAT_CONN_INCRV(session, block_byte_write_leaf, dsk->mem_size);
            WT_STAT_CONN_INCRV(session, block_byte_write_leaf_disk, ip->size);
        }
    }

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
     *
     * TODO: ignore block cache for deltas now.
     */
    if (blkcache->type == WT_BLKCACHE_UNCONFIGURED || (block_meta != NULL && delta_count > 0))
        ;
    else if (!blkcache->cache_on_checkpoint && checkpoint_io)
        WT_STAT_CONN_INCR(session, block_cache_bypass_chkpt);
    else if (!blkcache->cache_on_writes)
        WT_STAT_CONN_INCR(session, block_cache_bypass_writealloc);
    else if (!checkpoint) {
        /* If we are here, it means that we don't have deltas, so let's just ignore them. */
        WT_ITEM *cache_item = __blkcache_cache_wants_encrypted_data(session) ? ip :
          compressed                                                         ? ctmp :
                                                                               buf;
        WT_ERR(
          __wti_blkcache_put(session, cache_item, NULL, 0, block_meta, addr, *addr_sizep, true));
    }

err:
    __wt_scr_free(session, &ctmp);
    __wt_scr_free(session, &etmp);
    return (ret);
}
