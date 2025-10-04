/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *  All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_bm_read --
 *     Map or read address cookie referenced block into a buffer.
 */
int
__wt_bm_read(WT_BM *bm, WT_SESSION_IMPL *session, WT_ITEM *buf, WT_PAGE_BLOCK_META *block_meta,
  const uint8_t *addr, size_t addr_size)
{
    WT_BLOCK *block;
    WT_DECL_RET;
    wt_off_t offset;
    uint32_t checksum, objectid, size;
    bool last_release;

    WT_UNUSED(block_meta);

    block = bm->block;

    /* Crack the cookie. */
    WT_RET(__wt_block_addr_unpack(
      session, block, addr, addr_size, &objectid, &offset, &size, &checksum));

    if (bm->is_multi_handle)
        /* Lookup the block handle */
        WT_RET(__wt_blkcache_get_handle(session, bm, objectid, true, &block));

#ifdef HAVE_DIAGNOSTIC
    /*
     * In diagnostic mode, verify the block we're about to read isn't on the available list, or for
     * the writable objects, the discard list.
     */
    WT_ERR(__wti_block_misplaced(session, block, "read", offset, size,
      bm->is_live && block == bm->block, __PRETTY_FUNCTION__, __LINE__));
#endif

    /* Read the block. */
    WT_ERR(__wti_block_read_off(session, block, buf, objectid, offset, size, checksum));

    /* Optionally discard blocks from the system's buffer cache. */
    WT_ERR(__wti_block_discard(session, block, (size_t)size));

err:
    if (bm->is_multi_handle) {
        last_release = false;
        __wt_blkcache_release_handle(session, block, &last_release);
        if (last_release && __wt_block_eligible_for_sweep(bm, block))
            WT_TRET(__wt_bm_sweep_handles(session, bm));
    }

    return (ret);
}

/*
 * __wt_bm_corrupt_dump --
 *     Dump a block into the log in 1KB chunks.
 */
