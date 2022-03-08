/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __ckpt_process(WT_SESSION_IMPL *, WT_BLOCK *, WT_CKPT *);
static int __ckpt_update(WT_SESSION_IMPL *, WT_BLOCK *, WT_CKPT *, WT_CKPT *, WT_BLOCK_CKPT *);

/*
 * __wt_block_ckpt_init --
 *     Initialize a checkpoint structure.
 */
int
__wt_block_ckpt_init(WT_SESSION_IMPL *session, WT_BLOCK_CKPT *ci, const char *name)
{
    WT_CLEAR(*ci);

    ci->version = WT_BM_CHECKPOINT_VERSION;
    ci->root_offset = WT_BLOCK_INVALID_OFFSET;

    WT_RET(__wt_block_extlist_init(session, &ci->alloc, name, "alloc", false));
    WT_RET(__wt_block_extlist_init(session, &ci->avail, name, "avail", true));
    WT_RET(__wt_block_extlist_init(session, &ci->discard, name, "discard", false));
    WT_RET(__wt_block_extlist_init(session, &ci->ckpt_avail, name, "ckpt_avail", true));

    return (0);
}

/*
 * __wt_block_checkpoint_load --
 *     Load a checkpoint.
 */
int
__wt_block_checkpoint_load(WT_SESSION_IMPL *session, WT_BLOCK *block, const uint8_t *addr,
  size_t addr_size, uint8_t *root_addr, size_t *root_addr_sizep, bool checkpoint)
{
    WT_BLOCK_CKPT *ci, _ci;
    WT_DECL_RET;
    uint8_t *endp;

    /*
     * Sometimes we don't find a root page (we weren't given a checkpoint, or the checkpoint was
     * empty). In that case we return an empty root address, set that up now.
     */
    *root_addr_sizep = 0;

    ci = NULL;

    if (WT_VERBOSE_ISSET(session, WT_VERB_CHECKPOINT))
        __wt_ckpt_verbose(session, block, "load", NULL, addr, addr_size);

    /*
     * There's a single checkpoint in the file that can be written, all of the others are read-only.
     * We use the same initialization calls for readonly checkpoints, but the information doesn't
     * persist.
     */
    if (checkpoint) {
        ci = &_ci;
        WT_ERR(__wt_block_ckpt_init(session, ci, "checkpoint"));
    } else {
#ifdef HAVE_DIAGNOSTIC
        /*
         * We depend on the btree level for locking: things will go bad fast if we open the live
         * system in two handles, or salvage, truncate or verify the live/running file.
         */
        __wt_spin_lock(session, &block->live_lock);
        WT_ASSERT(session, block->live_open == false);
        block->live_open = true;
        __wt_spin_unlock(session, &block->live_lock);
#endif
        ci = &block->live;
        WT_ERR(__wt_block_ckpt_init(session, ci, "live"));
    }

    /*
     * If the checkpoint has an on-disk root page, load it. Otherwise, size the file past the
     * description information.
     */
    if (addr == NULL || addr_size == 0)
        ci->file_size = block->allocsize;
    else {
        /* Crack the checkpoint cookie. */
        WT_ERR(__wt_block_ckpt_unpack(session, block, addr, addr_size, ci));

        /* Verify sets up next. */
        if (block->verify)
            WT_ERR(__wt_verify_ckpt_load(session, block, ci));

        /* Read any root page. */
        if (ci->root_offset != WT_BLOCK_INVALID_OFFSET) {
            endp = root_addr;
            WT_ERR(__wt_block_addr_pack(
              block, &endp, ci->root_objectid, ci->root_offset, ci->root_size, ci->root_checksum));
            *root_addr_sizep = WT_PTRDIFF(endp, root_addr);
        }

        /*
         * Rolling a checkpoint forward requires the avail list, the blocks from which we can
         * allocate.
         */
        if (!checkpoint)
            WT_ERR(__wt_block_extlist_read_avail(session, block, &ci->avail, ci->file_size));
    }

    /*
     * If the checkpoint can be written, that means anything written after the checkpoint is no
     * longer interesting, truncate the file. Don't bother checking the avail list for a block at
     * the end of the file, that was done when the checkpoint was first written (re-writing the
     * checkpoint might possibly make it relevant here, but it's unlikely enough I don't bother).
     */
    if (!checkpoint && WT_BLOCK_ISLOCAL(block))
        WT_ERR(__wt_block_truncate(session, block, ci->file_size));

    if (0) {
err:
        /*
         * Don't call checkpoint-unload: unload does real work including file truncation. If we fail
         * early enough that the checkpoint information isn't correct, bad things would happen. The
         * only allocated memory was in the service of verify, clean that up.
         */
        if (block->verify)
            WT_TRET(__wt_verify_ckpt_unload(session, block));
    }

    /* Checkpoints don't need the original information, discard it. */
    if (checkpoint)
        __wt_block_ckpt_destroy(session, ci);

    return (ret);
}

