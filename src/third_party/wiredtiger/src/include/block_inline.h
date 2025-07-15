/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*
 * WiredTiger's block manager interface.
 */

/*
 * Define functions that increment histogram statistics for block manager operations latency.
 */
WT_STAT_MSECS_HIST_INCR_FUNC(bmread, perf_hist_bmread_latency)
WT_STAT_MSECS_HIST_INCR_FUNC(bmwrite, perf_hist_bmwrite_latency)
WT_STAT_USECS_HIST_INCR_FUNC(disaggbmread, perf_hist_disaggbmread_latency)
WT_STAT_USECS_HIST_INCR_FUNC(disaggbmwrite, perf_hist_disaggbmwrite_latency)

/*
 * __wt_extlist_write_pair --
 *     Write an extent list pair.
 */
static WT_INLINE int
__wt_extlist_write_pair(uint8_t **p, wt_off_t off, wt_off_t size)
{
    WT_RET(__wt_vpack_uint(p, 0, (uint64_t)(off)));
    WT_RET(__wt_vpack_uint(p, 0, (uint64_t)(size)));
    return (0);
}

/*
 * __wt_extlist_read_pair --
 *     Read an extent list pair.
 */
static WT_INLINE int
__wt_extlist_read_pair(const uint8_t **p, wt_off_t *offp, wt_off_t *sizep)
{
    uint64_t v;

    WT_RET(__wt_vunpack_uint(p, 0, &v));
    *offp = (wt_off_t)v;
    WT_RET(__wt_vunpack_uint(p, 0, &v));
    *sizep = (wt_off_t)v;
    return (0);
}

/*
 * __wt_block_header_byteswap_copy --
 *     Handle big- and little-endian transformation of a header block, copying from a source to a
 *     target.
 */
static WT_INLINE void
__wt_block_header_byteswap_copy(WT_BLOCK_HEADER *from, WT_BLOCK_HEADER *to)
{
    *to = *from;
#ifdef WORDS_BIGENDIAN
    to->disk_size = __wt_bswap32(from->disk_size);
    to->checksum = __wt_bswap32(from->checksum);
#endif
}

/*
 * __wt_block_header_byteswap --
 *     Handle big- and little-endian transformation of a header block.
 */
static WT_INLINE void
__wt_block_header_byteswap(WT_BLOCK_HEADER *blk)
{
#ifdef WORDS_BIGENDIAN
    __wt_block_header_byteswap_copy(blk, blk);
#else
    WT_UNUSED(blk);
#endif
}

/*
 * __wt_block_header --
 *     Return the size of the block-specific header.
 */
static WT_INLINE u_int
__wt_block_header(WT_BLOCK *block)
{
    WT_UNUSED(block);

    return ((u_int)WT_BLOCK_HEADER_SIZE);
}

/*
 * __wt_block_eligible_for_sweep --
 *     Return true if the block meets requirements for sweeping. The check that read reference count
 *     is zero is made elsewhere.
 */
static WT_INLINE bool
__wt_block_eligible_for_sweep(WT_BM *bm, WT_BLOCK *block)
{
    return (!block->remote && block->objectid <= bm->max_flushed_objectid);
}
