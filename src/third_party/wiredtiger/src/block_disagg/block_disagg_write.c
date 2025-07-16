/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __block_disagg_header_byteswap --
 *     Place holder - it might be necessary to swap things into network byte order.
 */
static void
__block_disagg_header_byteswap(WT_BLOCK_DISAGG_HEADER *blk)
{
    WT_UNUSED(blk);
}

/*
 * __wti_block_disagg_header_byteswap_copy --
 *     Place holder - might be necessary to handle network order.
 */
void
__wti_block_disagg_header_byteswap_copy(WT_BLOCK_DISAGG_HEADER *from, WT_BLOCK_DISAGG_HEADER *to)
{
    *to = *from;
}

/*
 * __wti_block_disagg_write_size --
 *     Return the buffer size required to write a block.
 */
int
__wti_block_disagg_write_size(size_t *sizep)
{
    /*
     * We write the page size, in bytes, into the block's header as a 4B unsigned value, and it's
     * possible for the engine to accept an item we can't write. For example, a huge key/value where
     * the allocation size has been set to something large will overflow 4B when it tries to align
     * the write. We could make this work, but it's not worth the effort, writing 4GB objects into a
     * btree makes no sense. Limit the writes to (4GB - 1KB), it gives us potential mode bits, and
     * I'm not interested in debugging corner cases anyway.
     *
     * For disaggregated storage, we use the maximum header size, since we have multiple kinds of
     * header and we don't know which one this is. Since the caller is invariably using the result
     * to size a buffer, we may cause a little bit of waste (for deltas), which should not be a
     * problem.
     */
    *sizep = (size_t)(*sizep +
      WT_MAX(WT_BLOCK_DISAGG_BASE_HEADER_BYTE_SIZE, WT_BLOCK_DISAGG_DELTA_HEADER_BYTE_SIZE));
    return (*sizep > UINT32_MAX - 1024 ? EINVAL : 0);
}

/*
 * __wti_block_disagg_write_internal --
 *     Write a buffer into a block, returning the block's id, size, checksum, and the new block
 *     metadata for the page. Note that the current and the new block page metadata pointers could
 *     be the same.
 */
