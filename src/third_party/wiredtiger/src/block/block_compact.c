/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static void __block_dump_file_stat(WT_SESSION_IMPL *, WT_BLOCK *, bool);

/*
 * __wt_block_compact_start --
 *     Start compaction of a file.
 */
int
__wt_block_compact_start(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
    WT_UNUSED(session);

    /* Switch to first-fit allocation. */
    __wt_block_configure_first_fit(block, true);

    /* Reset the compaction state information. */
    block->compact_pct_tenths = 0;
    block->compact_blocks_moved = 0;
    block->compact_cache_pages_dealt = 0;
    block->compact_pages_reviewed = 0;
    block->compact_pages_skipped = 0;

    return (0);
}

/*
 * __wt_block_compact_end --
 *     End compaction of a file.
 */
int
__wt_block_compact_end(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
    /* Restore the original allocation plan. */
    __wt_block_configure_first_fit(block, false);

    /* Dump the results of the compaction pass. */
    if (WT_VERBOSE_LEVEL_ISSET(session, WT_VERB_COMPACT, WT_VERBOSE_DEBUG)) {
        __wt_spin_lock(session, &block->live_lock);
        __block_dump_file_stat(session, block, false);
        __wt_spin_unlock(session, &block->live_lock);
    }
    return (0);
}

/*
 * __wt_block_compact_progress --
 *     Output compact progress message.
 */
void
__wt_block_compact_progress(WT_SESSION_IMPL *session, WT_BLOCK *block, u_int *msg_countp)
{
    struct timespec cur_time;
    uint64_t time_diff;

    if (!WT_VERBOSE_LEVEL_ISSET(session, WT_VERB_COMPACT_PROGRESS, WT_VERBOSE_DEBUG))
        return;

    __wt_epoch(session, &cur_time);

    /* Log one progress message every twenty seconds. */
    time_diff = WT_TIMEDIFF_SEC(cur_time, session->compact->begin);
    if (time_diff / WT_PROGRESS_MSG_PERIOD > *msg_countp) {
        ++*msg_countp;
        __wt_verbose_debug(session, WT_VERB_COMPACT_PROGRESS,
          " compacting %s for %" PRIu64 " seconds; reviewed %" PRIu64 " pages, skipped %" PRIu64
          " pages, cache pages evicted %" PRIu64 ", on-disk pages moved %" PRIu64,
          block->name, time_diff, block->compact_pages_reviewed, block->compact_pages_skipped,
          block->compact_cache_pages_dealt, block->compact_blocks_moved);
    }
}
/*
 * __wt_block_compact_skip --
 *     Return if compaction will shrink the file.
 */
