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

    if (block->compact_session_id != WT_SESSION_ID_INVALID)
        return (EBUSY);

    /* Switch to first-fit allocation. */
    __wt_block_configure_first_fit(block, true);

    /* Reset the compaction state information. */
    block->compact_session_id = session->id;
    block->compact_pct_tenths = 0;
    block->compact_bytes_reviewed = 0;
    block->compact_bytes_rewritten = 0;
    block->compact_internal_pages_reviewed = 0;
    block->compact_pages_reviewed = 0;
    block->compact_pages_rewritten = 0;
    block->compact_pages_rewritten_expected = 0;
    block->compact_pages_skipped = 0;

    if (session == S2C(session)->background_compact.session)
        WT_RET(__wt_background_compact_start(session));

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

    /* Ensure this the same session that started compaction. */
    WT_ASSERT(session, block->compact_session_id == session->id);
    block->compact_session_id = WT_SESSION_ID_INVALID;

    /* Dump the results of the compaction pass. */
    if (WT_VERBOSE_LEVEL_ISSET(session, WT_VERB_COMPACT, WT_VERBOSE_DEBUG_1)) {
        __wt_spin_lock(session, &block->live_lock);
        __block_dump_file_stat(session, block, false);
        __wt_spin_unlock(session, &block->live_lock);
    }

    if (session == S2C(session)->background_compact.session)
        WT_RET(__wt_background_compact_end(session));

    return (0);
}

/*
 * __wt_block_compact_get_progress_stats --
 *     Collect compact progress stats.
 */
void
__wt_block_compact_get_progress_stats(WT_SESSION_IMPL *session, WT_BM *bm,
  uint64_t *pages_reviewedp, uint64_t *pages_skippedp, uint64_t *pages_rewrittenp,
  uint64_t *pages_rewritten_expectedp)
{
    WT_BLOCK *block;

    WT_UNUSED(session);
    block = bm->block;
    *pages_reviewedp = block->compact_pages_reviewed;
    *pages_skippedp = block->compact_pages_skipped;
    *pages_rewrittenp = block->compact_pages_rewritten;
    *pages_rewritten_expectedp = block->compact_pages_rewritten_expected;
}

/*
 * __block_compact_trim_extent --
 *     Trim the extent to the given range mask, specified via start and end offsets.
 */
static inline void
__block_compact_trim_extent(WT_SESSION_IMPL *session, wt_off_t mask_start, wt_off_t mask_end,
  wt_off_t *ext_startp, wt_off_t *ext_sizep)
{
    WT_UNUSED(session);

    if (mask_end >= 0 && mask_end < mask_start) {
        *ext_sizep = 0;
        return;
    }

    /* Trim from the beginning. */
    if (*ext_startp < mask_start) {
        if (*ext_startp + *ext_sizep <= mask_start) {
            *ext_sizep = 0;
            return;
        }
        *ext_sizep -= mask_start - *ext_startp;
        *ext_startp = mask_start;
    }

    /* Trim from the end. */
    if (mask_end >= 0 && *ext_startp + *ext_sizep > mask_end) {
        *ext_sizep = mask_end - *ext_startp;
        if (*ext_sizep <= 0) {
            *ext_sizep = 0;
        }
    }
}

/*
 * __block_compact_skip_internal --
 *     Return if compaction will shrink the file. This function takes a few extra parameters, so
 *     that it can be useful for both making the actual compaction decisions as well as for
 *     estimating the work ahead of the compaction itself: the file size, the smallest offset that
 *     the first-fit allocation is likely to consider, and the number of available (unallocated)
 *     bytes before that offset.
 */
