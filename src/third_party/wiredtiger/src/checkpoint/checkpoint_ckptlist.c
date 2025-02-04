/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_ckptlist_free --
 *     Discard the checkpoint array.
 */
void
__wt_ckptlist_free(WT_SESSION_IMPL *session, WT_CKPT **ckptbasep)
{
    WT_CKPT *ckpt, *ckptbase;

    if ((ckptbase = *ckptbasep) == NULL)
        return;

    /*
     * Sometimes the checkpoint list has a checkpoint which has not been named yet, but carries an
     * order number.
     */
    WTI_CKPT_FOREACH_NAME_OR_ORDER (ckptbase, ckpt)
        __wt_checkpoint_free(session, ckpt);
    __wt_free(session, *ckptbasep);
}

/*
 * __wt_ckptlist_saved_free --
 *     Discard the saved checkpoint list.
 */
void
__wt_ckptlist_saved_free(WT_SESSION_IMPL *session)
{
    WT_BTREE *btree;

    btree = S2BT(session);

    __wt_ckptlist_free(session, &btree->ckpt);
    btree->ckpt_bytes_allocated = 0;
}

/*
 * __wt_checkpoint_free --
 *     Clean up a single checkpoint structure.
 */
void
__wt_checkpoint_free(WT_SESSION_IMPL *session, WT_CKPT *ckpt)
{
    WT_CKPT_BLOCK_MODS *blk_mod;
    uint64_t i;

    if (ckpt == NULL)
        return;

    __wt_free(session, ckpt->name);
    __wt_free(session, ckpt->block_metadata);
    __wt_free(session, ckpt->block_checkpoint);
    __wt_buf_free(session, &ckpt->addr);
    __wt_buf_free(session, &ckpt->raw);
    __wt_free(session, ckpt->bpriv);
    for (i = 0; i < WT_BLKINCR_MAX; ++i) {
        blk_mod = &ckpt->backup_blocks[i];
        __wt_buf_free(session, &blk_mod->bitstring);
        __wt_free(session, blk_mod->id_str);
        F_CLR(blk_mod, WT_CKPT_BLOCK_MODS_VALID);
    }

    WT_CLEAR(*ckpt); /* Clear to prepare for re-use. */
}
