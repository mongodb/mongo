/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static void __bm_method_set(WT_BM *, bool);

/*
 * __bm_close_block_remove --
 *     Remove a single block handle. Must be called with the block lock held.
 */
static int
__bm_close_block_remove(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
    u_int i;

    /* Discard any references we're holding. */
    for (i = 0; i < block->related_next; ++i) {
        --block->related[i]->ref;
        block->related[i] = NULL;
    }

    /* Discard the block structure. */
    return (__wt_block_close(session, block));
}

/*
 * __bm_close_block --
 *     Close a single block handle, removing the handle if it's no longer useful.
 */
static int
__bm_close_block(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    bool found;

    conn = S2C(session);

    __wt_verbose(session, WT_VERB_BLKCACHE, "close: %s", block->name);

    __wt_spin_lock(session, &conn->block_lock);
    if (block->ref > 0 && --block->ref > 0) {
        __wt_spin_unlock(session, &conn->block_lock);
        return (0);
    }

    /* You can't close files during a checkpoint. */
    WT_ASSERT(
      session, block->ckpt_state == WT_CKPT_NONE || block->ckpt_state == WT_CKPT_PANIC_ON_FAILURE);

    /*
     * Every time we remove a block, we may have sufficiently decremented other references to allow
     * other blocks to be removed. It's unlikely for blocks to reference each other but it's not out
     * of the question, either. Loop until we don't find anything to close.
     */
    do {
        found = false;
        TAILQ_FOREACH (block, &conn->blockqh, q)
            if (block->ref == 0) {
                found = true;
                WT_TRET(__bm_close_block_remove(session, block));
                break;
            }
    } while (found);
    __wt_spin_unlock(session, &conn->block_lock);

    return (ret);
}

/*
 * __bm_readonly --
 *     General-purpose "writes not supported on this handle" function.
 */
static int
__bm_readonly(WT_BM *bm, WT_SESSION_IMPL *session)
{
    WT_RET_MSG(
      session, ENOTSUP, "%s: write operation on read-only checkpoint handle", bm->block->name);
}

/*
 * __bm_addr_invalid --
 *     Return an error code if an address cookie is invalid.
 */
static int
__bm_addr_invalid(WT_BM *bm, WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size)
{
    return (__wt_block_addr_invalid(session, bm->block, addr, addr_size, bm->is_live));
}

/*
 * __bm_addr_string --
 *     Return a printable string representation of an address cookie.
 */
static int
__bm_addr_string(
  WT_BM *bm, WT_SESSION_IMPL *session, WT_ITEM *buf, const uint8_t *addr, size_t addr_size)
{
    return (__wt_block_addr_string(session, bm->block, buf, addr, addr_size));
}

/*
 * __bm_block_header --
 *     Return the size of the block header.
 */
static u_int
__bm_block_header(WT_BM *bm)
{
    return (__wt_block_header(bm->block));
}

/*
 * __bm_checkpoint --
 *     Write a buffer into a block, creating a checkpoint.
 */
static int
__bm_checkpoint(
  WT_BM *bm, WT_SESSION_IMPL *session, WT_ITEM *buf, WT_CKPT *ckptbase, bool data_checksum)
{
    WT_BLOCK *block, *tblock;
    WT_CONNECTION_IMPL *conn;
    bool found;

    conn = S2C(session);
    block = bm->block;

    WT_RET(__wt_block_checkpoint(session, block, buf, ckptbase, data_checksum));

    /*
     * Close previous primary objects that are no longer being written, that is, ones where all
     * in-flight writes have drained. We know all writes have drained when a subsequent checkpoint
     * completes, and we know the metadata file is the last file to be checkpointed. After
     * checkpointing the metadata file, review any previous primary objects, flushing writes and
     * discarding the primary reference.
     */
    if (strcmp(WT_METAFILE, block->name) != 0)
        return (0);
    do {
        found = false;
        __wt_spin_lock(session, &conn->block_lock);
        TAILQ_FOREACH (tblock, &conn->blockqh, q)
            if (tblock->close_on_checkpoint) {
                tblock->close_on_checkpoint = false;
                __wt_spin_unlock(session, &conn->block_lock);
                found = true;
                WT_RET(__wt_fsync(session, tblock->fh, true));
                WT_RET(__bm_close_block(session, tblock));
                break;
            }
    } while (found);
    __wt_spin_unlock(session, &conn->block_lock);

    return (0);
}

