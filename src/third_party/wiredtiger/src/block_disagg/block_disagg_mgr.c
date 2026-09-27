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
 * __bmd_block_header_init --
 *     Initialize the block header of a disk image laid out for writing.
 */
static void
__bmd_block_header_init(WT_BM *bm, WT_SESSION_IMPL *session, void *dsk)
{
    WT_UNUSED(bm);

    memset(WT_BLOCK_HEADER_REF(dsk), 0, S2BT(session)->block_header_write_size);
    __wti_block_disagg_header_init(session, WT_BLOCK_HEADER_REF(dsk));
}

/*
 * __bmd_block_header_read --
 *     Return the size of the block header when reading an existing header.
 */
static u_int
__bmd_block_header_read(WT_BM *bm, WT_SESSION_IMPL *session, const void *dsk)
{
    const WT_BLOCK_DISAGG_HEADER *header;

    WT_UNUSED(bm);

    /* Get the block disaggregated header from the disk image. */
    header = (const WT_BLOCK_DISAGG_HEADER *)(((const uint8_t *)dsk) + WT_PAGE_HEADER_SIZE);
    WT_ASSERT(session,
      header->magic == WT_BLOCK_DISAGG_MAGIC_BASE || header->magic == WT_BLOCK_DISAGG_MAGIC_DELTA);

    /* The stored size covers the page header as well; see the WT_BLOCK_DISAGG_HEADER definition. */
    WT_ASSERT(session, header->combined_header_size >= WT_BLOCK_DISAGG_HEADER_MIN_COMBINED_SIZE);
    return ((u_int)header->combined_header_size - WT_PAGE_HEADER_SIZE);
}

/*
 * __bmd_block_header_write --
 *     Return the size of the block header when writing a new header.
 */
static u_int
__bmd_block_header_write(WT_BM *bm, WT_SESSION_IMPL *session)
{
    WT_UNUSED(bm);

    return (__wti_block_disagg_header_write_size(session));
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
__bmd_free(WT_BM *bm, WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size, bool is_root)
{
    WT_BLKCACHE *blkcache;
    WT_DECL_RET;

    blkcache = &S2C(session)->blkcache;

    ret = __wti_block_disagg_page_discard(
      session, (WT_BLOCK_DISAGG *)bm->block, addr, addr_size, is_root);

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
    return (__wti_block_disagg_stat(session, (WT_BLOCK_DISAGG *)bm->block, stats));
}

/*
 * __bmd_write --
 *     Write a buffer into a block, returning the block's address cookie.
 */
static int
__bmd_write(WT_BM *bm, WT_SESSION_IMPL *session, WT_ITEM *buf, WT_PAGE_BLOCK_META *block_meta,
  size_t page_image_size, uint8_t *addr, size_t *addr_sizep, bool data_checksum, bool checkpoint_io)
{
    __wt_capacity_throttle(
      session, buf->size, checkpoint_io ? WT_THROTTLE_CKPT : WT_THROTTLE_EVICT);
    return (__wti_block_disagg_write(session, bm->block, buf, block_meta, page_image_size, addr,
      addr_sizep, data_checksum, checkpoint_io));
}

/*
 * __bmd_write_size --
 *     Return the buffer size required to write a block.
 */
static int
__bmd_write_size(WT_BM *bm, WT_SESSION_IMPL *session, size_t *sizep)
{
    WT_UNUSED(bm);

    return (__wti_block_disagg_write_size(session, sizep));
}

/*
 * __bmd_encrypt_skip_size --
 *     Return the skip size for encryption
 */
static size_t
__bmd_encrypt_skip_size(WT_BM *bm, WT_SESSION_IMPL *session, const void *dsk)
{
    /*
     * Encryption skips from the start of the image, so the page header counts towards the skip as
     * well as the block header.
     */
    return ((size_t)WT_PAGE_HEADER_SIZE + __bmd_block_header_read(bm, session, dsk));
}

/*
 * __bmd_get_page_ids --
 *     Get all page IDs for the given checkpoint.
 */
static int
__bmd_get_page_ids(
  WT_BM *bm, WT_SESSION_IMPL *session, WT_ITEM *item, size_t *size, uint64_t checkpoint_lsn)
{
    WT_BLOCK_DISAGG *block_disagg;

    /* FIXME-WT-15564: Check block is safe to cast as disagg block. */
    WT_ASSERT(session, F_ISSET(S2BT(session), WT_BTREE_DISAGGREGATED));
    block_disagg = (WT_BLOCK_DISAGG *)bm->block;

    if (block_disagg->plhandle->plh_get_page_ids == NULL) {
        __wt_verbose_warning(
          session, WT_VERB_DISAGGREGATED_STORAGE, "%s", "plh_get_page_ids is not implemented");
        return (0);
    }

    WT_RET(block_disagg->plhandle->plh_get_page_ids(
      block_disagg->plhandle, &session->iface, checkpoint_lsn, item, size));

    return (0);
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
    bm->block_header_init = __bmd_block_header_init;
    bm->block_header_read_size = __bmd_block_header_read;
    bm->block_header_write_size = __bmd_block_header_write;
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
    bm->get_page_ids = __bmd_get_page_ids;
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

    WT_ASSERT_ALWAYS(session, ((WT_BLOCK_DISAGG *)bm->block)->tableid == S2BT(session)->id,
      "block manager bound to a block handle for a different table");

    *bmp = bm;
    return (0);

err:
    WT_TRET(bm->close(bm, session));
    return (ret);
}

#ifdef HAVE_UNITTEST
/*
 * __ut_bmd_block_header_read --
 *     Unit-test wrapper for __bmd_block_header_read.
 */
u_int
__ut_bmd_block_header_read(WT_BM *bm, WT_SESSION_IMPL *session, const void *dsk)
{
    return (__bmd_block_header_read(bm, session, dsk));
}
#endif