/*
 * __wt_block_checkpoint_unload --
 *     Unload a checkpoint.
 */
int
__wt_block_checkpoint_unload(WT_SESSION_IMPL *session, WT_BLOCK *block, bool checkpoint)
{
    WT_DECL_RET;

    /* Verify cleanup. */
    if (block->verify)
        WT_TRET(__wt_verify_ckpt_unload(session, block));

    /*
     * If it's the live system, truncate to discard any extended blocks and discard the active
     * extent lists. Hold the lock even though we're unloading the live checkpoint, there could be
     * readers active in other checkpoints.
     */
    if (!checkpoint) {
        WT_TRET(__wt_block_truncate(session, block, block->size));

        __wt_spin_lock(session, &block->live_lock);
        __wt_block_ckpt_destroy(session, &block->live);
#ifdef HAVE_DIAGNOSTIC
        block->live_open = false;
#endif
        __wt_spin_unlock(session, &block->live_lock);
    }

    return (ret);
}

/*
 * __wt_block_ckpt_destroy --
 *     Clear a checkpoint structure.
 */
void
__wt_block_ckpt_destroy(WT_SESSION_IMPL *session, WT_BLOCK_CKPT *ci)
{
    /* Discard the extent lists. */
    __wt_block_extlist_free(session, &ci->alloc);
    __wt_block_extlist_free(session, &ci->avail);
    __wt_block_extlist_free(session, &ci->discard);
    __wt_block_extlist_free(session, &ci->ckpt_alloc);
    __wt_block_extlist_free(session, &ci->ckpt_avail);
    __wt_block_extlist_free(session, &ci->ckpt_discard);
}

/*
 * __wt_block_checkpoint_start --
 *     Start a checkpoint.
 */
int
__wt_block_checkpoint_start(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
    WT_DECL_RET;

    __wt_spin_lock(session, &block->live_lock);
    switch (block->ckpt_state) {
    case WT_CKPT_INPROGRESS:
    case WT_CKPT_PANIC_ON_FAILURE:
    case WT_CKPT_SALVAGE:
        ret = __wt_panic(session, EINVAL,
          "%s: an unexpected checkpoint start: the checkpoint has already started or was "
          "configured for salvage",
          block->name);
        __wt_blkcache_set_readonly(session);
        break;
    case WT_CKPT_NONE:
        block->ckpt_state = WT_CKPT_INPROGRESS;
        break;
    }
    __wt_spin_unlock(session, &block->live_lock);
    return (ret);
}

/*
 * __wt_block_checkpoint --
 *     Create a new checkpoint.
 */
int
__wt_block_checkpoint(
  WT_SESSION_IMPL *session, WT_BLOCK *block, WT_ITEM *buf, WT_CKPT *ckptbase, bool data_checksum)
{
    WT_BLOCK_CKPT *ci;
    WT_DECL_RET;

    ci = &block->live;

    /* Switch to first-fit allocation. */
    __wt_block_configure_first_fit(block, true);

    /*
     * Write the root page: it's possible for there to be a checkpoint of
     * an empty tree, in which case, we store an illegal root offset.
     *
     * !!!
     * We happen to know that checkpoints are single-threaded above us in
     * the btree engine.  That's probably something we want to guarantee
     * for any WiredTiger block manager.
     */
    if (buf == NULL) {
        ci->root_offset = WT_BLOCK_INVALID_OFFSET;
        ci->root_objectid = ci->root_size = ci->root_checksum = 0;
    } else
        WT_ERR(__wt_block_write_off(session, block, buf, &ci->root_objectid, &ci->root_offset,
          &ci->root_size, &ci->root_checksum, data_checksum, true, false));

    /*
     * Checkpoints are potentially reading/writing/merging lots of blocks, pre-allocate structures
     * for this thread's use.
     */
    WT_ERR(__wt_block_ext_prealloc(session, 250));

    /* Process the checkpoint list, deleting and updating as required. */
    ret = __ckpt_process(session, block, ckptbase);

    /* Discard any excessive memory we've allocated. */
    WT_TRET(__wt_block_ext_discard(session, 250));

/* Restore the original allocation plan. */
err:
    __wt_block_configure_first_fit(block, false);

    return (ret);
}

/*
 * __ckpt_extlist_read --
 *     Read a checkpoints extent lists and copy
 */