int
__wt_bm_corrupt_dump(WT_SESSION_IMPL *session, WT_ITEM *buf, uint32_t objectid, wt_off_t offset,
  uint32_t size, uint32_t checksum) WT_GCC_FUNC_ATTRIBUTE((cold))
{
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    size_t chunk, i, nchunks;

#define WT_CORRUPT_FMT "{%" PRIu32 ": %" PRIuMAX ", %" PRIu32 ", %#" PRIx32 "}"
    if (buf->size == 0) {
        __wt_errx(session, WT_CORRUPT_FMT ": empty buffer, no dump available", objectid,
          (uintmax_t)offset, size, checksum);
        return (0);
    }

    WT_RET(__wt_scr_alloc(session, 4 * 1024, &tmp));

    nchunks = buf->size / 1024 + (buf->size % 1024 == 0 ? 0 : 1);
    for (chunk = i = 0;;) {
        WT_ERR(__wt_buf_catfmt(session, tmp, "%02x ", ((uint8_t *)buf->data)[i]));
        if (++i == buf->size || i % 1024 == 0) {
            __wt_errx(session,
              WT_CORRUPT_FMT ": (chunk %" WT_SIZET_FMT " of %" WT_SIZET_FMT "): %.*s", objectid,
              (uintmax_t)offset, size, checksum, ++chunk, nchunks, (int)tmp->size,
              (char *)tmp->data);
            if (i == buf->size)
                break;
            WT_ERR(__wt_buf_set(session, tmp, "", 0));
        }
    }

err:
    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * __wt_bm_corrupt --
 *     Report a block has been corrupted, external API.
 */
int
__wt_bm_corrupt(WT_BM *bm, WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size)
{
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    wt_off_t offset;
    uint32_t checksum, objectid, size;

    /* Read the block. */
    WT_RET(__wt_scr_alloc(session, 0, &tmp));
    WT_ERR(__wt_bm_read(bm, session, tmp, NULL, addr, addr_size));

    /* Crack the cookie, dump the block. */
    WT_ERR(__wt_block_addr_unpack(
      session, bm->block, addr, addr_size, &objectid, &offset, &size, &checksum));
    WT_ERR(__wt_bm_corrupt_dump(session, tmp, objectid, offset, size, checksum));

err:
    __wt_scr_free(session, &tmp);
    return (ret);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_block_read_off_blind --
 *     Read the block at an offset, return the size and checksum, debugging only.
 */
int
__wt_block_read_off_blind(
  WT_SESSION_IMPL *session, WT_BLOCK *block, wt_off_t offset, uint32_t *sizep, uint32_t *checksump)
{
    WT_BLOCK_HEADER *blk;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;

    *sizep = 0;
    *checksump = 0;

    /*
     * Make sure the buffer is large enough for the header and read the first allocation-size block.
     */
    WT_RET(__wt_scr_alloc(session, block->allocsize, &tmp));
    WT_ERR(__wt_read(session, block->fh, offset, (size_t)block->allocsize, tmp->mem));
    blk = WT_BLOCK_HEADER_REF(tmp->mem);
    __wt_block_header_byteswap(blk);

    *sizep = blk->disk_size;
    *checksump = blk->checksum;

err:
    __wt_scr_free(session, &tmp);
    return (ret);
}
#endif

/*
 * __wti_block_read_off --
 *     Read an addr/size pair referenced block into a buffer.
 */
int
__wti_block_read_off(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_ITEM *buf, uint32_t objectid,
  wt_off_t offset, uint32_t size, uint32_t checksum)
{
    WT_BLOCK_HEADER *blk, swap;
    size_t bufsize, check_size;
    uint64_t time_start, time_stop;
    int failures, max_failures;
    bool chunkcache_hit, full_checksum_mismatch;

    time_start = __wt_clock(session);

    chunkcache_hit = full_checksum_mismatch = false;
    check_size = 0;
    failures = 0;
    bufsize = size;
    max_failures = F_ISSET(&S2C(session)->chunkcache, WT_CHUNKCACHE_CONFIGURED) ? 2 : 1;
    __wt_verbose_debug2(session, WT_VERB_READ,
      "off %" PRIuMAX ", size %" PRIu32 ", checksum %#" PRIx32, (uintmax_t)offset, size, checksum);

    WT_STAT_CONN_INCR(session, block_read);
    WT_STAT_CONN_INCRV(session, block_byte_read, size);

    /*
     * Ensure we don't read information that isn't there. It shouldn't ever happen, but it's a cheap
     * test.
     */
    if (size < block->allocsize)
        WT_RET_MSG(session, EINVAL,
          "%s: impossibly small block size of %" PRIu32 "B, less than allocation size of %" PRIu32,
          block->name, size, block->allocsize);

    WT_RET(__wt_buf_init(session, buf, bufsize));
    buf->size = size;

    while (failures < max_failures) {
        full_checksum_mismatch = false;
        if (F_ISSET(&S2C(session)->chunkcache, WT_CHUNKCACHE_CONFIGURED)) {
            if (failures == 0) {
                /*
                 * Check if the chunk cache has the needed data. If there is a miss in the chunk
                 * cache, it will read and cache the data. If the chunk cache has exceeded its
                 * configured capacity and is unable to evict chunks quickly enough, it will return
                 * the error code indicating that it is out of space. We do not propagate this error
                 * up to our caller; we read the needed data ourselves instead.
                 */
                WT_RET_ERROR_OK(__wt_chunkcache_get(session, block, objectid, offset, size,
                                  buf->mem, &chunkcache_hit),
                  ENOSPC);
            }
        }
        if (!chunkcache_hit || failures > 0) {
            __wt_capacity_throttle(session, size, WT_THROTTLE_READ);
            WT_RET(__wt_read(session, block->fh, offset, size, buf->mem));
        }

        /*
         * We incrementally read through the structure before doing a checksum, do little- to
         * big-endian handling early on, and then select from the original or swapped structure as
         * needed.
         */
        blk = WT_BLOCK_HEADER_REF(buf->mem);
        __wt_block_header_byteswap_copy(blk, &swap);
        check_size = F_ISSET(&swap, WT_BLOCK_DATA_CKSUM) ? size : WT_BLOCK_COMPRESS_SKIP;
        if (swap.checksum == checksum) {
            /*
             * Set block header checksum to 0 to allow the checksum to be computed, as its
             * calculation includes the block header. Not clearing it would result in the checksum
             * being miscalculated. blk->checksum remains cleared, as it will not be revisited
             * during a B-tree traversal.
             */
            blk->checksum = 0;
            if (__wt_checksum_match(buf->mem, check_size, checksum)) {
                time_stop = __wt_clock(session);
                __wt_stat_msecs_hist_incr_bmread(session, WT_CLOCKDIFF_MS(time_stop, time_start));

                /*
                 * Swap the page-header as needed; this doesn't belong here, but it's the best place
                 * to catch all callers.
                 */
                __wt_page_header_byteswap(buf->mem);
                return (0);
            }
            full_checksum_mismatch = true;
        }
        failures++;

        /*
         * If chunk cache is configured we want to account for the race condition where the chunk
         * cache could have stale content, and therefore a mismatched checksum. We can also have
         * corrupted data in the chunk cache. For those scenarios, we do not want to fail
         * immediately, so we will reload the data and retry one time.
         */
        if (failures < max_failures) {
            __wt_verbose(session, WT_VERB_BLOCK,
              "Reloading data due to checksum mismatch for block: %s" PRIu32 ", offset: %" PRIuMAX
              ", size: %" PRIu32
              " with possibly stale or corrupt chunk cache content for object id: %" PRIu32
              ". Retrying once.",
              block->name, (uintmax_t)offset, size, objectid);
            WT_RET(__wt_chunkcache_free_external(session, block, objectid, offset, size));
            WT_RET(__wt_read(session, block->fh, offset, size, buf->mem));
            WT_STAT_CONN_INCR(session, chunkcache_retries_checksum_mismatch);
        }
    }

    if (!F_ISSET(session, WT_SESSION_QUIET_CORRUPT_FILE)) {
        if (full_checksum_mismatch)
            __wt_errx(session,
              "%s: potential hardware corruption, read checksum error for %" PRIu32
              "B block at offset %" PRIuMAX ": calculated block checksum of %#" PRIx32
              " doesn't match expected checksum of %#" PRIx32,
              block->name, size, (uintmax_t)offset, __wt_checksum(buf->mem, check_size), checksum);
        else
            __wt_errx(session,
              "%s: potential hardware corruption, read checksum error for %" PRIu32
              "B block at offset %" PRIuMAX ": block header checksum of %#" PRIx32
              " doesn't match expected checksum of %#" PRIx32,
              block->name, size, (uintmax_t)offset, swap.checksum, checksum);
        WT_IGNORE_RET(__wt_bm_corrupt_dump(session, buf, objectid, offset, size, checksum));
    }

    /* Panic if a checksum fails during an ordinary read. */
    F_SET_ATOMIC_32(S2C(session), WT_CONN_DATA_CORRUPTION);

    if (block->verify || F_ISSET(session, WT_SESSION_QUIET_CORRUPT_FILE))
        return (WT_ERROR);

    /*
     * Dump the extent lists associated with all available checkpoints in the system. Viewing the
     * state of the extent lists in the event of a read error can help pinpoint the reason for the
     * read error. Since dumping the extent lists also requires reading the block, we must set the
     * WT_SESSION_DUMPING_EXTLIST flag to ensure we don't recursively attempt to dump extent lists.
     */
    if (!F_ISSET(session, WT_SESSION_DUMPING_EXTLIST)) {
        F_SET(session, WT_SESSION_DUMPING_EXTLIST);
        /* Dump the live checkpoint extent lists. */
        WT_IGNORE_RET(__wti_block_extlist_dump(session, &block->live.alloc));
        WT_IGNORE_RET(__wti_block_extlist_dump(session, &block->live.avail));
        WT_IGNORE_RET(__wti_block_extlist_dump(session, &block->live.discard));

        /* Dump the rest of the extent lists associated with any other valid checkpoints in the
         * file. */
        WT_IGNORE_RET(__wti_block_checkpoint_extlist_dump(session, block));

        F_CLR(session, WT_SESSION_DUMPING_EXTLIST);
    }

    WT_RET_PANIC(session, WT_ERROR, "%s: fatal read error", block->name);
}
