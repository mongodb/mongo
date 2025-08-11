/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __bmd_addr_invalid --
 *     Return an error code if an address cookie is invalid.
 */
static int
__bmd_addr_invalid(WT_BM *bm, WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size)
{
    WT_UNUSED(bm);
    WT_UNUSED(session);

    return (__wti_block_disagg_addr_invalid(session, addr, addr_size));
}

/*
 * __bmd_block_header --
 *     Return the size of the block header.
 */
static u_int
__bmd_block_header(WT_BM *bm)
{
    WT_UNUSED(bm);

    return ((u_int)WT_BLOCK_DISAGG_HEADER_SIZE);
}

/*
 * __bmd_can_truncate --
 *     Nominally whether there's free space at the end of the file; useless in disagg.
 */
static bool
__bmd_can_truncate(WT_BM *bm, WT_SESSION_IMPL *session)
{
    WT_UNUSED(bm);
    WT_UNUSED(session);

    return (false);
}

/*
 * __bmd_close --
 *     Close a file.
 */
static int
__bmd_close(WT_BM *bm, WT_SESSION_IMPL *session)
{
    WT_DECL_RET;

    if (bm == NULL) /* Safety check */
        return (0);

    ret = __wti_block_disagg_close(session, (WT_BLOCK_DISAGG *)bm->block);

    __wt_overwrite_and_free(session, bm);
    return (ret);
}

/*
 * __bmd_free --
 *     Free a block of space to the underlying file.
 */
static int
__bmd_free(WT_BM *bm, WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size)
{
    WT_BLKCACHE *blkcache;
    WT_DECL_RET;

    blkcache = &S2C(session)->blkcache;

    ret = __wti_block_disagg_page_discard(session, (WT_BLOCK_DISAGG *)bm->block, addr, addr_size);

    /* Evict the freed block from the block cache */
    if (ret == 0 && blkcache->type != WT_BLKCACHE_UNCONFIGURED)
        __wt_blkcache_remove(session, addr, addr_size);

    return (ret);
}

/*
 * __bmd_stat --
 *     Block-manager statistics.
 */
static int
__bmd_stat(WT_BM *bm, WT_SESSION_IMPL *session, WT_DSRC_STATS *stats)
{
    __wti_block_disagg_stat(session, (WT_BLOCK_DISAGG *)bm->block, stats);
    return (0);
}

/*
 * __bmd_write --
 *     Write a buffer into a block, returning the block's address cookie.
 */
static int
__bmd_write(WT_BM *bm, WT_SESSION_IMPL *session, WT_ITEM *buf, WT_PAGE_BLOCK_META *block_meta,
  uint8_t *addr, size_t *addr_sizep, bool data_checksum, bool checkpoint_io)
{
    __wt_capacity_throttle(
      session, buf->size, checkpoint_io ? WT_THROTTLE_CKPT : WT_THROTTLE_EVICT);
    return (__wti_block_disagg_write(
      session, bm->block, buf, block_meta, addr, addr_sizep, data_checksum, checkpoint_io));
}

/*
 * __bmd_write_size --
 *     Return the buffer size required to write a block.
 */
static int
__bmd_write_size(WT_BM *bm, WT_SESSION_IMPL *session, size_t *sizep)
{
    WT_UNUSED(bm);
    WT_UNUSED(session);

    return (__wti_block_disagg_write_size(sizep));
}

/*
 * __bmd_encrypt_skip_size --
 *     Return the skip size for encryption
 */
static size_t
__bmd_encrypt_skip_size(WT_BM *bm, WT_SESSION_IMPL *session)
{
    WT_UNUSED(bm);
    WT_UNUSED(session);

    return (WT_BLOCK_DISAGG_HEADER_BYTE_SIZE);
}

/*
 * __bmd_method_set --
 *     Set up the legal methods.
 */
static void
__bmd_method_set(WT_BM *bm, bool readonly)
{
    WT_UNUSED(readonly);

    bm->addr_invalid = __bmd_addr_invalid;
    bm->addr_string = __wti_block_disagg_addr_string;
    bm->block_header = __bmd_block_header;
    bm->can_truncate = __bmd_can_truncate;
    bm->checkpoint = __wti_block_disagg_checkpoint;
    bm->checkpoint_load = __wti_block_disagg_checkpoint_load;
    bm->checkpoint_resolve = __wti_block_disagg_checkpoint_resolve;
    bm->checkpoint_start = __wti_block_disagg_checkpoint_start;
    bm->checkpoint_unload = __wti_block_disagg_checkpoint_unload;
    bm->close = __bmd_close;
    bm->compact_end = __wti_block_disagg_compact_end;
    bm->compact_page_skip = __wti_block_disagg_compact_page_skip;
    bm->compact_skip = __wti_block_disagg_compact_skip;
    bm->compact_start = __wti_block_disagg_compact_start;
    bm->corrupt = __wti_block_disagg_corrupt;
    bm->free = __bmd_free;
    bm->is_mapped = __wti_block_disagg_is_mapped;
    bm->map_discard = __wti_block_disagg_map_discard;
    bm->read = __wti_block_disagg_read;
    bm->read_multiple = __wti_block_disagg_read_multiple;
    bm->salvage_end = __wti_block_disagg_salvage_end;
    bm->salvage_next = __wti_block_disagg_salvage_next;
    bm->salvage_start = __wti_block_disagg_salvage_start;
    bm->salvage_valid = __wti_block_disagg_salvage_valid;
    bm->size = __wti_block_disagg_manager_size;
    bm->stat = __bmd_stat;
    bm->sync = __wti_block_disagg_sync;
    bm->verify_addr = __wti_block_disagg_verify_addr;
    bm->verify_end = __wti_block_disagg_verify_end;
    bm->verify_start = __wti_block_disagg_verify_start;
    bm->write = __bmd_write;
    bm->write_size = __bmd_write_size;
    bm->encrypt_skip = __bmd_encrypt_skip_size;
}

/*
 * __wt_block_disagg_manager_owns_object --
 *     Check whether the object being opened should be managed by this block manager.
 */
bool
__wt_block_disagg_manager_owns_object(WT_SESSION_IMPL *session, const char *uri)
{
    /*
     * It's a check that should be made better, but assume any handle with a page log belongs to
     * this object-based block manager for now.
     */
    if (session->dhandle == NULL || S2BT(session) == NULL)
        return (false);
    if (WT_PREFIX_MATCH(uri, "file:") && (S2BT(session)->page_log != NULL))
        return (true);
    return (false);
}

/*
 * __wt_block_disagg_manager_open --
 *     Open a file.
 */
int
__wt_block_disagg_manager_open(WT_SESSION_IMPL *session, const char *uri, const char *cfg[],
  bool forced_salvage, bool readonly, WT_BM **bmp)
{
    WT_BM *bm;
    WT_DECL_RET;

    *bmp = NULL;

    WT_RET(__wt_calloc_one(session, &bm));
    bm->is_remote = true;

    __bmd_method_set(bm, false);

    uri += strlen("file:");

    WT_ERR(__wti_block_disagg_open(session, uri, cfg, forced_salvage, readonly, &bm->block));

    *bmp = bm;
    return (0);

err:
    WT_TRET(bm->close(bm, session));
    return (ret);
}
