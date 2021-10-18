/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static void __block_dump_avail(WT_SESSION_IMPL *, WT_BLOCK *, bool);

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
    block->compact_pages_reviewed = 0;
    block->compact_pages_skipped = 0;
    block->compact_pages_written = 0;

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
    if (WT_VERBOSE_ISSET(session, WT_VERB_COMPACT)) {
        __wt_spin_lock(session, &block->live_lock);
        __block_dump_avail(session, block, false);
        __wt_spin_unlock(session, &block->live_lock);
    }
    return (0);
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
    if (block->size <= WT_MEGABYTE)
        return (0);

    __wt_spin_lock(session, &block->live_lock);

    /* Dump the current state of the file. */
    if (WT_VERBOSE_ISSET(session, WT_VERB_COMPACT))
        __block_dump_avail(session, block, true);

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

    __wt_verbose(session, WT_VERB_COMPACT,
      "%s: %" PRIuMAX "MB (%" PRIuMAX ") available space in the first 80%% of the file",
      block->name, (uintmax_t)avail_eighty / WT_MEGABYTE, (uintmax_t)avail_eighty);
    __wt_verbose(session, WT_VERB_COMPACT,
      "%s: %" PRIuMAX "MB (%" PRIuMAX ") available space in the first 90%% of the file",
      block->name, (uintmax_t)avail_ninety / WT_MEGABYTE, (uintmax_t)avail_ninety);
    __wt_verbose(session, WT_VERB_COMPACT,
      "%s: require 10%% or %" PRIuMAX "MB (%" PRIuMAX
      ") in the first 90%% of the file to perform compaction, compaction %s",
      block->name, (uintmax_t)(block->size / 10) / WT_MEGABYTE, (uintmax_t)block->size / 10,
      *skipp ? "skipped" : "proceeding");

    __wt_spin_unlock(session, &block->live_lock);

    return (0);
}

/*
 * __wt_block_compact_page_skip --
 *     Return if writing a particular page will shrink the file.
 */
int
__wt_block_compact_page_skip(
  WT_SESSION_IMPL *session, WT_BLOCK *block, const uint8_t *addr, size_t addr_size, bool *skipp)
{
    WT_EXT *ext;
    WT_EXTLIST *el;
    wt_off_t limit, offset;
    uint32_t checksum, objectid, size;

    *skipp = true; /* Return a default skip. */

    /* Crack the cookie. */
    WT_RET(__wt_block_addr_unpack(
      session, block, addr, addr_size, &objectid, &offset, &size, &checksum));

    /*
     * If this block is in the chosen percentage of the file and there's a block on the available
     * list that's appears before that percentage of the file, rewrite the block. Checking the
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

    ++block->compact_pages_reviewed;
    if (*skipp)
        ++block->compact_pages_skipped;
    else
        ++block->compact_pages_written;

    return (0);
}

/*
 * __block_dump_avail --
 *     Dump out the avail list so we can see what compaction will look like.
 */
static void
__block_dump_avail(WT_SESSION_IMPL *session, WT_BLOCK *block, bool start)
{
    WT_EXT *ext;
    WT_EXTLIST *el;
    wt_off_t decile[10], percentile[100], size, v;
    u_int i;

    el = &block->live.avail;
    size = block->size;

    __wt_verbose(session, WT_VERB_COMPACT, "============ %s",
      start ? "testing for compaction" : "ending compaction pass");

    if (!start) {
        __wt_verbose(
          session, WT_VERB_COMPACT, "pages reviewed: %" PRIu64, block->compact_pages_reviewed);
        __wt_verbose(
          session, WT_VERB_COMPACT, "pages skipped: %" PRIu64, block->compact_pages_skipped);
        __wt_verbose(
          session, WT_VERB_COMPACT, "pages written: %" PRIu64, block->compact_pages_written);
    }

    __wt_verbose(session, WT_VERB_COMPACT,
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
     * buckets.
     */
    for (i = 0; i < WT_ELEMENTS(percentile); ++i) {
        v = percentile[i] * 512;
        __wt_verbose(session, WT_VERB_COMPACT,
          "%2u%%: %12" PRIuMAX "MB, (%" PRIuMAX "B, %" PRIuMAX "%%)", i, (uintmax_t)v / WT_MEGABYTE,
          (uintmax_t)v, (uintmax_t)((v * 100) / (wt_off_t)el->bytes));
    }
#endif
    for (i = 0; i < WT_ELEMENTS(decile); ++i) {
        v = decile[i] * 512;
        __wt_verbose(session, WT_VERB_COMPACT,
          "%2u%%: %12" PRIuMAX "MB, (%" PRIuMAX "B, %" PRIuMAX "%%)", i * 10,
          (uintmax_t)v / WT_MEGABYTE, (uintmax_t)v, (uintmax_t)((v * 100) / (wt_off_t)el->bytes));
    }
}