/*
 * __bm_checkpoint_last --
 *     Return information for the last known file checkpoint.
 */
static int
__bm_checkpoint_last(WT_BM *bm, WT_SESSION_IMPL *session, char **metadatap, char **checkpoint_listp,
  WT_ITEM *checkpoint)
{
    return (
      __wt_block_checkpoint_last(session, bm->block, metadatap, checkpoint_listp, checkpoint));
}

/*
 * __bm_checkpoint_readonly --
 *     Write a buffer into a block, creating a checkpoint; readonly version.
 */
static int
__bm_checkpoint_readonly(
  WT_BM *bm, WT_SESSION_IMPL *session, WT_ITEM *buf, WT_CKPT *ckptbase, bool data_checksum)
{
    WT_UNUSED(buf);
    WT_UNUSED(ckptbase);
    WT_UNUSED(data_checksum);

    return (__bm_readonly(bm, session));
}

/*
 * __bm_checkpoint_load --
 *     Load a checkpoint.
 */
static int
__bm_checkpoint_load(WT_BM *bm, WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size,
  uint8_t *root_addr, size_t *root_addr_sizep, bool checkpoint)
{
    /* If not opening a checkpoint, we're opening the live system. */
    bm->is_live = !checkpoint;
    WT_RET(__wt_block_checkpoint_load(
      session, bm->block, addr, addr_size, root_addr, root_addr_sizep, checkpoint));

    if (checkpoint) {
        /*
         * Read-only objects are optionally mapped into memory instead of being read into cache
         * buffers.
         */
        WT_RET(__wt_blkcache_map(session, bm->block, &bm->map, &bm->maplen, &bm->mapped_cookie));

        /*
         * If this handle is for a checkpoint, that is, read-only, there isn't a lot you can do with
         * it. Although the btree layer prevents attempts to write a checkpoint reference, paranoia
         * is healthy.
         */
        __bm_method_set(bm, true);
    }

    return (0);
}

/*
 * __bm_checkpoint_resolve --
 *     Resolve the checkpoint.
 */
static int
__bm_checkpoint_resolve(WT_BM *bm, WT_SESSION_IMPL *session, bool failed)
{
    return (__wt_block_checkpoint_resolve(session, bm->block, failed));
}

/*
 * __bm_checkpoint_resolve_readonly --
 *     Resolve the checkpoint; readonly version.
 */
static int
__bm_checkpoint_resolve_readonly(WT_BM *bm, WT_SESSION_IMPL *session, bool failed)
{
    WT_UNUSED(failed);

    return (__bm_readonly(bm, session));
}

/*
 * __bm_checkpoint_start --
 *     Start the checkpoint.
 */
static int
__bm_checkpoint_start(WT_BM *bm, WT_SESSION_IMPL *session)
{
    return (__wt_block_checkpoint_start(session, bm->block));
}

/*
 * __bm_checkpoint_start_readonly --
 *     Start the checkpoint; readonly version.
 */
static int
__bm_checkpoint_start_readonly(WT_BM *bm, WT_SESSION_IMPL *session)
{
    return (__bm_readonly(bm, session));
}

/*
 * __bm_checkpoint_unload --
 *     Unload a checkpoint point.
 */
static int
__bm_checkpoint_unload(WT_BM *bm, WT_SESSION_IMPL *session)
{
    WT_DECL_RET;

    /* Unmap any mapped segment. */
    if (bm->map != NULL)
        WT_TRET(__wt_blkcache_unmap(session, bm->block, bm->map, bm->maplen, &bm->mapped_cookie));

    /* Unload the checkpoint. */
    WT_TRET(__wt_block_checkpoint_unload(session, bm->block, !bm->is_live));

    return (ret);
}

/*
 * __bm_close --
 *     Close a file.
 */
static int
__bm_close(WT_BM *bm, WT_SESSION_IMPL *session)
{
    WT_DECL_RET;

    if (bm == NULL) /* Safety check */
        return (0);

    ret = __bm_close_block(session, bm->block);

    __wt_overwrite_and_free(session, bm);
    return (ret);
}

/*
 * __bm_compact_end --
 *     End a block manager compaction.
 */
static int
__bm_compact_end(WT_BM *bm, WT_SESSION_IMPL *session)
{
    return (__wt_block_compact_end(session, bm->block));
}

/*
 * __bm_compact_end_readonly --
 *     End a block manager compaction; readonly version.
 */