static void
__block_compact_skip_internal(WT_SESSION_IMPL *session, WT_BLOCK *block, bool estimate,
  wt_off_t file_size, wt_off_t start_offset, wt_off_t avail_bytes_before_start_offset, bool *skipp,
  int *compact_pct_tenths_p)
{
    WT_EXT *ext;
    wt_off_t avail_eighty, avail_ninety, off, size, eighty, ninety;

    WT_ASSERT_SPINLOCK_OWNED(session, &block->live_lock);

    /* Sum the available bytes in the initial 80% and 90% of the file. */
    avail_eighty = avail_ninety = avail_bytes_before_start_offset;
    ninety = file_size - file_size / 10;
    eighty = file_size - ((file_size / 10) * 2);

    WT_EXT_FOREACH_FROM_OFFSET_INCL(ext, &block->live.avail, start_offset)
    {
        off = ext->off;
        size = ext->size;
        __block_compact_trim_extent(session, start_offset, file_size, &off, &size);
        if (off < ninety) {
            avail_ninety += size;
            if (off < eighty)
                avail_eighty += size;
        }
    }

    /*
     * Skip files where we can't recover at least 1MB.
     *
     * WiredTiger uses first-fit compaction: It finds space in the beginning of the file and moves
     * data from the end of the file into that space. If at least 20% of the total file is available
     * and in the first 80% of the file, we'll try compaction on the last 20% of the file; else, if
     * at least 10% of the total file is available and in the first 90% of the file, we'll try
     * compaction on the last 10% of the file.
     *
     * We could push this further, but there's diminishing returns, a mostly empty file can be
     * processed quickly, so more aggressive compaction is less useful.
     */
    if (avail_eighty > WT_MEGABYTE && avail_eighty >= ((file_size / 10) * 2)) {
        *skipp = false;
        *compact_pct_tenths_p = 2;
    } else if (avail_ninety > WT_MEGABYTE && avail_ninety >= file_size / 10) {
        *skipp = false;
        *compact_pct_tenths_p = 1;
    } else {
        *skipp = true;
        *compact_pct_tenths_p = 0;
    }

    if (!estimate)
        __wt_verbose_level(session, WT_VERB_COMPACT, WT_VERBOSE_DEBUG_1,
          "%s: total reviewed %" PRIu64 " pages, total rewritten %" PRIu64 " pages", block->name,
          block->compact_pages_reviewed, block->compact_pages_rewritten);
    __wt_verbose_level(session, WT_VERB_COMPACT,
      (estimate ? WT_VERBOSE_DEBUG_3 : WT_VERBOSE_DEBUG_1),
      "%s:%s %" PRIuMAX "MB (%" PRIuMAX ") available space in the first 80%% of the file",
      block->name, estimate ? " estimating --" : "", (uintmax_t)avail_eighty / WT_MEGABYTE,
      (uintmax_t)avail_eighty);
    __wt_verbose_level(session, WT_VERB_COMPACT,
      (estimate ? WT_VERBOSE_DEBUG_3 : WT_VERBOSE_DEBUG_1),
      "%s:%s %" PRIuMAX "MB (%" PRIuMAX ") available space in the first 90%% of the file",
      block->name, estimate ? " estimating --" : "", (uintmax_t)avail_ninety / WT_MEGABYTE,
      (uintmax_t)avail_ninety);
    __wt_verbose_level(session, WT_VERB_COMPACT,
      (estimate ? WT_VERBOSE_DEBUG_3 : WT_VERBOSE_DEBUG_1),
      "%s:%s require 10%% or %" PRIuMAX "MB (%" PRIuMAX
      ") in the first 90%% of the file to perform compaction, compaction %s",
      block->name, estimate ? " estimating --" : "", (uintmax_t)(file_size / 10) / WT_MEGABYTE,
      (uintmax_t)(file_size / 10), *skipp ? "skipped" : "proceeding");
}

/*
 * __block_compact_estimate_remaining_work --
 *     Estimate how much more work the compaction needs to do for the given file.
 */