static int
__ckpt_extlist_read(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_CKPT *ckpt)
{
    WT_BLOCK_CKPT *ci;

    /*
     * Allocate a checkpoint structure, crack the cookie and read the checkpoint's extent lists.
     *
     * Ignore the avail list: checkpoint avail lists are only useful if we are rolling forward from
     * the particular checkpoint and they represent our best understanding of what blocks can be
     * allocated. If we are not operating on the live checkpoint, subsequent checkpoints might have
     * allocated those blocks, and the avail list is useless. We don't discard it, because it is
     * useful as part of verification, but we don't re-write it either.
     */
    WT_RET(__wt_calloc(session, 1, sizeof(WT_BLOCK_CKPT), &ckpt->bpriv));

    ci = ckpt->bpriv;
    WT_RET(__wt_block_ckpt_init(session, ci, ckpt->name));
    WT_RET(__wt_block_ckpt_unpack(session, block, ckpt->raw.data, ckpt->raw.size, ci));
    WT_RET(__wt_block_extlist_read(session, block, &ci->alloc, ci->file_size));
    WT_RET(__wt_block_extlist_read(session, block, &ci->discard, ci->file_size));

    return (0);
}

/*
 * __ckpt_extlist_fblocks --
 *     If a checkpoint's extent list is going away, free its blocks.
 */