static int
__bm_compact_end_readonly(WT_BM *bm, WT_SESSION_IMPL *session)
{
    return (__bm_readonly(bm, session));
}

/*
 * __bm_compact_page_rewrite --
 *     Rewrite a page for compaction.
 */
static int
__bm_compact_page_rewrite(
  WT_BM *bm, WT_SESSION_IMPL *session, uint8_t *addr, size_t *addr_sizep, bool *writtenp)
{
    return (__wt_block_compact_page_rewrite(session, bm->block, addr, addr_sizep, writtenp));
}

/*
 * __bm_compact_page_rewrite_readonly --
 *     Rewrite a page for compaction; readonly version.
 */
static int
__bm_compact_page_rewrite_readonly(
  WT_BM *bm, WT_SESSION_IMPL *session, uint8_t *addr, size_t *addr_sizep, bool *writtenp)
{
    WT_UNUSED(addr);
    WT_UNUSED(addr_sizep);
    WT_UNUSED(writtenp);

    return (__bm_readonly(bm, session));
}

/*
 * __bm_compact_page_skip --
 *     Return if a page is useful for compaction.
 */
static int
__bm_compact_page_skip(
  WT_BM *bm, WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size, bool *skipp)
{
    return (__wt_block_compact_page_skip(session, bm->block, addr, addr_size, skipp));
}

/*
 * __bm_compact_page_skip_readonly --
 *     Return if a page is useful for compaction; readonly version.
 */
static int
__bm_compact_page_skip_readonly(
  WT_BM *bm, WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size, bool *skipp)
{
    WT_UNUSED(addr);
    WT_UNUSED(addr_size);
    WT_UNUSED(skipp);

    return (__bm_readonly(bm, session));
}

/*
 * __bm_compact_progress --
 *     Output compact progress message.
 */
static void
__bm_compact_progress(WT_BM *bm, WT_SESSION_IMPL *session, u_int *msg_countp)
{
    __wt_block_compact_progress(session, bm->block, msg_countp);
}

/*
 * __bm_compact_skip --
 *     Return if a file can be compacted.
 */
static int
__bm_compact_skip(WT_BM *bm, WT_SESSION_IMPL *session, bool *skipp)
{
    return (__wt_block_compact_skip(session, bm->block, skipp));
}

/*
 * __bm_compact_skip_readonly --
 *     Return if a file can be compacted; readonly version.
 */
static int
__bm_compact_skip_readonly(WT_BM *bm, WT_SESSION_IMPL *session, bool *skipp)
{
    WT_UNUSED(skipp);

    return (__bm_readonly(bm, session));
}

/*
 * __bm_compact_start --
 *     Start a block manager compaction.
 */
static int
__bm_compact_start(WT_BM *bm, WT_SESSION_IMPL *session)
{
    return (__wt_block_compact_start(session, bm->block));
}

/*
 * __bm_compact_start_readonly --
 *     Start a block manager compaction; readonly version.
 */
static int
__bm_compact_start_readonly(WT_BM *bm, WT_SESSION_IMPL *session)
{
    return (__bm_readonly(bm, session));
}

/*
 * __bm_free --
 *     Free a block of space to the underlying file.
 */
static int
__bm_free(WT_BM *bm, WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size)
{
    WT_BLKCACHE *blkcache;

    blkcache = &S2C(session)->blkcache;

    /* Evict the freed block from the block cache */
    if (blkcache->type != BLKCACHE_UNCONFIGURED)
        __wt_blkcache_remove(session, addr, addr_size);

    return (__wt_block_free(session, bm->block, addr, addr_size));
}

/*
 * __bm_free_readonly --
 *     Free a block of space to the underlying file; readonly version.
 */
static int
__bm_free_readonly(WT_BM *bm, WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size)
{
    WT_UNUSED(addr);
    WT_UNUSED(addr_size);

    return (__bm_readonly(bm, session));
}

/*
 * __bm_is_mapped --
 *     Return if the file is mapped into memory.
 */
static bool
__bm_is_mapped(WT_BM *bm, WT_SESSION_IMPL *session)
{
    WT_UNUSED(session);

    return (bm->map == NULL ? false : true);
}

/*
 * __bm_map_discard --
 *     Discard a mapped segment.
 */
