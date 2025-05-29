/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wti_block_disagg_checkpoint_start --
 *     Start the checkpoint.
 */
int
__wti_block_disagg_checkpoint_start(WT_BM *bm, WT_SESSION_IMPL *session)
{
    WT_UNUSED(bm);
    WT_UNUSED(session);
    return (0);
}

/*
 * __wti_block_disagg_checkpoint_unload --
 *     Unload a checkpoint point.
 */
int
__wti_block_disagg_checkpoint_unload(WT_BM *bm, WT_SESSION_IMPL *session)
{
    WT_UNUSED(bm);
    WT_UNUSED(session);
    return (0);
}

/*
 * __wti_block_disagg_compact_end --
 *     End a block manager compaction.
 */
int
__wti_block_disagg_compact_end(WT_BM *bm, WT_SESSION_IMPL *session)
{
    WT_UNUSED(bm);
    WT_UNUSED(session);
    return (0);
}

/*
 * __wti_block_disagg_compact_page_skip --
 *     Return if a page is useful for compaction.
 */
int
__wti_block_disagg_compact_page_skip(
  WT_BM *bm, WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size, bool *skipp)
{
    WT_UNUSED(bm);
    WT_UNUSED(session);
    WT_UNUSED(addr);
    WT_UNUSED(addr_size);
    WT_UNUSED(skipp);
    return (0);
}

/*
 * __wti_block_disagg_compact_skip --
 *     Return if a file can be compacted.
 */
int
__wti_block_disagg_compact_skip(WT_BM *bm, WT_SESSION_IMPL *session, bool *skipp)
{
    WT_UNUSED(bm);
    WT_UNUSED(session);
    WT_UNUSED(skipp);
    return (0);
}

/*
 * __wti_block_disagg_compact_start --
 *     Start a block manager compaction.
 */
int
__wti_block_disagg_compact_start(WT_BM *bm, WT_SESSION_IMPL *session)
{
    WT_UNUSED(bm);
    WT_UNUSED(session);
    return (0);
}

/*
 * __wti_block_disagg_is_mapped --
 *     Return if the file is mapped into memory.
 */
bool
__wti_block_disagg_is_mapped(WT_BM *bm, WT_SESSION_IMPL *session)
{
    WT_UNUSED(bm);
    WT_UNUSED(session);
    return (false);
}

/*
 * __wti_block_disagg_map_discard --
 *     Discard a mapped segment.
 */
int
__wti_block_disagg_map_discard(WT_BM *bm, WT_SESSION_IMPL *session, void *map, size_t len)
{
    WT_UNUSED(bm);
    WT_UNUSED(session);
    WT_UNUSED(map);
    WT_UNUSED(len);
    return (0);
}

/*
 * __wti_block_disagg_salvage_end --
 *     End a block manager salvage.
 */
int
__wti_block_disagg_salvage_end(WT_BM *bm, WT_SESSION_IMPL *session)
{
    WT_UNUSED(bm);
    WT_UNUSED(session);
    return (0);
}

/*
 * __wti_block_disagg_salvage_next --
 *     Return the next block from the file.
 */
int
__wti_block_disagg_salvage_next(
  WT_BM *bm, WT_SESSION_IMPL *session, uint8_t *addr, size_t *addr_sizep, bool *eofp)
{
    WT_UNUSED(bm);
    WT_UNUSED(session);
    WT_UNUSED(addr);
    WT_UNUSED(addr_sizep);
    WT_UNUSED(eofp);
    return (0);
}

/*
 * __wti_block_disagg_salvage_start --
 *     Start a block manager salvage.
 */
int
__wti_block_disagg_salvage_start(WT_BM *bm, WT_SESSION_IMPL *session)
{
    WT_UNUSED(bm);
    WT_UNUSED(session);
    return (0);
}

/*
 * __wti_block_disagg_salvage_valid --
 *     Inform salvage a block is valid.
 */
int
__wti_block_disagg_salvage_valid(
  WT_BM *bm, WT_SESSION_IMPL *session, uint8_t *addr, size_t addr_size, bool valid)
{
    WT_UNUSED(bm);
    WT_UNUSED(session);
    WT_UNUSED(addr);
    WT_UNUSED(addr_size);
    WT_UNUSED(valid);
    return (0);
}

/*
 * __wti_block_disagg_sync --
 *     Flush a file to disk.
 */
int
__wti_block_disagg_sync(WT_BM *bm, WT_SESSION_IMPL *session, bool block)
{
    WT_UNUSED(bm);
    WT_UNUSED(session);
    WT_UNUSED(block);
    return (0);
}

/*
 * __wti_block_disagg_verify_addr --
 *     Verify an address.
 */
int
__wti_block_disagg_verify_addr(
  WT_BM *bm, WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size)
{
    WT_UNUSED(bm);
    WT_UNUSED(session);
    WT_UNUSED(addr);
    WT_UNUSED(addr_size);
    return (0);
}

/*
 * __wti_block_disagg_verify_end --
 *     End a block manager verify.
 */
int
__wti_block_disagg_verify_end(WT_BM *bm, WT_SESSION_IMPL *session)
{
    WT_UNUSED(bm);
    WT_UNUSED(session);
    return (0);
}

/*
 * __wti_block_disagg_verify_start --
 *     Start a block manager verify.
 */
int
__wti_block_disagg_verify_start(
  WT_BM *bm, WT_SESSION_IMPL *session, WT_CKPT *ckptbase, const char *cfg[])
{
    WT_UNUSED(bm);
    WT_UNUSED(session);
    WT_UNUSED(ckptbase);
    WT_UNUSED(cfg);
    return (0);
}