int
__wti_block_disagg_write_internal(WT_SESSION_IMPL *session, WT_BLOCK_DISAGG *block_disagg,
  WT_ITEM *buf, WT_PAGE_BLOCK_META *block_meta, uint32_t *sizep, uint32_t *checksump,
  bool data_checksum, bool checkpoint_io)
{
    WT_BLOCK_DISAGG_HEADER *blk;
    WT_CONNECTION_IMPL *conn;
    WT_DELTA_HEADER *delta_header;
    WT_PAGE_HEADER *page_header;
    WT_PAGE_LOG_HANDLE *plhandle;
    WT_PAGE_LOG_PUT_ARGS put_args;
    uint64_t checkpoint_id, page_id, page_log_checkpoint_id, time_start, time_stop;
    uint32_t checksum;
    bool is_delta;

    time_start = __wt_clock(session);

    WT_ASSERT(session, block_meta != NULL);
    WT_ASSERT(session, block_meta->page_id >= WT_BLOCK_MIN_PAGE_ID);

    conn = S2C(session);

    *sizep = 0;     /* -Werror=maybe-uninitialized */
    *checksump = 0; /* -Werror=maybe-uninitialized */

    plhandle = block_disagg->plhandle;
    WT_CLEAR(put_args);
    is_delta = (block_meta->delta_count != 0);

    WT_ASSERT_ALWAYS(session, plhandle != NULL, "Disaggregated block store requires page log");

    /*
     * Clear the block header to ensure all of it is initialized, even the unused fields.
     */
    if (is_delta)
        blk = WT_BLOCK_HEADER_REF_FOR_DELTAS(buf->mem);
    else
        blk = WT_BLOCK_HEADER_REF(buf->mem);
    memset(blk, 0, sizeof(*blk));

    if (buf->size > UINT32_MAX) {
        WT_ASSERT(session, buf->size <= UINT32_MAX);
        WT_RET_MSG(session, EINVAL, "buffer size check: write buffer too large to write");
    }

    /* Get the page ID. */
    page_id = block_meta->page_id;
    /* Get the checkpoint ID. */
    WT_ACQUIRE_READ(checkpoint_id, conn->disaggregated_storage.global_checkpoint_id);
    WT_ASSERT_ALWAYS(session, checkpoint_id == block_meta->checkpoint_id,
      "The page checkpoint id doesn't match the current checkpoint id");
    /* Check that we are the leader (only leaders can write). */
    WT_ASSERT_ALWAYS(
      session, conn->layered_table_manager.leader, "Trying to write the page from a follower");
    /* Check that the checkpoint ID matches the current checkpoint in the page log. */
    if (block_disagg->plhandle->page_log != NULL &&
      block_disagg->plhandle->page_log->pl_get_open_checkpoint != NULL) {
        WT_RET(block_disagg->plhandle->page_log->pl_get_open_checkpoint(
          block_disagg->plhandle->page_log, &session->iface, &page_log_checkpoint_id));
        WT_ASSERT_ALWAYS(session, checkpoint_id == page_log_checkpoint_id,
          "The global checkpoint ID does not match the opened checkpoint in the page log");
    }

    /*
     * Update the block's checksum: if our caller specifies, checksum the complete data, otherwise
     * checksum the leading WT_BLOCK_COMPRESS_SKIP bytes. The assumption is applications with good
     * compression support turn off checksums and assume corrupted blocks won't decompress
     * correctly. However, if compression failed to shrink the block, the block wasn't compressed,
     * in which case our caller will tell us to checksum the data to detect corruption. If
     * compression succeeded, we still need to checksum the first WT_BLOCK_COMPRESS_SKIP bytes
     * because they're not compressed, both to give salvage a quick test of whether a block is
     * useful and to give us a test so we don't lose the first WT_BLOCK_COMPRESS_SKIP bytes without
     * noticing.
     *
     * Checksum a little-endian version of the header, and write everything in little-endian format.
     * The checksum is (potentially) returned in a big-endian format, swap it into place in a
     * separate step.
     */
    blk->flags = 0;
    if (data_checksum)
        F_SET(blk, WT_BLOCK_DISAGG_DATA_CKSUM);

    /*
     * FIXME-WT-14613: temporary measure until we put the block header at the beginning of the data.
     * We have two sets of flags for encrypt/compress! Set the block manager encrypt/compress flags
     * - the block manager/block cache layer will eventually do all encrypt/compress and it will use
     * a unified set of flags for encrypt/compress, (only in the block header). But we can only do
     * that when the block header is always at the beginning of the data.
     */
    if (!is_delta) {
        page_header = (WT_PAGE_HEADER *)buf->mem;
        if (F_ISSET(page_header, WT_PAGE_COMPRESSED))
            F_SET(blk, WT_BLOCK_DISAGG_COMPRESSED);
        if (F_ISSET(page_header, WT_PAGE_ENCRYPTED))
            F_SET(blk, WT_BLOCK_DISAGG_ENCRYPTED);
    } else {
        delta_header = (WT_DELTA_HEADER *)buf->mem;
        if (F_ISSET(delta_header, WT_PAGE_COMPRESSED))
            F_SET(blk, WT_BLOCK_DISAGG_COMPRESSED);
        if (F_ISSET(delta_header, WT_PAGE_ENCRYPTED))
            F_SET(blk, WT_BLOCK_DISAGG_ENCRYPTED);
    }

    if (block_meta->delta_count == 0) {
        blk->magic = WT_BLOCK_DISAGG_MAGIC_BASE;
        blk->header_size = WT_BLOCK_DISAGG_BASE_HEADER_BYTE_SIZE;
    } else {
        blk->magic = WT_BLOCK_DISAGG_MAGIC_DELTA;
        blk->header_size = WT_BLOCK_DISAGG_DELTA_HEADER_BYTE_SIZE;
        F_SET(&put_args, WT_PAGE_LOG_DELTA);
    }
    blk->version = WT_BLOCK_DISAGG_VERSION;
    blk->compatible_version = WT_BLOCK_DISAGG_COMPATIBLE_VERSION;

    /*
     * The reconciliation id stored in the block header is diagnostic, we don't care if it's
     * truncated.
     */
    blk->reconciliation_id =
      (uint8_t)WT_MIN(block_meta->reconciliation_id, WT_BLOCK_OVERFLOW_RECONCILIATION_ID);
    blk->previous_checksum = block_meta->checksum;
    blk->checksum = 0;
    __block_disagg_header_byteswap(blk);
    blk->checksum = checksum =
      __wt_checksum(buf->mem, data_checksum ? buf->size : WT_BLOCK_COMPRESS_SKIP);

    put_args.backlink_lsn = block_meta->backlink_lsn;
    put_args.base_lsn = block_meta->base_lsn;
    put_args.backlink_checkpoint_id = block_meta->backlink_checkpoint_id;
    put_args.base_checkpoint_id = block_meta->base_checkpoint_id;
    put_args.encryption = block_meta->encryption;

    if (F_ISSET(blk, WT_BLOCK_DISAGG_COMPRESSED))
        F_SET(&put_args, WT_PAGE_LOG_COMPRESSED);
    if (F_ISSET(blk, WT_BLOCK_DISAGG_ENCRYPTED))
        F_SET(&put_args, WT_PAGE_LOG_ENCRYPTED);

    /* Write the block. */
    WT_RET(plhandle->plh_put(plhandle, &session->iface, page_id, checkpoint_id, &put_args, buf));

    WT_STAT_CONN_INCR(session, disagg_block_put);
    WT_STAT_CONN_INCR(session, block_write);
    WT_STAT_CONN_INCRV(session, block_byte_write, buf->size);
    if (F_ISSET(block_disagg, WT_BLOCK_DISAGG_HS)) {
        WT_STAT_CONN_INCR(session, disagg_block_hs_put);
        WT_STAT_CONN_INCRV(session, disagg_block_hs_byte_write, buf->size);
    }
    if (checkpoint_io)
        WT_STAT_CONN_INCRV(session, block_byte_write_checkpoint, buf->size);
    time_stop = __wt_clock(session);
    __wt_stat_usecs_hist_incr_disaggbmwrite(session, WT_CLOCKDIFF_US(time_stop, time_start));

    __wt_verbose(session, WT_VERB_WRITE,
      "page_id %" PRIuMAX ", size %" PRIuMAX ", checksum %" PRIx32 ", lsn %" PRIu64,
      (uintmax_t)page_id, (uintmax_t)buf->size, checksum, put_args.lsn);

    /* Some extra data is set by the put interface, and must be returned up the chain. */
    block_meta->disagg_lsn = put_args.lsn;
    WT_ASSERT(session, put_args.lsn > 0);
    block_meta->checksum = checksum;

    *sizep = WT_STORE_SIZE(buf->size);
    *checksump = checksum;

    return (0);
}