static int
__bm_map_discard(WT_BM *bm, WT_SESSION_IMPL *session, void *map, size_t len)
{
    WT_FILE_HANDLE *handle;

    handle = bm->block->fh->handle;
    return (handle->fh_map_discard(handle, (WT_SESSION *)session, map, len, bm->mapped_cookie));
}

/*
 * __bm_read --
 *     Read an address cookie referenced block into a buffer.
 */
static int
__bm_read(WT_BM *bm, WT_SESSION_IMPL *session, WT_ITEM *buf, const uint8_t *addr, size_t addr_size)
{
    return (__wt_bm_read(bm, session, buf, addr, addr_size));
}

/*
 * __bm_salvage_end --
 *     End a block manager salvage.
 */
static int
__bm_salvage_end(WT_BM *bm, WT_SESSION_IMPL *session)
{
    return (__wt_block_salvage_end(session, bm->block));
}

/*
 * __bm_salvage_end_readonly --
 *     End a block manager salvage; readonly version.
 */
static int
__bm_salvage_end_readonly(WT_BM *bm, WT_SESSION_IMPL *session)
{
    return (__bm_readonly(bm, session));
}

/*
 * __bm_salvage_next_readonly --
 *     Return the next block from the file; readonly version.
 */
static int
__bm_salvage_next_readonly(
  WT_BM *bm, WT_SESSION_IMPL *session, uint8_t *addr, size_t *addr_sizep, bool *eofp)
{
    WT_UNUSED(addr);
    WT_UNUSED(addr_sizep);
    WT_UNUSED(eofp);

    return (__bm_readonly(bm, session));
}

/*
 * __bm_salvage_next --
 *     Return the next block from the file.
 */
static int
__bm_salvage_next(
  WT_BM *bm, WT_SESSION_IMPL *session, uint8_t *addr, size_t *addr_sizep, bool *eofp)
{
    return (__wt_block_salvage_next(session, bm->block, addr, addr_sizep, eofp));
}

/*
 * __bm_salvage_start --
 *     Start a block manager salvage.
 */
static int
__bm_salvage_start(WT_BM *bm, WT_SESSION_IMPL *session)
{
    return (__wt_block_salvage_start(session, bm->block));
}

/*
 * __bm_salvage_start_readonly --
 *     Start a block manager salvage; readonly version.
 */
static int
__bm_salvage_start_readonly(WT_BM *bm, WT_SESSION_IMPL *session)
{
    return (__bm_readonly(bm, session));
}

/*
 * __bm_salvage_valid --
 *     Inform salvage a block is valid.
 */
static int
__bm_salvage_valid(WT_BM *bm, WT_SESSION_IMPL *session, uint8_t *addr, size_t addr_size, bool valid)
{
    return (__wt_block_salvage_valid(session, bm->block, addr, addr_size, valid));
}

/*
 * __bm_salvage_valid_readonly --
 *     Inform salvage a block is valid; readonly version.
 */
static int
__bm_salvage_valid_readonly(
  WT_BM *bm, WT_SESSION_IMPL *session, uint8_t *addr, size_t addr_size, bool valid)
{
    WT_UNUSED(addr);
    WT_UNUSED(addr_size);
    WT_UNUSED(valid);

    return (__bm_readonly(bm, session));
}

/*
 * __bm_stat --
 *     Block-manager statistics.
 */
static int
__bm_stat(WT_BM *bm, WT_SESSION_IMPL *session, WT_DSRC_STATS *stats)
{
    __wt_block_stat(session, bm->block, stats);
    return (0);
}

/*
 * __bm_switch_object --
 *     Switch the tiered object.
 */