int
__wt_block_compact_skip(WT_SESSION_IMPL *session, WT_BLOCK *block, bool *skipp)
{
    WT_EXT *ext;
    WT_EXTLIST *el;
    wt_off_t avail_eighty, avail_ninety, eighty, ninety;

    *skipp = true; /* Return a default skip. */

    /*
     * We do compaction by copying blocks from the end of the file to the beginning of the file, and
     * we need some metrics to decide if it's worth doing. Ignore small files, and files where we
     * are unlikely to recover 10% of the file.
     */
    if (block->size <= WT_MEGABYTE) {
        __wt_verbose_info(session, WT_VERB_COMPACT,
          "%s: skipping because the file size must be greater than 1MB: %" PRIuMAX "B.",
          block->name, (uintmax_t)block->size);

        return (0);
    }

    __wt_spin_lock(session, &block->live_lock);

    /* Dump the current state of the file. */
    if (WT_VERBOSE_LEVEL_ISSET(session, WT_VERB_COMPACT, WT_VERBOSE_DEBUG))
        __block_dump_file_stat(session, block, true);

    /* Sum the available bytes in the initial 80% and 90% of the file. */
    avail_eighty = avail_ninety = 0;
    ninety = block->size - block->size / 10;
    eighty = block->size - ((block->size / 10) * 2);

    el = &block->live.avail;
    WT_EXT_FOREACH (ext, el->off)
        if (ext->off < ninety) {
            avail_ninety += ext->size;
            if (ext->off < eighty)
                avail_eighty += ext->size;
        }

    /*
     * Skip files where we can't recover at least 1MB.
     *
     * If at least 20% of the total file is available and in the first 80% of the file, we'll try
     * compaction on the last 20% of the file; else, if at least 10% of the total file is available
     * and in the first 90% of the file, we'll try compaction on the last 10% of the file.
     *
     * We could push this further, but there's diminishing returns, a mostly empty file can be
     * processed quickly, so more aggressive compaction is less useful.
     */
    if (avail_eighty > WT_MEGABYTE && avail_eighty >= ((block->size / 10) * 2)) {
        *skipp = false;
        block->compact_pct_tenths = 2;
    } else if (avail_ninety > WT_MEGABYTE && avail_ninety >= block->size / 10) {
        *skipp = false;
        block->compact_pct_tenths = 1;
    }

    __wt_verbose_debug(session, WT_VERB_COMPACT,
      "%s: total reviewed %" PRIu64 " pages, total skipped %" PRIu64 " pages, total wrote %" PRIu64
      " pages",
      block->name, block->compact_pages_reviewed, block->compact_pages_skipped,
      block->compact_cache_pages_dealt);
    __wt_verbose_debug(session, WT_VERB_COMPACT,
      "%s: %" PRIuMAX "MB (%" PRIuMAX ") available space in the first 80%% of the file",
      block->name, (uintmax_t)avail_eighty / WT_MEGABYTE, (uintmax_t)avail_eighty);
    __wt_verbose_debug(session, WT_VERB_COMPACT,
      "%s: %" PRIuMAX "MB (%" PRIuMAX ") available space in the first 90%% of the file",
      block->name, (uintmax_t)avail_ninety / WT_MEGABYTE, (uintmax_t)avail_ninety);
    __wt_verbose_debug(session, WT_VERB_COMPACT,
      "%s: require 10%% or %" PRIuMAX "MB (%" PRIuMAX
      ") in the first 90%% of the file to perform compaction, compaction %s",
      block->name, (uintmax_t)(block->size / 10) / WT_MEGABYTE, (uintmax_t)block->size / 10,
      *skipp ? "skipped" : "proceeding");

    __wt_spin_unlock(session, &block->live_lock);

    return (0);
}

/*
 * __compact_page_skip --
 *     Return if writing a particular page will shrink the file.
 */
static void
__compact_page_skip(
  WT_SESSION_IMPL *session, WT_BLOCK *block, wt_off_t offset, uint32_t size, bool *skipp)
{
    WT_EXT *ext;
    WT_EXTLIST *el;
    wt_off_t limit;

    *skipp = true; /* Return a default skip. */

    /*
     * If this block is in the chosen percentage of the file and there's a block on the available
     * list that appears before that percentage of the file, rewrite the block. Checking the
     * available list is necessary (otherwise writing the block would extend the file), but there's
     * an obvious race if the file is sufficiently busy.
     */
    __wt_spin_lock(session, &block->live_lock);
    limit = block->size - ((block->size / 10) * block->compact_pct_tenths);
    if (offset > limit) {
        el = &block->live.avail;
        WT_EXT_FOREACH (ext, el->off) {
            if (ext->off >= limit)
                break;
            if (ext->size >= size) {
                *skipp = false;
                break;
            }
        }
    }
    __wt_spin_unlock(session, &block->live_lock);
}

/*
 * __wt_block_compact_page_skip --
 *     Return if writing a particular page will shrink the file.
 */
int
__wt_block_compact_page_skip(
  WT_SESSION_IMPL *session, WT_BLOCK *block, const uint8_t *addr, size_t addr_size, bool *skipp)
{
    wt_off_t offset;
    uint32_t size, checksum, objectid;

    WT_UNUSED(addr_size);
    *skipp = true; /* Return a default skip. */
    offset = 0;

    /* Crack the cookie. */
    WT_RET(__wt_block_addr_unpack(
      session, block, addr, addr_size, &objectid, &offset, &size, &checksum));

    __compact_page_skip(session, block, offset, size, skipp);

    ++block->compact_pages_reviewed;
    if (*skipp)
        ++block->compact_pages_skipped;
    else
        ++block->compact_cache_pages_dealt;

    return (0);
}