/*
 * __wti_block_disagg_write --
 *     Write a buffer into a block, returning the block's address cookie.
 */
int
__wti_block_disagg_write(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_ITEM *buf,
  WT_PAGE_BLOCK_META *block_meta, uint8_t *addr, size_t *addr_sizep, bool data_checksum,
  bool checkpoint_io)
{
    WT_BLOCK_DISAGG *block_disagg;
    uint32_t checksum, size;
    uint8_t *endp;

    /*
     * The data structure needs to be cleaned up, so it can be specialized similarly to how a
     * session has public and private parts. That involves a bunch of mechanical replacement in the
     * existing block manager code, so for now just cheat and specialize inside the disagg block
     * code.
     */
    block_disagg = (WT_BLOCK_DISAGG *)block;
    /*
     * Ensure the page header is in little endian order; this doesn't belong here, but it's the best
     * place to catch all callers. After the write, swap values back to native order so callers
     * never see anything other than their original content.
     */
    if (block_meta->delta_count == 0)
        __wt_page_header_byteswap(buf->mem);
    else
        __wt_delta_header_byteswap(buf->mem);

    WT_RET(__wti_block_disagg_write_internal(
      session, block_disagg, buf, block_meta, &size, &checksum, data_checksum, checkpoint_io));

    if (block_meta->delta_count == 0)
        __wt_page_header_byteswap(buf->mem);
    else
        __wt_delta_header_byteswap(buf->mem);

    endp = addr;
    WT_RET(__wti_block_disagg_addr_pack(&endp, block_meta->page_id, block_meta->disagg_lsn,
      block_meta->checkpoint_id, block_meta->reconciliation_id, size, checksum));
    *addr_sizep = WT_PTRDIFF(endp, addr);

    return (0);
}

/*
 * __wti_block_disagg_page_discard --
 *     Discard a page.
 */
int
__wti_block_disagg_page_discard(
  WT_SESSION_IMPL *session, WT_BLOCK_DISAGG *block_disagg, const uint8_t *addr, size_t addr_size)
{
    /* Crack the cookie. */
    uint64_t checkpoint_id, lsn, page_id, reconciliation_id;
    uint32_t checksum, size;
    WT_RET(__wti_block_disagg_addr_unpack(
      &addr, addr_size, &page_id, &lsn, &checkpoint_id, &reconciliation_id, &size, &checksum));

    __wt_verbose(session, WT_VERB_BLOCK,
      "block free: page_id %" PRIu64 ", lsn %" PRIu64 ", checkpoint_id %" PRIu64
      ", reconciliation_id %" PRIu64 ", size %" PRIu32 ", checksum %" PRIx32,
      page_id, lsn, checkpoint_id, reconciliation_id, size, checksum);

    /* Create the discard request. */
    WT_PAGE_LOG_HANDLE *plhandle = block_disagg->plhandle;

    /* Ignore the call if the function is not implemented. */
    if (plhandle->plh_discard == NULL) {
        __wt_verbose_warning(
          session, WT_VERB_DISAGGREGATED_STORAGE, "%s", "plh_discard is not implemented");
        return (0);
    }

    WT_PAGE_LOG_DISCARD_ARGS discard_args;
    WT_CLEAR(discard_args);

    /* Not used by the page server. */
    discard_args.base_checkpoint_id = 0;
    /* Always 0 for full page. */
    discard_args.base_lsn = 0;

    discard_args.backlink_checkpoint_id = checkpoint_id;
    discard_args.backlink_lsn = lsn;

    WT_STAT_CONN_INCR(session, disagg_block_page_discard);

    return (
      plhandle->plh_discard(plhandle, &session->iface, page_id, checkpoint_id, &discard_args));
}