static void
__block_compact_estimate_remaining_work(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
    WT_EXT *ext;
    wt_off_t avg_block_size, avg_internal_block_size, depth1_subtree_size, leaves_per_internal_page;
    wt_off_t compact_start_off, extra_space, file_size, last, off, rewrite_size, size, write_off;
    uint64_t n, pages_to_move, pages_to_move_orig, total_pages_to_move;
    int compact_pct_tenths, iteration;
    bool skip;

    /*
     * We must have reviewed at least some interesting number of pages for any estimates below to be
     * worthwhile.
     */
    if (block->compact_pages_reviewed < WT_THOUSAND)
        return;

    /* Assume that we have already checked whether this file can be skipped. */
    WT_ASSERT(session, block->compact_pct_tenths > 0);

    /*
     * Get the average block size that we encountered so far during compaction. Note that we are not
     * currently accounting for overflow pages, as compact does not currently account for them
     * either.
     */
    avg_block_size = (wt_off_t)WT_ALIGN(
      block->compact_bytes_reviewed / block->compact_pages_reviewed, block->allocsize);

    /* We don't currently have a way to track the internal page size, but this should be okay. */
    avg_internal_block_size = block->allocsize;

    /*
     * Estimate the average number of leaf pages per one internal page. This way of doing the
     * estimate is sufficient, because we expect each internal node to have a large number of
     * children, so that the number of higher-level internal nodes is small relative to the internal
     * nodes at the bottom.
     */
    leaves_per_internal_page =
      (wt_off_t)(block->compact_pages_reviewed / block->compact_internal_pages_reviewed);

    /*
     * Estimate the size of a "depth 1" subtree consisting of one internal page and the
     * corresponding leaves.
     */
    depth1_subtree_size = avg_block_size * leaves_per_internal_page + avg_internal_block_size;

    __wt_verbose_debug2(session, WT_VERB_COMPACT,
      "%s: the average block size is %" PRId64 " bytes (based on %" PRIu64 " blocks)", block->name,
      avg_block_size, block->compact_pages_reviewed);
    __wt_verbose_debug2(session, WT_VERB_COMPACT, "%s: reviewed %" PRIu64 " internal pages so far",
      block->name, block->compact_internal_pages_reviewed);

    /*
     * We would like to estimate how much data will be moved during compaction, so that we can
     * inform users how far along we are in the process. We will estimate how many pages are in the
     * last part of the file (typically the last 10%) and where they will be written using the
     * first-fit allocation, and then repeat as long as we continue to make progress, to emulate the
     * behavior of the actual compaction. This does not account for all complexities that we may
     * encounter, but the hope is that the estimate would be still good enough.
     */

    __wt_spin_lock(session, &block->live_lock);

    compact_pct_tenths = block->compact_pct_tenths;
    extra_space = 0;
    file_size = block->size;
    pages_to_move = 0;
    total_pages_to_move = 0;
    write_off = 0;

    /* Macro for estimating the number of leaf pages that can be stored within an extent. */
#define WT_EXT_SIZE_TO_LEAF_PAGES(ext_size)                                  \
    (uint64_t)((ext_size) / depth1_subtree_size * leaves_per_internal_page + \
      ((ext_size) % depth1_subtree_size) / avg_block_size)

    /* Now do the actual estimation, simulating one compact pass at a time. */
    for (iteration = 0;; iteration++) {
        compact_start_off = file_size - compact_pct_tenths * file_size / 10;
        if (write_off >= compact_start_off)
            break;
        __wt_verbose_debug2(session, WT_VERB_COMPACT,
          "%s: estimating -- pass %d: file size: %" PRId64 ", compact offset: %" PRId64
          ", will move blocks from the last %d%% of the file",
          block->name, iteration, file_size, compact_start_off, compact_pct_tenths * 10);

        /*
         * Estimate how many pages we would like to move, just using the live checkpoint. The
         * checkpoint doesn't have a complete list of allocated extents, so we estimate it in two
         * phases: We first take an inverse of the "available" list, which gives us all extents that
         * are either currently allocated or are to be discarded at the next checkpoint. We do this
         * by first estimating the number of pages that can fit in the inverse of the "available"
         * list, and then we subtract the number of pages determined from the "discard" list.
         */
        last = compact_start_off;
        WT_EXT_FOREACH_FROM_OFFSET_INCL(ext, &block->live.avail, compact_start_off)
        {
            off = ext->off;
            size = ext->size;
            WT_ASSERT(session, off >= compact_start_off || off + size >= compact_start_off);

            __block_compact_trim_extent(session, compact_start_off, file_size, &off, &size);
            if (off >= compact_start_off && size <= 0)
                break;

            if (off > last) {
                n = WT_EXT_SIZE_TO_LEAF_PAGES(off - last);
                pages_to_move += n;
                __wt_verbose_debug3(session, WT_VERB_COMPACT,
                  "%s: estimating -- %" PRIu64 " pages to move between %" PRId64 " and %" PRId64,
                  block->name, n, last, off);
            }
            last = off + size;
        }
        n = WT_EXT_SIZE_TO_LEAF_PAGES(file_size - last);
        pages_to_move += n;
        __wt_verbose_debug3(session, WT_VERB_COMPACT,
          "%s: estimating -- %" PRIu64 " pages to move between %" PRId64 " and %" PRId64,
          block->name, n, last, file_size);

        WT_EXT_FOREACH_FROM_OFFSET_INCL(ext, &block->live.discard, compact_start_off)
        {
            off = ext->off;
            size = ext->size;
            WT_ASSERT(session, off >= compact_start_off || off + size >= compact_start_off);

            __block_compact_trim_extent(session, compact_start_off, file_size, &off, &size);
            if (off >= compact_start_off && size <= 0)
                break;

            n = WT_EXT_SIZE_TO_LEAF_PAGES(size);
            pages_to_move -= WT_MIN(n, pages_to_move);
            __wt_verbose_debug3(session, WT_VERB_COMPACT,
              "%s: estimating -- %" PRIu64 " pages on discard list between %" PRId64
              " and %" PRId64,
              block->name, n, off, off + size);
        }
        if (pages_to_move == 0)
            break;

        /* Estimate where in the file we would be when we finish moving those pages. */
        pages_to_move_orig = pages_to_move;
        WT_EXT_FOREACH_FROM_OFFSET_INCL(ext, &block->live.avail, write_off)
        {
            off = ext->off;
            size = ext->size;
            WT_ASSERT(session, off >= write_off || off + size >= write_off);

            if (pages_to_move == 0 || off >= compact_start_off)
                break;

            __block_compact_trim_extent(session, write_off, compact_start_off, &off, &size);
            if (off >= write_off && size <= 0)
                break;

            n = WT_EXT_SIZE_TO_LEAF_PAGES(size);
            n = WT_MIN(n, pages_to_move);
            pages_to_move -= n;
            total_pages_to_move += n;

            rewrite_size = (wt_off_t)n * avg_block_size +
              (wt_off_t)n * avg_internal_block_size / leaves_per_internal_page;
            write_off = off + rewrite_size;
            if (pages_to_move > 0)
                extra_space += size - rewrite_size;
        }
        __wt_verbose_debug2(session, WT_VERB_COMPACT,
          "%s: estimating -- pass %d: will rewrite %" PRIu64 " pages, next write offset: %" PRId64
          ", extra space: %" PRId64,
          block->name, iteration, pages_to_move_orig, write_off, extra_space);

        /* See if we ran out of pages to move. */
        if (pages_to_move > 0)
            break;

        /* If there is more work that could be done, repeat with the shorter file. */
        ext = __wt_block_off_srch_inclusive(&block->live.avail, compact_start_off);
        file_size = ext == NULL ? compact_start_off : WT_MIN(ext->off, compact_start_off);
        __block_compact_skip_internal(
          session, block, true, file_size, write_off, extra_space, &skip, &compact_pct_tenths);
        if (skip)
            break;
    }

#undef WT_EXT_SIZE_TO_LEAF_PAGES

    __wt_spin_unlock(session, &block->live_lock);

    block->compact_pages_rewritten_expected = block->compact_pages_rewritten + total_pages_to_move;
    __wt_verbose_debug1(session, WT_VERB_COMPACT,
      "%s: expecting to move approx. %" PRIu64 " more pages (%" PRIu64 "MB), %" PRIu64
      " total, target %" PRIu64 "MB (%" PRIu64 "B)",
      block->name, total_pages_to_move,
      total_pages_to_move * (uint64_t)avg_block_size / WT_MEGABYTE,
      block->compact_pages_rewritten_expected, session->compact->free_space_target / WT_MEGABYTE,
      session->compact->free_space_target);
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
    int progress;

    if (!WT_VERBOSE_LEVEL_ISSET(session, WT_VERB_COMPACT_PROGRESS, WT_VERBOSE_DEBUG_1))
        return;

    __wt_epoch(session, &cur_time);

    /* Log one progress message every twenty seconds. */
    time_diff = WT_TIMEDIFF_SEC(cur_time, session->compact->begin);
    if (time_diff / WT_PROGRESS_MSG_PERIOD > *msg_countp) {
        ++*msg_countp;

        /*
         * If we don't have the estimate at this point, it means that we haven't reviewed even
         * enough pages. This should almost never happen.
         */
        if (block->compact_pages_rewritten_expected == 0) {
            __wt_verbose_debug1(session, WT_VERB_COMPACT_PROGRESS,
              "compacting %s for %" PRIu64 " seconds; reviewed %" PRIu64
              " pages, rewritten %" PRIu64 " pages (%" PRIu64 "MB)",
              block->name, time_diff, block->compact_pages_reviewed, block->compact_pages_rewritten,
              block->compact_bytes_rewritten / WT_MEGABYTE);
            __wt_verbose_debug1(session, WT_VERB_COMPACT,
              "%s: still collecting information for estimating the progress", block->name);
        } else {
            progress = WT_MIN(
              (int)(100 * block->compact_pages_rewritten / block->compact_pages_rewritten_expected),
              100);
            __wt_verbose_debug1(session, WT_VERB_COMPACT_PROGRESS,
              "compacting %s for %" PRIu64 " seconds; reviewed %" PRIu64
              " pages, rewritten %" PRIu64 " pages (%" PRIu64 "MB), approx. %d%% done",
              block->name, time_diff, block->compact_pages_reviewed, block->compact_pages_rewritten,
              block->compact_bytes_rewritten / WT_MEGABYTE, progress);
        }
    }
}

