/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wti_block_disagg_get_size --
 *     Return the total byte count.
 */
uint64_t
__wti_block_disagg_get_size(WT_BLOCK_DISAGG *block_disagg)
{
    return (__wt_atomic_load_uint64(&block_disagg->size));
}

/*
 * __wti_block_disagg_increase_size --
 *     Increase the total byte count.
 */
void
__wti_block_disagg_increase_size(WT_BLOCK_DISAGG *block_disagg, uint64_t size)
{
    (void)__wt_atomic_add_uint64(&block_disagg->size, size);
}

/*
 * __wti_block_disagg_decrease_size --
 *     Decrease the total byte count.
 */
void
__wti_block_disagg_decrease_size(
  WT_SESSION_IMPL *session, WT_BLOCK_DISAGG *block_disagg, uint64_t size)
{
    uint64_t orig;

    /*
     * Check for underflow and decrement as a single atomic step. The CAS guarantees we subtract
     * from the same value we validated, closing the race between the check and the subtraction. A
     * failed CAS means a concurrent update beat us to it; reload and retry.
     *
     * Clamping to zero hides a real accounting bug where we decrement more than was added. Remove
     * the clamp once that bug is fixed and add the assert back in.
     */
    do {
        orig = __wt_atomic_load_uint64(&block_disagg->size);
    } while (!__wt_atomic_cas_uint64(&block_disagg->size, orig, orig < size ? 0 : orig - size));

    if (orig < size)
        __wt_verbose_warning(session, WT_VERB_DISAGGREGATED_STORAGE,
          "disaggregated block size underflow: decrementing %" PRIu64 " from %" PRIu64
          ", clamped to 0",
          size, orig);
}

/*
 * __wt_block_disagg_get_size --
 *     Return the total byte count.
 */
uint64_t
__wt_block_disagg_get_size(WT_SESSION_IMPL *session)
{
    WT_ASSERT(session, F_ISSET(S2BT(session), WT_BTREE_DISAGGREGATED));
    return (__wti_block_disagg_get_size((WT_BLOCK_DISAGG *)S2BT(session)->bm->block));
}

/*
 * __wt_block_disagg_set_size --
 *     Set the total byte count.
 */
void
__wt_block_disagg_set_size(WT_SESSION_IMPL *session, uint64_t size)
{
    WT_ASSERT(session, F_ISSET(S2BT(session), WT_BTREE_DISAGGREGATED));
    (void)__wt_atomic_store_uint64(&((WT_BLOCK_DISAGG *)S2BT(session)->bm->block)->size, size);
}

/*
 * __wt_block_disagg_obsolete_delta_chain --
 *     Notify the block manager that a delta chain has been obsoleted by a full page image. The
 *     cumulative size of the old chain is no longer counted toward the total.
 */
void
__wt_block_disagg_obsolete_delta_chain(WT_SESSION_IMPL *session, uint64_t cumulative_size)
{
    WT_ASSERT(session, F_ISSET(S2BT(session), WT_BTREE_DISAGGREGATED));
    __wti_block_disagg_decrease_size(
      session, (WT_BLOCK_DISAGG *)S2BT(session)->bm->block, cumulative_size);
}

/*
 * __wti_block_disagg_apply_root_size --
 *     Account for the root page size transition during checkpoint. Subtract the previous root page
 *     size and add the new one, recording the generation so the change can be rolled back on
 *     failure.
 */
void
__wti_block_disagg_apply_root_size(
  WT_SESSION_IMPL *session, WT_BLOCK_DISAGG *block_disagg, uint32_t new_root_size)
{
    block_disagg->previous_root_size = block_disagg->current_root_size;
    block_disagg->current_root_size = new_root_size;

    block_disagg->root_size_gen = __wt_gen(session, WT_GEN_CHECKPOINT);

    __wti_block_disagg_decrease_size(session, block_disagg, block_disagg->previous_root_size);
    __wti_block_disagg_increase_size(block_disagg, block_disagg->current_root_size);
}

/*
 * __wt_block_disagg_checkpoint_rollback --
 *     Notify the block manager that a checkpoint has failed, allowing it to revert any in-progress
 *     size accounting. A no-op for non-disaggregated btrees.
 */
void
__wt_block_disagg_checkpoint_rollback(WT_SESSION_IMPL *session)
{
    WT_BLOCK_DISAGG *block_disagg;
    WT_BTREE *btree;

    btree = S2BT(session);
    if (!F_ISSET(btree, WT_BTREE_DISAGGREGATED))
        return;

    block_disagg = (WT_BLOCK_DISAGG *)btree->bm->block;
    if (block_disagg->root_size_gen != __wt_gen(session, WT_GEN_CHECKPOINT))
        return;

    __wti_block_disagg_decrease_size(session, block_disagg, block_disagg->current_root_size);
    __wti_block_disagg_increase_size(block_disagg, block_disagg->previous_root_size);

    block_disagg->current_root_size = block_disagg->previous_root_size;
}