static int
__bm_switch_object(WT_BM *bm, WT_SESSION_IMPL *session, uint32_t objectid)
{
    WT_BLOCK *block, *current;
    size_t root_addr_size;

    current = bm->block;

    /* There should not be a checkpoint in progress. */
    WT_ASSERT(session, !S2C(session)->txn_global.checkpoint_running);

    WT_RET(__wt_blkcache_tiered_open(session, NULL, objectid, &block));

    __wt_verbose(
      session, WT_VERB_TIERED, "block manager switching from %s to %s", current->name, block->name);

    /* Fast-path switching to the current object, just undo the reference count increment. */
    if (block == current)
        return (__bm_close_block(session, block));

    /* Load a new object. */
    WT_RET(__wt_block_checkpoint_load(session, block, NULL, 0, NULL, &root_addr_size, false));

    /*
     * The previous object should be closed once writes have drained.
     *
     * FIXME: the old object does not participate in the upcoming checkpoint which has a couple of
     * implications. First, the extent lists for the old object are discarded and never written,
     * which makes it impossible to treat the old object as a standalone object, so, for example,
     * you can't verify it. A solution to this is for the upper layers to checkpoint all modified
     * objects in the logical object before the checkpoint updates the metadata, flushing all
     * underlying writes to stable storage, but that means writing extent lists without a root page.
     */
    current->close_on_checkpoint = true;

    /*
     * Swap out the block manager's default handler.
     *
     * FIXME: the new block handle reasonably has different methods for objects in different backing
     * sources. That's not the case today, but the current architecture lacks the ability to support
     * multiple sources cleanly.
     *
     * FIXME: it should not be possible for a thread of control to copy the WT_BM value in the btree
     * layer, sleep until after a subsequent switch and a subsequent a checkpoint that would discard
     * the WT_BM it copied, but it would be worth thinking through those scenarios in detail to be
     * sure there aren't any races.
     */
    bm->block = block;
    return (0);
}

/*
 * __bm_switch_object_readonly --
 *     Switch the tiered object; readonly version.
 */
static int
__bm_switch_object_readonly(WT_BM *bm, WT_SESSION_IMPL *session, uint32_t objectid)
{
    WT_UNUSED(objectid);

    return (__bm_readonly(bm, session));
}

/*
 * __bm_sync --
 *     Flush a file to disk.
 */
static int
__bm_sync(WT_BM *bm, WT_SESSION_IMPL *session, bool block)
{
    return (__wt_fsync(session, bm->block->fh, block));
}

/*
 * __bm_sync_readonly --
 *     Flush a file to disk; readonly version.
 */
static int
__bm_sync_readonly(WT_BM *bm, WT_SESSION_IMPL *session, bool async)
{
    WT_UNUSED(async);

    return (__bm_readonly(bm, session));
}

/*
 * __bm_verify_addr --
 *     Verify an address.
 */
static int
__bm_verify_addr(WT_BM *bm, WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size)
{
    return (__wt_block_verify_addr(session, bm->block, addr, addr_size));
}

/*
 * __bm_verify_end --
 *     End a block manager verify.
 */
static int
__bm_verify_end(WT_BM *bm, WT_SESSION_IMPL *session)
{
    return (__wt_block_verify_end(session, bm->block));
}

/*
 * __bm_verify_start --
 *     Start a block manager verify.
 */
static int
__bm_verify_start(WT_BM *bm, WT_SESSION_IMPL *session, WT_CKPT *ckptbase, const char *cfg[])
{
    return (__wt_block_verify_start(session, bm->block, ckptbase, cfg));
}

/*
 * __bm_write --
 *     Write a buffer into a block, returning the block's address cookie.
 */
static int
__bm_write(WT_BM *bm, WT_SESSION_IMPL *session, WT_ITEM *buf, uint8_t *addr, size_t *addr_sizep,
  bool data_checksum, bool checkpoint_io)
{
    __wt_capacity_throttle(
      session, buf->size, checkpoint_io ? WT_THROTTLE_CKPT : WT_THROTTLE_EVICT);

    return (
      __wt_block_write(session, bm->block, buf, addr, addr_sizep, data_checksum, checkpoint_io));
}

/*
 * __bm_write_readonly --
 *     Write a buffer into a block, returning the block's address cookie; readonly version.
 */
static int
__bm_write_readonly(WT_BM *bm, WT_SESSION_IMPL *session, WT_ITEM *buf, uint8_t *addr,
  size_t *addr_sizep, bool data_checksum, bool checkpoint_io)
{
    WT_UNUSED(buf);
    WT_UNUSED(addr);
    WT_UNUSED(addr_sizep);
    WT_UNUSED(data_checksum);
    WT_UNUSED(checkpoint_io);

    return (__bm_readonly(bm, session));
}

/*
 * __bm_write_size --
 *     Return the buffer size required to write a block.
 */
static int
__bm_write_size(WT_BM *bm, WT_SESSION_IMPL *session, size_t *sizep)
{
    return (__wt_block_write_size(session, bm->block, sizep));
}

/*
 * __bm_write_size_readonly --
 *     Return the buffer size required to write a block; readonly version.
 */
static int
__bm_write_size_readonly(WT_BM *bm, WT_SESSION_IMPL *session, size_t *sizep)
{
    WT_UNUSED(sizep);

    return (__bm_readonly(bm, session));
}

