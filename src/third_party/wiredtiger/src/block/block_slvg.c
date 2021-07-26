/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_block_salvage_start --
 *     Start a file salvage.
 */
int
__wt_block_salvage_start(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
    wt_off_t len;
    uint32_t allocsize;

    allocsize = block->allocsize;

    /* Reset the description information in the first block. */
    WT_RET(__wt_desc_write(session, block->fh, allocsize));

    /*
     * Salvage creates a new checkpoint when it's finished, set up for rolling an empty file
     * forward.
     */
    WT_RET(__wt_block_ckpt_init(session, &block->live, "live"));

    /*
     * Truncate the file to an allocation-size multiple of blocks (bytes trailing the last block
     * must be garbage, by definition).
     */
    len = allocsize;
    if (block->size > allocsize)
        len = (block->size / allocsize) * allocsize;
    WT_RET(__wt_block_truncate(session, block, len));

    /*
     * The file's first allocation-sized block is description information, skip it when reading
     * through the file.
     */
    block->slvg_off = allocsize;

    /*
     * The only checkpoint extent we care about is the allocation list. Start with the entire file
     * on the allocation list, we'll "free" any blocks we don't want as we process the file.
     */
    WT_RET(__wt_block_insert_ext(session, block, &block->live.alloc, allocsize, len - allocsize));

    /* Salvage performs a checkpoint but doesn't start or resolve it. */
    WT_ASSERT(session, block->ckpt_state == WT_CKPT_NONE);
    block->ckpt_state = WT_CKPT_SALVAGE;

    return (0);
}

/*
 * __wt_block_salvage_end --
 *     End a file salvage.
 */
int
__wt_block_salvage_end(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
    /* Salvage performs a checkpoint but doesn't start or resolve it. */
    WT_ASSERT(session, block->ckpt_state == WT_CKPT_SALVAGE);
    block->ckpt_state = WT_CKPT_NONE;

    /* Discard the checkpoint. */
    return (__wt_block_checkpoint_unload(session, block, false));
}

/*
 * __wt_block_offset_invalid --
 *     Return if the block offset is insane.
 */
bool
__wt_block_offset_invalid(WT_BLOCK *block, wt_off_t offset, uint32_t size)
{
    if (size == 0) /* < minimum page size */
        return (true);
    if (size % block->allocsize != 0) /* not allocation-size units */
        return (true);
    if (size > WT_BTREE_PAGE_SIZE_MAX) /* > maximum page size */
        return (true);
    /* past end-of-file */
    if (offset + (wt_off_t)size > block->size)
        return (true);
    return (false);
}

/*
 * __wt_block_salvage_next --
 *     Return the address for the next potential block from the file.
 */
int
__wt_block_salvage_next(
  WT_SESSION_IMPL *session, WT_BLOCK *block, uint8_t *addr, size_t *addr_sizep, bool *eofp)
{
    WT_BLOCK_HEADER *blk;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    WT_FH *fh;
    wt_off_t max, offset;
    uint32_t allocsize, checksum, objectid, size;
    uint8_t *endp;

    *eofp = 0;

    /* Salvage isn't implemented (yet) for tiered trees. */
    objectid = 0;

    fh = block->fh;
    allocsize = block->allocsize;
    WT_ERR(__wt_scr_alloc(session, allocsize, &tmp));

    /* Read through the file, looking for pages. */
    for (max = block->size;;) {
        offset = block->slvg_off;
        if (offset >= max) { /* Check eof. */
            *eofp = 1;
            goto done;
        }

        /*
         * Read the start of a possible page (an allocation-size block), and get a page length from
         * it. Move to the next allocation sized boundary, we'll never consider this one again.
         */
        WT_ERR(__wt_read(session, fh, offset, (size_t)allocsize, tmp->mem));
        blk = WT_BLOCK_HEADER_REF(tmp->mem);
        __wt_block_header_byteswap(blk);
        size = blk->disk_size;
        checksum = blk->checksum;

        /*
         * Check the block size: if it's not insane, read the block. Reading the block validates any
         * checksum; if reading the block succeeds, return its address as a possible page,
         * otherwise, move past it.
         */
        if (!__wt_block_offset_invalid(block, offset, size) &&
          __wt_block_read_off(session, block, tmp, objectid, offset, size, checksum) == 0)
            break;

        /* Free the allocation-size block. */
        __wt_verbose(session, WT_VERB_SALVAGE, "skipping %" PRIu32 "B at file offset %" PRIuMAX,
          allocsize, (uintmax_t)offset);
        WT_ERR(__wt_block_off_free(session, block, objectid, offset, (wt_off_t)allocsize));
        block->slvg_off += allocsize;
    }

    /* Re-create the address cookie that should reference this block. */
    endp = addr;
    WT_ERR(__wt_block_addr_pack(block, &endp, objectid, offset, size, checksum));
    *addr_sizep = WT_PTRDIFF(endp, addr);

done:
err:
    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * __wt_block_salvage_valid --
 *     Let salvage know if a block is valid.
 */
int
__wt_block_salvage_valid(
  WT_SESSION_IMPL *session, WT_BLOCK *block, uint8_t *addr, size_t addr_size, bool valid)
{
    wt_off_t offset;
    uint32_t size, objectid, checksum;

    /*
     * Crack the cookie. If the upper layer took the block, move past it; if the upper layer
     * rejected the block, move past an allocation size chunk and free it.
     */
    WT_RET(__wt_block_addr_unpack(
      session, block, addr, addr_size, &objectid, &offset, &size, &checksum));
    if (valid)
        block->slvg_off = offset + size;
    else {
        WT_RET(__wt_block_off_free(session, block, objectid, offset, (wt_off_t)block->allocsize));
        block->slvg_off = offset + block->allocsize;
    }

    return (0);
}