/*
 * __wt_block_compact_skip --
 *     Return if compaction will shrink the file.
 */
int
__wt_block_compact_skip(WT_SESSION_IMPL *session, WT_BLOCK *block, bool *skipp)
{
    *skipp = true; /* Return a default skip. */

    /*
     * We do compaction by copying blocks from the end of the file to the beginning of the file, and
     * we need some metrics to decide if it's worth doing. Ignore small files, and files where we
     * are unlikely to recover 10% of the file.
     */
    if (block->size <= WT_MEGABYTE) {
        __wt_verbose_debug1(session, WT_VERB_COMPACT,
          "%s: skipping because the file size must be greater than 1MB: %" PRIuMAX "B.",
          block->name, (uintmax_t)block->size);

        return (0);
    }

    __wt_spin_lock(session, &block->live_lock);

    /* Dump the current state of the file. */
    if (WT_VERBOSE_LEVEL_ISSET(session, WT_VERB_COMPACT, WT_VERBOSE_DEBUG_2))
        __block_dump_file_stat(session, block, true);

    /*
     * Check if the number of available bytes matches the expected configured threshold. Only
     * perform that check during the first iteration.
     */
    if (block->compact_pages_reviewed == 0 &&
      block->live.avail.bytes < session->compact->free_space_target)
        __wt_verbose_debug1(session, WT_VERB_COMPACT,
          "%s: skipping because the number of available bytes %" PRIu64
          "B is less than the configured threshold %" PRIu64 "B.",
          block->name, block->live.avail.bytes, session->compact->free_space_target);
    else
        __block_compact_skip_internal(
          session, block, false, block->size, 0, 0, skipp, &block->compact_pct_tenths);

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

    ++block->compact_pages_reviewed;
    block->compact_bytes_reviewed += size;
    if (*skipp)
        ++block->compact_pages_skipped;
    else
        ++block->compact_pages_rewritten;

    /* Estimate how much work is left. */
    if (block->compact_pages_rewritten_expected == 0)
        __block_compact_estimate_remaining_work(session, block);
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
    block->compact_bytes_rewritten += size;

    WT_STAT_CONN_INCR(session, block_write);
    WT_STAT_CONN_INCRV(session, block_byte_write, size);
    WT_STAT_CONN_INCRV(session, block_byte_write_compact, size);

    discard_block = false;
    __wt_verbose_level(session, WT_VERB_COMPACT, WT_VERBOSE_DEBUG_4,
      "%s: rewrite %" PRId64 " --> %" PRId64 " (%" PRIu32 "B)", block->name, offset, new_offset,
      size);

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

    __wt_verbose_debug2(session, WT_VERB_COMPACT,
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

    WT_ASSERT_SPINLOCK_OWNED(session, &block->live_lock);

    el = &block->live.avail;
    size = block->size;

    __wt_verbose_debug1(session, WT_VERB_COMPACT, "============ %s",
      start ? "testing for compaction" : "ending compaction pass");

    if (!start) {
        __wt_verbose_debug1(
          session, WT_VERB_COMPACT, "pages reviewed: %" PRIu64, block->compact_pages_reviewed);
        __wt_verbose_debug1(
          session, WT_VERB_COMPACT, "pages skipped: %" PRIu64, block->compact_pages_skipped);
        __wt_verbose_debug1(session, WT_VERB_COMPACT,
          "pages rewritten: %" PRIu64 " (%" PRIu64 " expected)", block->compact_pages_rewritten,
          block->compact_pages_rewritten_expected);
    }

    __wt_verbose_debug1(session, WT_VERB_COMPACT,
      "file size %" PRIuMAX "MB (%" PRIuMAX "B) with %" PRIuMAX "%% space available %" PRIuMAX
      "MB (%" PRIuMAX "B)",
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
