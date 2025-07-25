/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __bm_sync_tiered_handles(WT_BM *, WT_SESSION_IMPL *);

/*
 * __wti_bm_close_block --
 *     Close a block handle.
 */
int
__wti_bm_close_block(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
    WT_CONNECTION_IMPL *conn;
    uint64_t bucket, hash;

    __wt_verbose(session, WT_VERB_BLKCACHE, "block close: %s", block->name);

    conn = S2C(session);
    __wt_spin_lock(session, &conn->block_lock);
    if (block->ref > 0 && --block->ref > 0) {
        __wt_spin_unlock(session, &conn->block_lock);
        return (0);
    }

    /* Make the block unreachable. */
    hash = __wt_hash_city64(block->name, strlen(block->name));
    bucket = hash & (conn->hash_size - 1);
    WT_CONN_BLOCK_REMOVE(conn, block, bucket);
    __wt_spin_unlock(session, &conn->block_lock);

    /* You can't close files during a checkpoint. */
    WT_ASSERT(
      session, block->ckpt_state == WT_CKPT_NONE || block->ckpt_state == WT_CKPT_PANIC_ON_FAILURE);

    if (block->sync_on_checkpoint) {
        WT_RET(__wt_fsync(session, block->fh, true));
        block->sync_on_checkpoint = false;
    }

    /* If fsync fails WT panics so failure to reach __wt_block_close() is irrelevant. */
    return (__wt_block_close(session, block));
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
 * __bm_sync_tiered_handles --
 *     Ensure that tiered object handles are synced to disk.
 */
static int
__bm_sync_tiered_handles(WT_BM *bm, WT_SESSION_IMPL *session)
{
    WT_BLOCK *block;
    WT_DECL_RET;
    u_int i;
    int fsync_ret;
    bool found, last_release, need_sweep;

    need_sweep = false;

    /*
     * For tiered tables, we need to fsync any previous active files to ensure the full checkpoint
     * is written. We wait until now because there may have been in-progress writes to old files.
     * But now we know those writes must have completed. Checkpoint ensures that all dirty pages of
     * the tree have been written and eviction is disabled at this point, so no new data is getting
     * written.
     *
     * We don't hold the handle array lock across fsync calls since those could be slow and that
     * would block a concurrent thread opening a new block handle. To guard against the block being
     * swept, we retain a read reference during the sync.
     */
    do {
        found = false;
        block = NULL;
        __wt_readlock(session, &bm->handle_array_lock);
        for (i = 0; i < bm->handle_array_next; ++i) {
            block = bm->handle_array[i];
            if (block->sync_on_checkpoint) {
                found = true;
                break;
            }
        }
        if (found)
            __wti_blkcache_get_read_handle(block);
        __wt_readunlock(session, &bm->handle_array_lock);

        if (found) {
            fsync_ret = __wt_fsync(session, block->fh, true);
            __wt_blkcache_release_handle(session, block, &last_release);

            /* Return immediately if the sync failed, we're in trouble. */
            WT_RET(fsync_ret);

            block->sync_on_checkpoint = false;

            /* See if we should try to remove this handle. */
            if (last_release && __wt_block_eligible_for_sweep(bm, block))
                need_sweep = true;
        }
    } while (found);

    if (need_sweep)
        WT_TRET(__wt_bm_sweep_handles(session, bm));

    return (ret);
}

/*
 * __bm_checkpoint --
 *     Write a buffer into a block, creating a checkpoint.
 */
static int
__bm_checkpoint(WT_BM *bm, WT_SESSION_IMPL *session, WT_ITEM *buf, WT_PAGE_BLOCK_META *block_meta,
  WT_CKPT *ckptbase, bool data_checksum)
{
    WT_BLOCK *block;

    WT_UNUSED(block_meta);

    block = bm->block;

    WT_RET(__wt_block_checkpoint(session, block, buf, ckptbase, data_checksum));

    if (!bm->is_multi_handle)
        return (0);

    /*
     * For tiered tables, if we postponed switching to a new file, this is the right time to make
     * that happen since eviction is disabled at the moment and we are the exclusive writers.
     */
    if (bm->next_block != NULL) {
        WT_ASSERT(session, bm->prev_block == NULL);
        __wt_writelock(session, &bm->handle_array_lock);
        bm->prev_block = bm->block;
        bm->block = bm->next_block;
        bm->next_block = NULL;
        __wt_writeunlock(session, &bm->handle_array_lock);
        __wt_verbose(session, WT_VERB_TIERED, "block manager switched from %s to %s",
          bm->prev_block->name, bm->block->name);
    }

    /* Finally, sync any previous active files. */
    return (__bm_sync_tiered_handles(bm, session));
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
__bm_checkpoint_readonly(WT_BM *bm, WT_SESSION_IMPL *session, WT_ITEM *buf,
  WT_PAGE_BLOCK_META *block_meta, WT_CKPT *ckptbase, bool data_checksum)
{
    WT_UNUSED(buf);
    WT_UNUSED(block_meta);
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
         * buffers. This isn't supported for trees that use multiple files.
         */
        if (!bm->is_multi_handle)
            WT_RET(
              __wti_blkcache_map(session, bm->block, &bm->map, &bm->maplen, &bm->mapped_cookie));

        /*
         * If this handle is for a checkpoint, that is, read-only, there isn't a lot you can do with
         * it. Although the btree layer prevents attempts to write a checkpoint reference, paranoia
         * is healthy.
         */
        __wti_bm_method_set(bm, true);
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
    WT_DECL_RET;

    /* If we have made a switch from the older file, resolve the older one instead. */
    if (bm->prev_block != NULL) {
        if ((ret = __wt_block_checkpoint_resolve(session, bm->prev_block, failed)) == 0)
            bm->prev_block = NULL;
        return (ret);
    } else
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
        WT_TRET(__wti_blkcache_unmap(session, bm->block, bm->map, bm->maplen, &bm->mapped_cookie));

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
    u_int i;

    if (bm == NULL) /* Safety check */
        return (0);

    if (!bm->is_multi_handle)
        ret = __wti_bm_close_block(session, bm->block);
    else {
        /*
         * Higher-level code ensures that we can only have one call to close a block manager. So we
         * don't need to lock the block handle array here.
         *
         * We don't need to explicitly close the active handle; it is also in the handle array.
         */
        for (i = 0; i < bm->handle_array_next; ++i)
            WT_TRET(__wti_bm_close_block(session, bm->handle_array[i]));

        __wt_rwlock_destroy(session, &bm->handle_array_lock);
        __wt_free(session, bm->handle_array);
    }

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
__bm_compact_progress(WT_BM *bm, WT_SESSION_IMPL *session)
{
    __wt_block_compact_progress(session, bm->block);
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
 * __bm_encrypt_skip_size --
 *     Return the skip size for encryption
 */
static size_t
__bm_encrypt_skip_size(WT_BM *bm, WT_SESSION_IMPL *session)
{
    WT_UNUSED(bm);
    WT_UNUSED(session);

    return (WT_BLOCK_HEADER_BYTE_SIZE);
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
    if (blkcache->type != WT_BLKCACHE_UNCONFIGURED)
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
__bm_read(WT_BM *bm, WT_SESSION_IMPL *session, WT_ITEM *buf, WT_PAGE_BLOCK_META *block_meta,
  const uint8_t *addr, size_t addr_size)
{
    return (__wt_bm_read(bm, session, buf, block_meta, addr, addr_size));
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
 *     Switch the active handle to a different object.
 */
static int
__bm_switch_object(WT_BM *bm, WT_SESSION_IMPL *session, uint32_t objectid)
{
    WT_BLOCK *block, *current;
    size_t root_addr_size;

    /* The checkpoint lock protects against concurrent switches */
    WT_ASSERT_SPINLOCK_OWNED(session, &S2C(session)->checkpoint_lock)
    WT_ASSERT(session, bm->is_multi_handle);

    current = bm->block;

    /* We shouldn't ask to switch objects unless we actually need to switch objects */
    WT_ASSERT(session, current->objectid != objectid);

    WT_RET(__wt_blkcache_get_handle(session, bm, objectid, false, &block));

    __wt_verbose(session, WT_VERB_TIERED, "block manager scheduling a switch from %s to %s",
      current->name, block->name);

    /* This will be the new writable object. Load its checkpoint */
    WT_RET(__wt_block_checkpoint_load(session, block, NULL, 0, NULL, &root_addr_size, false));

    /*
     * The previous object must be synced to disk as part of the next checkpoint. Until that next
     * checkpoint completes, we may be writing into more than one block, and any sync at the block
     * manager level must take that until account.
     */
    current->sync_on_checkpoint = true;

    /*
     * We don't do the actual switch just yet. Eviction is active and might write to the file in
     * parallel. We postpone the switch to later when the block manager writes the checkpoint.
     */
    WT_ASSERT(session, bm->next_block == NULL && bm->prev_block == NULL);
    bm->next_block = block;

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
 * __bm_switch_object_end --
 *     Complete switching the active handle to a different object.
 */
static int
__bm_switch_object_end(WT_BM *bm, WT_SESSION_IMPL *session, uint32_t objectid)
{
    WT_ASSERT(session, bm->max_flushed_objectid == 0 || objectid == bm->max_flushed_objectid + 1);
    bm->max_flushed_objectid = objectid;

    return (__wt_bm_sweep_handles(session, bm));
}

/*
 * __bm_switch_object_end_readonly --
 *     Complete switching the tiered object; readonly version.
 */
static int
__bm_switch_object_end_readonly(WT_BM *bm, WT_SESSION_IMPL *session, uint32_t objectid)
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
    /*
     * If a tiered switch was scheduled, it should have happened by now. If we somehow miss it, we
     * will leave a dangling switch. Tiered server might incorrectly attempt to flush an active file
     * in such a case.
     */
    WT_ASSERT_ALWAYS(session, bm->next_block == NULL, "Missed switching the local file");

    if (bm->is_multi_handle)
        WT_RET(__bm_sync_tiered_handles(bm, session));

    /* If we have made a switch from the older file, sync the older one instead. */
    if (bm->prev_block != NULL)
        return (__wt_fsync(session, bm->prev_block->fh, block));
    else
        return (__wt_fsync(session, bm->block->fh, block));
}

/*
 * __wt_bm_sweep_handles --
 *     Free blocks from the manager's handle array if possible.
 */
int
__wt_bm_sweep_handles(WT_SESSION_IMPL *session, WT_BM *bm)
{
    WT_BLOCK *block;
    WT_DECL_RET;
    size_t nbytes;
    u_int i;

    WT_ASSERT(session, bm->is_multi_handle);

    /*
     * This function may be called when the reader count for a block has been observed at zero. Grab
     * the lock and check again to see if we can remove any block from our list. If the count for a
     * block is not zero, other readers have references at this time. The last of those readers will
     * have another chance to free it.
     */
    __wt_writelock(session, &bm->handle_array_lock);
    for (i = 0; i < bm->handle_array_next; ++i) {
        block = bm->handle_array[i];
        if (block->read_count == 0 && __wt_block_eligible_for_sweep(bm, block)) {
            /* We cannot close the active handle. */
            WT_ASSERT(session, block != bm->block);
            WT_TRET(__wti_bm_close_block(session, block));

            /*
             * To fill the hole just created, shift the rest of the array down. Adjust the loop
             * index so we'll continue just where we left off.
             *
             * FIXME-WT-12028: The set of active handles may be quite large, so the memmove may be
             * slow. We should consider hash tables to store the handles.
             */
            nbytes = (bm->handle_array_next - i - 1) * sizeof(bm->handle_array[0]);
            memmove(&bm->handle_array[i], &bm->handle_array[i + 1], nbytes);
            --bm->handle_array_next;
            --i;
        }
    }
    __wt_writeunlock(session, &bm->handle_array_lock);

    return (ret);
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
__bm_write(WT_BM *bm, WT_SESSION_IMPL *session, WT_ITEM *buf, WT_PAGE_BLOCK_META *block_meta,
  uint8_t *addr, size_t *addr_sizep, bool data_checksum, bool checkpoint_io)
{
    __wt_capacity_throttle(
      session, buf->size, checkpoint_io ? WT_THROTTLE_CKPT : WT_THROTTLE_EVICT);

    return (__wt_block_write(
      session, bm->block, buf, block_meta, addr, addr_sizep, data_checksum, checkpoint_io));
}

/*
 * __bm_write_readonly --
 *     Write a buffer into a block, returning the block's address cookie; readonly version.
 */
static int
__bm_write_readonly(WT_BM *bm, WT_SESSION_IMPL *session, WT_ITEM *buf,
  WT_PAGE_BLOCK_META *block_meta, uint8_t *addr, size_t *addr_sizep, bool data_checksum,
  bool checkpoint_io)
{
    WT_UNUSED(buf);
    WT_UNUSED(block_meta);
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
 * __bm_can_truncate --
 *     Check if a file has available space at the end of the file.
 */
static bool
__bm_can_truncate(WT_BM *bm, WT_SESSION_IMPL *session)
{
    return (__wt_block_extlist_can_truncate(session, bm->block, &bm->block->live.avail));
}

/*
 * __wti_bm_method_set --
 *     Set up the legal methods.
 */
void
__wti_bm_method_set(WT_BM *bm, bool readonly)
{
    bm->addr_invalid = __bm_addr_invalid;
    bm->addr_string = __bm_addr_string;
    bm->block_header = __bm_block_header;
    bm->can_truncate = __bm_can_truncate;
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
    bm->encrypt_skip = __bm_encrypt_skip_size;
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
    bm->switch_object_end = __bm_switch_object_end;
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
        bm->switch_object_end = __bm_switch_object_end_readonly;
        bm->sync = __bm_sync_readonly;
        bm->write = __bm_write_readonly;
        bm->write_size = __bm_write_size_readonly;
    }
}

/*
 * __wt_bm_set_readonly --
 *     Set the block API to read-only.
 */
void
__wt_bm_set_readonly(WT_SESSION_IMPL *session) WT_GCC_FUNC_ATTRIBUTE((cold))
{
    /* Switch the handle into read-only mode. */
    __wti_bm_method_set(S2BT(session)->bm, true);
}