/*
 * __bm_method_set --
 *     Set up the legal methods.
 */
static void
__bm_method_set(WT_BM *bm, bool readonly)
{
    bm->addr_invalid = __bm_addr_invalid;
    bm->addr_string = __bm_addr_string;
    bm->block_header = __bm_block_header;
    bm->checkpoint = __bm_checkpoint;
    bm->checkpoint_last = __bm_checkpoint_last;
    bm->checkpoint_load = __bm_checkpoint_load;
    bm->checkpoint_resolve = __bm_checkpoint_resolve;
    bm->checkpoint_start = __bm_checkpoint_start;
    bm->checkpoint_unload = __bm_checkpoint_unload;
    bm->close = __bm_close;
    bm->compact_end = __bm_compact_end;
    bm->compact_page_rewrite = __bm_compact_page_rewrite;
    bm->compact_page_skip = __bm_compact_page_skip;
    bm->compact_progress = __bm_compact_progress;
    bm->compact_skip = __bm_compact_skip;
    bm->compact_start = __bm_compact_start;
    bm->corrupt = __wt_bm_corrupt;
    bm->free = __bm_free;
    bm->is_mapped = __bm_is_mapped;
    bm->map_discard = __bm_map_discard;
    bm->read = __bm_read;
    bm->salvage_end = __bm_salvage_end;
    bm->salvage_next = __bm_salvage_next;
    bm->salvage_start = __bm_salvage_start;
    bm->salvage_valid = __bm_salvage_valid;
    bm->size = __wt_block_manager_size;
    bm->stat = __bm_stat;
    bm->switch_object = __bm_switch_object;
    bm->sync = __bm_sync;
    bm->verify_addr = __bm_verify_addr;
    bm->verify_end = __bm_verify_end;
    bm->verify_start = __bm_verify_start;
    bm->write = __bm_write;
    bm->write_size = __bm_write_size;

    if (readonly) {
        bm->checkpoint = __bm_checkpoint_readonly;
        bm->checkpoint_resolve = __bm_checkpoint_resolve_readonly;
        bm->checkpoint_start = __bm_checkpoint_start_readonly;
        bm->compact_end = __bm_compact_end_readonly;
        bm->compact_page_rewrite = __bm_compact_page_rewrite_readonly;
        bm->compact_page_skip = __bm_compact_page_skip_readonly;
        bm->compact_skip = __bm_compact_skip_readonly;
        bm->compact_start = __bm_compact_start_readonly;
        bm->free = __bm_free_readonly;
        bm->salvage_end = __bm_salvage_end_readonly;
        bm->salvage_next = __bm_salvage_next_readonly;
        bm->salvage_start = __bm_salvage_start_readonly;
        bm->salvage_valid = __bm_salvage_valid_readonly;
        bm->switch_object = __bm_switch_object_readonly;
        bm->sync = __bm_sync_readonly;
        bm->write = __bm_write_readonly;
        bm->write_size = __bm_write_size_readonly;
    }
}

/*
 * __wt_blkcache_open --
 *     Open a file.
 */
int
__wt_blkcache_open(WT_SESSION_IMPL *session, const char *uri, const char *cfg[],
  bool forced_salvage, bool readonly, uint32_t allocsize, WT_BM **bmp)
{
    WT_BM *bm;
    WT_DECL_RET;

    *bmp = NULL;

    __wt_verbose(session, WT_VERB_BLKCACHE, "open: %s", uri);

    WT_RET(__wt_calloc_one(session, &bm));
    __bm_method_set(bm, false);

    if (WT_PREFIX_MATCH(uri, "file:")) {
        uri += strlen("file:");
        WT_ERR(__wt_block_open(session, uri, WT_TIERED_OBJECTID_NONE, cfg, forced_salvage, readonly,
          false, allocsize, &bm->block));
    } else
        WT_ERR(__wt_blkcache_tiered_open(session, uri, 0, &bm->block));

    *bmp = bm;
    return (0);

err:
    __wt_free(session, bm);
    return (ret);
}

/*
 * __wt_blkcache_set_readonly --
 *     Set the block API to read-only.
 */
void
__wt_blkcache_set_readonly(WT_SESSION_IMPL *session) WT_GCC_FUNC_ATTRIBUTE((cold))
{
    /* Switch the handle into read-only mode. */
    __bm_method_set(S2BT(session)->bm, true);
}