/*
 * __wt_block_compact_page_rewrite --
 *     Rewrite a page if it will shrink the file.
 */
int
__wt_block_compact_page_rewrite(
  WT_SESSION_IMPL *session, WT_BLOCK *block, uint8_t *addr, size_t *addr_sizep, bool *skipp)
{
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    wt_off_t offset, new_offset;
    uint32_t size, checksum, objectid;
    uint8_t *endp;
    bool discard_block;

    *skipp = true;  /* Return a default skip. */
    new_offset = 0; /* -Werror=maybe-uninitialized */

    discard_block = false;

    WT_ERR(__wt_block_addr_unpack(
      session, block, addr, *addr_sizep, &objectid, &offset, &size, &checksum));

    /* Check if the block is worth rewriting. */
    __compact_page_skip(session, block, offset, size, skipp);

    if (WT_VERBOSE_LEVEL_ISSET(session, WT_VERB_COMPACT, WT_VERBOSE_DEBUG) ||
      WT_VERBOSE_LEVEL_ISSET(session, WT_VERB_COMPACT_PROGRESS, WT_VERBOSE_DEBUG)) {
        ++block->compact_pages_reviewed;
        if (*skipp)
            ++block->compact_pages_skipped;
        else
            ++block->compact_blocks_moved;
    }
    if (*skipp)
        return (0);

    /* Read the block. */
    WT_ERR(__wt_scr_alloc(session, size, &tmp));
    WT_ERR(__wt_read(session, block->fh, offset, size, tmp->mem));

    /* Allocate a replacement block. */
    WT_ERR(__wt_block_ext_prealloc(session, 5));
    __wt_spin_lock(session, &block->live_lock);
    ret = __wt_block_alloc(session, block, &new_offset, (wt_off_t)size);
    __wt_spin_unlock(session, &block->live_lock);
    WT_ERR(ret);
    discard_block = true;

    /* Write the block. */
    WT_ERR(__wt_write(session, block->fh, new_offset, size, tmp->mem));

    /* Free the original block. */
    __wt_spin_lock(session, &block->live_lock);
    ret = __wt_block_off_free(session, block, objectid, offset, (wt_off_t)size);
    __wt_spin_unlock(session, &block->live_lock);
    WT_ERR(ret);

    /* Build the returned address cookie. */
    endp = addr;
    WT_ERR(__wt_block_addr_pack(block, &endp, objectid, new_offset, size, checksum));
    *addr_sizep = WT_PTRDIFF(endp, addr);

    WT_STAT_CONN_INCR(session, block_write);
    WT_STAT_CONN_INCRV(session, block_byte_write, size);

    discard_block = false;

err:
    if (discard_block) {
        __wt_spin_lock(session, &block->live_lock);
        WT_TRET(__wt_block_off_free(session, block, objectid, new_offset, (wt_off_t)size));
        __wt_spin_unlock(session, &block->live_lock);
    }
    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * __block_dump_bucket_stat --
 *     Dump out the information about available and used blocks in the given bucket (part of the
 *     file).
 */
static void
__block_dump_bucket_stat(WT_SESSION_IMPL *session, uintmax_t file_size, uintmax_t file_free,
  uintmax_t bucket_size, uintmax_t bucket_free, u_int bucket_pct)
{
    uintmax_t bucket_used, free_pct, used_pct;

    free_pct = used_pct = 0;

    /* Handle rounding error in which case bucket used size can be negative. */
    bucket_used = (bucket_size > bucket_free) ? (bucket_size - bucket_free) : 0;

    if (file_free != 0)
        free_pct = (bucket_free * 100) / file_free;

    if (file_size > file_free)
        used_pct = (bucket_used * 100) / (file_size - file_free);

    __wt_verbose_debug(session, WT_VERB_COMPACT,
      "%2u%%: %12" PRIuMAX "MB, (free: %" PRIuMAX "B, %" PRIuMAX "%%), (used: %" PRIuMAX
      "MB, %" PRIuMAX "B, %" PRIuMAX "%%)",
      bucket_pct, bucket_free / WT_MEGABYTE, bucket_free, free_pct, bucket_used / WT_MEGABYTE,
      bucket_used, used_pct);
}

/*
 * __block_dump_file_stat --
 *     Dump out the avail/used list so we can see what compaction will look like.
 */
static void
__block_dump_file_stat(WT_SESSION_IMPL *session, WT_BLOCK *block, bool start)
{
    WT_EXT *ext;
    WT_EXTLIST *el;
    wt_off_t decile[10], percentile[100], size;
    uintmax_t bucket_size;
    u_int i;

    el = &block->live.avail;
    size = block->size;

    __wt_verbose_debug(session, WT_VERB_COMPACT, "============ %s",
      start ? "testing for compaction" : "ending compaction pass");

    if (!start) {
        __wt_verbose_debug(
          session, WT_VERB_COMPACT, "pages reviewed: %" PRIu64, block->compact_pages_reviewed);
        __wt_verbose_debug(
          session, WT_VERB_COMPACT, "pages skipped: %" PRIu64, block->compact_pages_skipped);
        __wt_verbose_debug(session, WT_VERB_COMPACT,
          "cache pages read/flushed out of the cache: %" PRIu64, block->compact_cache_pages_dealt);
        __wt_verbose_debug(
          session, WT_VERB_COMPACT, "blocks moved : %" PRIu64, block->compact_blocks_moved);
    }

    __wt_verbose_debug(session, WT_VERB_COMPACT,
      "file size %" PRIuMAX "MB (%" PRIuMAX ") with %" PRIuMAX "%% space available %" PRIuMAX
      "MB (%" PRIuMAX ")",
      (uintmax_t)size / WT_MEGABYTE, (uintmax_t)size,
      ((uintmax_t)el->bytes * 100) / (uintmax_t)size, (uintmax_t)el->bytes / WT_MEGABYTE,
      (uintmax_t)el->bytes);

    if (el->entries == 0)
        return;

    /*
     * Bucket the available memory into file deciles/percentiles. Large pieces of memory will cross
     * over multiple buckets, assign to the decile/percentile in 512B chunks.
     */
    memset(decile, 0, sizeof(decile));
    memset(percentile, 0, sizeof(percentile));
    WT_EXT_FOREACH (ext, el->off)
        for (i = 0; i < ext->size / 512; ++i) {
            ++decile[((ext->off + (wt_off_t)i * 512) * 10) / size];
            ++percentile[((ext->off + (wt_off_t)i * 512) * 100) / size];
        }

#ifdef __VERBOSE_OUTPUT_PERCENTILE
    /*
     * The verbose output always displays 10% buckets, running this code as well also displays 1%
     * buckets. There will be rounding error in the `used` stats because of the bucket size
     * calculation. Adding 50 to minimize the rounding error.
     */
    bucket_size = (uintmax_t)((size + 50) / 100);
    for (i = 0; i < WT_ELEMENTS(percentile); ++i)
        __block_dump_bucket_stat(session, (uintmax_t)size, (uintmax_t)el->bytes, bucket_size,
          (uintmax_t)percentile[i] * 512, i);
#endif

    /*
     * There will be rounding error in the `used` stats because of the bucket size calculation.
     * Adding 5 to minimize the rounding error.
     */
    bucket_size = (uintmax_t)((size + 5) / 10);
    for (i = 0; i < WT_ELEMENTS(decile); ++i)
        __block_dump_bucket_stat(session, (uintmax_t)size, (uintmax_t)el->bytes, bucket_size,
          (uintmax_t)decile[i] * 512, i * 10);
}