static int
__ckpt_extlist_fblocks(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el)
{
    if (el->offset == WT_BLOCK_INVALID_OFFSET)
        return (0);

    /*
     * Free blocks used to write checkpoint extents into the live system's checkpoint avail list
     * (they were never on any alloc list). Do not use the live system's avail list because that
     * list is used to decide if the file can be truncated, and we can't truncate any part of the
     * file that contains a previous checkpoint's extents.
     */
    return (__wt_block_insert_ext(session, block, &block->live.ckpt_avail, el->offset, el->size));
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __ckpt_verify --
 *     Diagnostic code, confirm we get what we expect in the checkpoint array.
 */
static int
__ckpt_verify(WT_SESSION_IMPL *session, WT_CKPT *ckptbase)
{
    WT_CKPT *ckpt;

    /*
     * Fast check that we're seeing what we expect to see: some number of checkpoints to add, delete
     * or ignore, terminated by a new checkpoint.
     */
    WT_CKPT_FOREACH (ckptbase, ckpt)
        switch (ckpt->flags) {
        case 0:
        case WT_CKPT_DELETE:
        case WT_CKPT_DELETE | WT_CKPT_FAKE:
        case WT_CKPT_FAKE:
            break;
        case WT_CKPT_ADD | WT_CKPT_BLOCK_MODS:
        case WT_CKPT_ADD:
            if (ckpt[1].name == NULL)
                break;
        /* FALLTHROUGH */
        default:
            return (__wt_illegal_value(session, ckpt->flags));
        }
    return (0);
}
#endif

/*
 * __ckpt_add_blkmod_entry --
 *     Add an offset/length entry to the bitstring based on granularity.
 */
static int
__ckpt_add_blkmod_entry(
  WT_SESSION_IMPL *session, WT_BLOCK_MODS *blk_mod, wt_off_t offset, wt_off_t len)
{
    uint64_t end_bit, start_bit;
    uint32_t end_buf_bytes, end_rdup_bits, end_rdup_bytes;

    WT_ASSERT(session, blk_mod->granularity != 0);
    /*
     * Figure out the starting and ending locations in the bitmap based on its granularity and our
     * offset and length. The bit locations are zero-based; be careful translating to sizes.
     */
    start_bit = (uint64_t)offset / blk_mod->granularity;
    end_bit = (uint64_t)(offset + len - 1) / blk_mod->granularity;
    WT_ASSERT(session, end_bit < UINT32_MAX);
    /* We want to grow the bitmap by 64 bits, or 8 bytes at a time. */
    end_rdup_bits = WT_MAX(__wt_rduppo2((uint32_t)end_bit + 1, 64), WT_BLOCK_MODS_LIST_MIN);
    end_rdup_bytes = __bitstr_size(end_rdup_bits);
    end_buf_bytes = __bitstr_size((uint32_t)blk_mod->nbits);
    /*
     * We are doing a lot of shifting. Make sure that the number of bytes we end up with is a
     * multiple of eight. We guarantee that in the rounding up call, but also make sure that the
     * constant stays a multiple of eight.
     */
    WT_ASSERT(session, end_rdup_bytes % 8 == 0);
    if (end_rdup_bytes > end_buf_bytes) {
        /* If we don't have enough, extend the buffer. */
        if (blk_mod->nbits == 0) {
            WT_RET(__wt_buf_initsize(session, &blk_mod->bitstring, end_rdup_bytes));
            memset(blk_mod->bitstring.mem, 0, end_rdup_bytes);
        } else {
            WT_RET(
              __wt_buf_set(session, &blk_mod->bitstring, blk_mod->bitstring.data, end_rdup_bytes));
            memset(
              (uint8_t *)blk_mod->bitstring.mem + end_buf_bytes, 0, end_rdup_bytes - end_buf_bytes);
        }
        blk_mod->nbits = end_rdup_bits;
    }
    /* Make sure we're not going to run past the end of the bitmap */
    WT_ASSERT(session, blk_mod->bitstring.size >= __bitstr_size((uint32_t)blk_mod->nbits));
    WT_ASSERT(session, end_bit < blk_mod->nbits);
    /* Set all the bits needed to record this offset/length pair. */
    __bit_nset(blk_mod->bitstring.mem, start_bit, end_bit);
    return (0);
}

/*
 * __ckpt_add_blk_mods_alloc --
 *     Add the checkpoint's allocated blocks to all valid incremental backup source identifiers.
 */
static int
__ckpt_add_blk_mods_alloc(
  WT_SESSION_IMPL *session, WT_CKPT *ckptbase, WT_BLOCK_CKPT *ci, WT_BLOCK *block)
{
    WT_BLOCK_MODS *blk_mod;
    WT_CKPT *ckpt;
    WT_EXT *ext;
    u_int i;

    WT_CKPT_FOREACH (ckptbase, ckpt) {
        if (F_ISSET(ckpt, WT_CKPT_ADD))
            break;
    }
    /* If this is not the live checkpoint or we don't care about incremental blocks, we're done. */
    if (ckpt == NULL || !F_ISSET(ckpt, WT_CKPT_BLOCK_MODS))
        return (0);
    for (i = 0; i < WT_BLKINCR_MAX; ++i) {
        blk_mod = &ckpt->backup_blocks[i];
        /* If there is no information at this entry, we're done. */
        if (!F_ISSET(blk_mod, WT_BLOCK_MODS_VALID))
            continue;

        if (block->created_during_backup)
            WT_RET(__ckpt_add_blkmod_entry(session, blk_mod, 0, block->allocsize));
        WT_EXT_FOREACH (ext, ci->alloc.off) {
            WT_RET(__ckpt_add_blkmod_entry(session, blk_mod, ext->off, ext->size));
        }
    }
    block->created_during_backup = false;
    return (0);
}

/*
 * __ckpt_add_blk_mods_ext --
 *     Add a set of extent blocks to all valid incremental backup source identifiers.
 */
static int
__ckpt_add_blk_mods_ext(WT_SESSION_IMPL *session, WT_CKPT *ckptbase, WT_BLOCK_CKPT *ci)
{
    WT_BLOCK_MODS *blk_mod;
    WT_CKPT *ckpt;
    u_int i;

    WT_CKPT_FOREACH (ckptbase, ckpt) {
        if (F_ISSET(ckpt, WT_CKPT_ADD))
            break;
    }
    /* If this is not the live checkpoint or we don't care about incremental blocks, we're done. */
    if (ckpt == NULL || !F_ISSET(ckpt, WT_CKPT_BLOCK_MODS))
        return (0);
    for (i = 0; i < WT_BLKINCR_MAX; ++i) {
        blk_mod = &ckpt->backup_blocks[i];
        /* If there is no information at this entry, we're done. */
        if (!F_ISSET(blk_mod, WT_BLOCK_MODS_VALID))
            continue;

        if (ci->alloc.offset != WT_BLOCK_INVALID_OFFSET)
            WT_RET(__ckpt_add_blkmod_entry(session, blk_mod, ci->alloc.offset, ci->alloc.size));
        if (ci->discard.offset != WT_BLOCK_INVALID_OFFSET)
            WT_RET(__ckpt_add_blkmod_entry(session, blk_mod, ci->discard.offset, ci->discard.size));
        if (ci->avail.offset != WT_BLOCK_INVALID_OFFSET)
            WT_RET(__ckpt_add_blkmod_entry(session, blk_mod, ci->avail.offset, ci->avail.size));
    }
    return (0);
}

/*
 * __ckpt_process --
 *     Process the list of checkpoints.
 */
static int
__ckpt_process(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_CKPT *ckptbase)
{
    WT_BLOCK_CKPT *a, *b, *ci;
    WT_CKPT *ckpt, *next_ckpt;
    WT_DECL_RET;
    uint64_t ckpt_size;
    bool deleting, fatal, locked;

    ci = &block->live;
    fatal = locked = false;

#ifdef HAVE_DIAGNOSTIC
    WT_RET(__ckpt_verify(session, ckptbase));
#endif

    /*
     * Checkpoints are a two-step process: first, write a new checkpoint to disk (including all the
     * new extent lists for modified checkpoints and the live system). As part of this, create a
     * list of file blocks newly available for reallocation, based on checkpoints being deleted. We
     * then return the locations of the new checkpoint information to our caller. Our caller has to
     * write that information into some kind of stable storage, and once that's done, we can
     * actually allocate from that list of newly available file blocks. (We can't allocate from that
     * list immediately because the allocation might happen before our caller saves the new
     * checkpoint information, and if we crashed before the new checkpoint location was saved, we'd
     * have overwritten blocks still referenced by checkpoints in the system.) In summary, there is
     * a second step: after our caller saves the checkpoint information, we are called to add the
     * newly available blocks into the live system's available list.
     *
     * This function is the first step, the second step is in the resolve function.
     *
     * If we're called to checkpoint the same file twice (without the second resolution step), or
     * re-entered for any reason, it's an error in our caller, and our choices are all bad: leak
     * blocks or potentially crash with our caller not yet having saved previous checkpoint
     * information to stable storage.
     */
    __wt_spin_lock(session, &block->live_lock);
    switch (block->ckpt_state) {
    case WT_CKPT_INPROGRESS:
        block->ckpt_state = WT_CKPT_PANIC_ON_FAILURE;
        break;
    case WT_CKPT_NONE:
    case WT_CKPT_PANIC_ON_FAILURE:
        ret = __wt_panic(session, EINVAL,
          "%s: an unexpected checkpoint attempt: the checkpoint was never started or has already "
          "completed",
          block->name);
        __wt_blkcache_set_readonly(session);
        break;
    case WT_CKPT_SALVAGE:
        /* Salvage doesn't use the standard checkpoint APIs. */
        break;
    }
    __wt_spin_unlock(session, &block->live_lock);
    WT_RET(ret);

    /*
     * Extents newly available as a result of deleting previous checkpoints are added to a list of
     * extents. The list should be empty, but as described above, there is no "free the checkpoint
     * information" call into the block manager; if there was an error in an upper level that
     * resulted in some previous checkpoint never being resolved, the list may not be empty. We
     * should have caught that with the "checkpoint in progress" test, but it doesn't cost us
     * anything to be cautious.
     *
     * We free the checkpoint's allocation and discard extent lists as part of the resolution step,
     * not because they're needed at that time, but because it's potentially a lot of work, and
     * waiting allows the btree layer to continue eviction sooner. As for the checkpoint-available
     * list, make sure they get cleaned out.
     */
    __wt_block_extlist_free(session, &ci->ckpt_avail);
    WT_RET(__wt_block_extlist_init(session, &ci->ckpt_avail, "live", "ckpt_avail", true));
    __wt_block_extlist_free(session, &ci->ckpt_alloc);
    __wt_block_extlist_free(session, &ci->ckpt_discard);

    /*
     * To delete a checkpoint, we'll need checkpoint information for it and the subsequent
     * checkpoint into which it gets rolled; read them from disk before we lock things down.
     */
    deleting = false;
    WT_CKPT_FOREACH (ckptbase, ckpt) {
        if (F_ISSET(ckpt, WT_CKPT_FAKE) || !F_ISSET(ckpt, WT_CKPT_DELETE))
            continue;
        deleting = true;

        /*
         * Read the checkpoint and next checkpoint extent lists if we haven't already read them (we
         * may have already read these extent blocks if there is more than one deleted checkpoint).
         */
        if (ckpt->bpriv == NULL)
            WT_ERR(__ckpt_extlist_read(session, block, ckpt));

        for (next_ckpt = ckpt + 1;; ++next_ckpt)
            if (!F_ISSET(next_ckpt, WT_CKPT_FAKE))
                break;

        /*
         * The "next" checkpoint may be the live tree which has no extent blocks to read.
         */
        if (next_ckpt->bpriv == NULL && !F_ISSET(next_ckpt, WT_CKPT_ADD))
            WT_ERR(__ckpt_extlist_read(session, block, next_ckpt));
    }

    /*
     * Failures are now fatal: we can't currently back out the merge of any deleted checkpoint
     * extent lists into the live system's extent lists, so continuing after error would leave the
     * live system's extent lists corrupted for any subsequent checkpoint (and potentially, should a
     * subsequent checkpoint succeed, for recovery).
     */
    fatal = true;

    /*
     * Hold a lock so the live extent lists and the file size can't change underneath us. I suspect
     * we'll tighten this if checkpoints take too much time away from real work: we read the
     * historic checkpoint information without a lock, but we could also merge and re-write the
     * deleted and merged checkpoint information without a lock, except for the final merge of
     * ranges into the live tree.
     */
    __wt_spin_lock(session, &block->live_lock);
    locked = true;

    /*
     * We've allocated our last page, update the checkpoint size. We need to calculate the live
     * system's checkpoint size before merging checkpoint allocation and discard information from
     * the checkpoints we're deleting, those operations change the underlying byte counts.
     */
    ckpt_size = ci->ckpt_size;
    ckpt_size += ci->alloc.bytes;
    ckpt_size -= ci->discard.bytes;

    /*
     * Record the checkpoint's allocated blocks. Do so before skipping any processing and before
     * possibly merging in blocks from any previous checkpoint.
     */
    WT_ERR(__ckpt_add_blk_mods_alloc(session, ckptbase, ci, block));

    /* Skip the additional processing if we aren't deleting checkpoints. */
    if (!deleting)
        goto live_update;

    /*
     * Delete any no-longer-needed checkpoints: we do this first as it frees blocks to the live
     * lists, and the freed blocks will then be included when writing the live extent lists.
     */
    WT_CKPT_FOREACH (ckptbase, ckpt) {
        if (F_ISSET(ckpt, WT_CKPT_FAKE) || !F_ISSET(ckpt, WT_CKPT_DELETE) ||
          !WT_BLOCK_ISLOCAL(block))
            continue;

        if (WT_VERBOSE_ISSET(session, WT_VERB_CHECKPOINT))
            __wt_ckpt_verbose(session, block, "delete", ckpt->name, ckpt->raw.data, ckpt->raw.size);

        /*
         * Find the checkpoint into which we'll roll this checkpoint's blocks: it's the next real
         * checkpoint in the list, and it better have been read in (if it's not the add slot).
         */
        for (next_ckpt = ckpt + 1;; ++next_ckpt)
            if (!F_ISSET(next_ckpt, WT_CKPT_FAKE))
                break;

        /*
         * Set the from/to checkpoint structures, where the "to" value may be the live tree.
         */
        a = ckpt->bpriv;
        if (F_ISSET(next_ckpt, WT_CKPT_ADD))
            b = &block->live;
        else
            b = next_ckpt->bpriv;

        /*
         * Free the root page: there's nothing special about this free, the root page is allocated
         * using normal rules, that is, it may have been taken from the avail list, and was entered
         * on the live system's alloc list at that time. We free it into the checkpoint's discard
         * list, however, not the live system's list because it appears on the checkpoint's alloc
         * list and so must be paired in the checkpoint.
         */
        if (a->root_offset != WT_BLOCK_INVALID_OFFSET)
            WT_ERR(
              __wt_block_insert_ext(session, block, &a->discard, a->root_offset, a->root_size));

        /*
         * Free the blocks used to hold the "from" checkpoint's extent lists, including the avail
         * list.
         */
        WT_ERR(__ckpt_extlist_fblocks(session, block, &a->alloc));
        WT_ERR(__ckpt_extlist_fblocks(session, block, &a->avail));
        WT_ERR(__ckpt_extlist_fblocks(session, block, &a->discard));

        /*
         * Roll the "from" alloc and discard extent lists into the "to" checkpoint's lists.
         */
        if (a->alloc.entries != 0)
            WT_ERR(__wt_block_extlist_merge(session, block, &a->alloc, &b->alloc));
        if (a->discard.entries != 0)
            WT_ERR(__wt_block_extlist_merge(session, block, &a->discard, &b->discard));

        /*
         * If the "to" checkpoint is also being deleted, we're done with it, it's merged into some
         * other checkpoint in the next loop. This means the extent lists may aggregate over a
         * number of checkpoints, but that's OK, they're disjoint sets of ranges.
         */
        if (F_ISSET(next_ckpt, WT_CKPT_DELETE))
            continue;

        /*
         * Find blocks for re-use: wherever the "to" checkpoint's allocate and discard lists
         * overlap, move the range to the live system's checkpoint available list.
         */
        WT_ERR(__wt_block_extlist_overlap(session, block, b));

        /*
         * If we're updating the live system's information, we're done.
         */
        if (F_ISSET(next_ckpt, WT_CKPT_ADD))
            continue;

        /*
         * We have to write the "to" checkpoint's extent lists out in new blocks, and update its
         * cookie.
         *
         * Free the blocks used to hold the "to" checkpoint's extent lists; don't include the avail
         * list, it's not changing.
         */
        WT_ERR(__ckpt_extlist_fblocks(session, block, &b->alloc));
        WT_ERR(__ckpt_extlist_fblocks(session, block, &b->discard));

        F_SET(next_ckpt, WT_CKPT_UPDATE);
    }

    /* Update checkpoints marked for update. */
    WT_CKPT_FOREACH (ckptbase, ckpt)
        if (F_ISSET(ckpt, WT_CKPT_UPDATE))
            WT_ERR(__ckpt_update(session, block, ckptbase, ckpt, ckpt->bpriv));

live_update:
    /* Truncate the file if that's possible. */
    WT_ERR(__wt_block_extlist_truncate(session, block, &ci->avail));

    /* Update the final, added checkpoint based on the live system. */
    WT_CKPT_FOREACH (ckptbase, ckpt)
        if (F_ISSET(ckpt, WT_CKPT_ADD)) {
            /*
             * !!!
             * Our caller wants the final checkpoint size. Setting the size here violates layering,
             * but the alternative is a call for the btree layer to crack the checkpoint cookie into
             * its components, and that's a fair amount of work.
             */
            ckpt->size = ckpt_size;

            /*
             * Set the rolling checkpoint size for the live system. The current size includes the
             * current checkpoint's root page size (root pages are on the checkpoint's block
             * allocation list as root pages are allocated with the usual block allocation
             * functions). That's correct, but we don't want to include it in the size for the next
             * checkpoint.
             */
            ckpt_size -= ci->root_size;

            /*
             * Additionally, we had a bug for awhile where the live checkpoint size grew without
             * bound. We can't sanity check the value, that would require walking the tree as part
             * of the checkpoint. Bound any bug at the size of the file. It isn't practical to
             * assert that the value is within bounds since databases created with older versions of
             * WiredTiger (2.8.0) would likely see an error.
             */
            ci->ckpt_size = WT_MIN(ckpt_size, (uint64_t)block->size);

            WT_ERR(__ckpt_update(session, block, ckptbase, ckpt, ci));
        }

    /*
     * Reset the live system's alloc and discard extent lists, leave the avail list alone. This
     * includes freeing a lot of extents, so do it outside of the system's lock by copying and
     * resetting the original, then doing the work later.
     */
    ci->ckpt_alloc = ci->alloc;
    WT_ERR(__wt_block_extlist_init(session, &ci->alloc, "live", "alloc", false));
    ci->ckpt_discard = ci->discard;
    WT_ERR(__wt_block_extlist_init(session, &ci->discard, "live", "discard", false));

#ifdef HAVE_DIAGNOSTIC
    /*
     * The first checkpoint in the system should always have an empty discard list. If we've read
     * that checkpoint and/or created it, check.
     */
    WT_CKPT_FOREACH (ckptbase, ckpt)
        if (!F_ISSET(ckpt, WT_CKPT_DELETE))
            break;
    if ((a = ckpt->bpriv) == NULL)
        a = &block->live;
    if (a->discard.entries != 0)
        WT_ERR_MSG(
          session, WT_ERROR, "first checkpoint incorrectly has blocks on the discard list");
#endif

err:
    if (ret != 0 && fatal) {
        ret = __wt_panic(session, ret, "%s: fatal checkpoint failure", block->name);
        __wt_blkcache_set_readonly(session);
    }

    if (locked)
        __wt_spin_unlock(session, &block->live_lock);

    /* Discard any checkpoint information we loaded. */
    WT_CKPT_FOREACH (ckptbase, ckpt)
        if ((ci = ckpt->bpriv) != NULL)
            __wt_block_ckpt_destroy(session, ci);

    return (ret);
}

/*
 * __ckpt_update --
 *     Update a checkpoint.
 */
static int
__ckpt_update(
  WT_SESSION_IMPL *session, WT_BLOCK *block, WT_CKPT *ckptbase, WT_CKPT *ckpt, WT_BLOCK_CKPT *ci)
{
    WT_DECL_ITEM(a);
    WT_DECL_RET;
    uint8_t *endp;
    bool is_live;

    is_live = F_ISSET(ckpt, WT_CKPT_ADD);

#ifdef HAVE_DIAGNOSTIC
    /* Check the extent list combinations for overlaps. */
    WT_RET(__wt_block_extlist_check(session, &ci->alloc, &ci->avail));
    WT_RET(__wt_block_extlist_check(session, &ci->discard, &ci->avail));
    WT_RET(__wt_block_extlist_check(session, &ci->alloc, &ci->discard));
#endif
    /*
     * Write the checkpoint's alloc and discard extent lists. Note these blocks never appear on the
     * system's allocation list, checkpoint extent blocks don't appear on any extent lists.
     */
    WT_RET(__wt_block_extlist_write(session, block, &ci->alloc, NULL));
    WT_RET(__wt_block_extlist_write(session, block, &ci->discard, NULL));

    /*
     * If this is the final block, we append an incomplete copy of the checkpoint information to the
     * avail list for standalone retrieval.
     */
    if (is_live) {
        /*
         * Copy the INCOMPLETE checkpoint information into the checkpoint.
         */
        WT_RET(__wt_buf_init(session, &ckpt->raw, WT_BLOCK_CHECKPOINT_BUFFER));
        endp = ckpt->raw.mem;
        WT_RET(__wt_block_ckpt_pack(session, block, &endp, ci, true));
        ckpt->raw.size = WT_PTRDIFF(endp, ckpt->raw.mem);

        /*
         * Convert the INCOMPLETE checkpoint array into its metadata representation. This must match
         * what is eventually written into the metadata file, in other words, everything must be
         * initialized before the block manager does the checkpoint.
         */
        WT_RET(__wt_scr_alloc(session, 8 * 1024, &a));
        ret = __wt_meta_ckptlist_to_meta(session, ckptbase, a);
        if (ret == 0)
            ret = __wt_strndup(session, a->data, a->size, &ckpt->block_checkpoint);
        __wt_scr_free(session, &a);
        WT_RET(ret);
    }

    /*
     * We only write an avail list for the live system, other checkpoint's avail lists are static
     * and never change.
     *
     * Write the avail list last so it reflects changes due to allocating blocks for the alloc and
     * discard lists. Second, when we write the live system's avail list, it's two lists: the
     * current avail list plus the list of blocks to be made available when the new checkpoint
     * completes. We can't merge that second list into the real list yet, it's not truly available
     * until the new checkpoint locations have been saved to the metadata.
     */
    if (is_live) {
        block->final_ckpt = ckpt;
        ret = __wt_block_extlist_write(session, block, &ci->avail, &ci->ckpt_avail);
        block->final_ckpt = NULL;
        WT_RET(ret);
    }

    /*
     * Record the blocks allocated to write the extent lists. We must record blocks in the live
     * system's extent lists, as those blocks are a necessary part of the checkpoint a hot backup
     * might recover. Update blocks in extent lists used to rewrite other checkpoints (for example,
     * an intermediate checkpoint rewritten because a checkpoint was rolled into it), even though
     * it's not necessary: those blocks aren't the last checkpoint in the file and so aren't
     * included in a recoverable checkpoint, they don't matter on a hot backup target until they're
     * allocated and used in the context of a live system. Regardless, they shouldn't materially
     * affect how much data we're writing, and it keeps things more consistent on the target to
     * update them. (Ignore the live system's ckpt_avail list here. The blocks on that list were
     * written into the final avail extent list which will be copied to the hot backup, and that's
     * all that matters.)
     */
    WT_RET(__ckpt_add_blk_mods_ext(session, ckptbase, ci));

    /*
     * Set the file size for the live system.
     *
     * !!!
     * We do NOT set the file size when re-writing checkpoints because we want to test the
     * checkpoint's blocks against a reasonable maximum file size during verification. This is bad:
     * imagine a checkpoint appearing early in the file, re-written, and then the checkpoint
     * requires blocks at the end of the file, blocks after the listed file size. If the application
     * opens that checkpoint for writing (discarding subsequent checkpoints), we would truncate the
     * file to the early chunk, discarding the re-written checkpoint information. The alternative,
     * updating the file size has its own problems, in that case we'd work correctly, but we'd lose
     * all of the blocks between the original checkpoint and the re-written checkpoint. Currently,
     * there's no API to roll-forward intermediate checkpoints, if there ever is, this will need to
     * be fixed.
     */
    if (is_live)
        ci->file_size = block->size;

    /* Copy the COMPLETE checkpoint information into the checkpoint. */
    WT_RET(__wt_buf_init(session, &ckpt->raw, WT_BLOCK_CHECKPOINT_BUFFER));
    endp = ckpt->raw.mem;
    WT_RET(__wt_block_ckpt_pack(session, block, &endp, ci, false));
    ckpt->raw.size = WT_PTRDIFF(endp, ckpt->raw.mem);

    if (WT_VERBOSE_ISSET(session, WT_VERB_CHECKPOINT))
        __wt_ckpt_verbose(session, block, "create", ckpt->name, ckpt->raw.data, ckpt->raw.size);

    return (0);
}

/*
 * __wt_block_checkpoint_resolve --
 *     Resolve a checkpoint.
 */
int
__wt_block_checkpoint_resolve(WT_SESSION_IMPL *session, WT_BLOCK *block, bool failed)
{
    WT_BLOCK_CKPT *ci;
    WT_DECL_RET;

    ci = &block->live;

    /*
     * Resolve the checkpoint after our caller has written the checkpoint information to stable
     * storage.
     */
    __wt_spin_lock(session, &block->live_lock);
    switch (block->ckpt_state) {
    case WT_CKPT_INPROGRESS:
        /* Something went wrong, but it's recoverable at our level. */
        goto done;
    case WT_CKPT_NONE:
    case WT_CKPT_SALVAGE:
        ret = __wt_panic(session, EINVAL,
          "%s: an unexpected checkpoint resolution: the checkpoint was never started or completed, "
          "or configured for salvage",
          block->name);
        __wt_blkcache_set_readonly(session);
        break;
    case WT_CKPT_PANIC_ON_FAILURE:
        if (!failed)
            break;
        ret = __wt_panic(
          session, EINVAL, "%s: the checkpoint failed, the system must restart", block->name);
        __wt_blkcache_set_readonly(session);
        break;
    }
    WT_ERR(ret);

    if ((ret = __wt_block_extlist_merge(session, block, &ci->ckpt_avail, &ci->avail)) != 0) {
        ret = __wt_panic(
          session, ret, "%s: fatal checkpoint failure during extent list merge", block->name);
        __wt_blkcache_set_readonly(session);
    }
    __wt_spin_unlock(session, &block->live_lock);

    /* Discard the lists remaining after the checkpoint call. */
    __wt_block_extlist_free(session, &ci->ckpt_avail);
    __wt_block_extlist_free(session, &ci->ckpt_alloc);
    __wt_block_extlist_free(session, &ci->ckpt_discard);

    __wt_spin_lock(session, &block->live_lock);
done:
    block->ckpt_state = WT_CKPT_NONE;
err:
    __wt_spin_unlock(session, &block->live_lock);

    return (ret);
}

#ifdef HAVE_UNITTEST
int
__ut_ckpt_add_blkmod_entry(
  WT_SESSION_IMPL *session, WT_BLOCK_MODS *blk_mod, wt_off_t offset, wt_off_t len)
{
    return (__ckpt_add_blkmod_entry(session, blk_mod, offset, len));
}
#endif
