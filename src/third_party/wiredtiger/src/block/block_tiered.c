/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __block_switch_writeable --
 *     Switch a new writeable object.
 */
static int
__block_switch_writeable(WT_SESSION_IMPL *session, WT_BLOCK *block, uint32_t object_id)
{
    WT_DECL_RET;
    WT_FH *new_fh, *old_fh;

    /*
     * FIXME-WT-7470: write lock while opening a new write handle.
     *
     * The block manager must always have valid file handle since other threads may have concurrent
     * requests in flight.
     */
    old_fh = block->fh;
    WT_ERR(block->opener->open(
      block->opener, session, object_id, WT_FS_OPEN_FILE_TYPE_DATA, block->file_flags, &new_fh));
    block->fh = new_fh;
    block->objectid = object_id;

    WT_ERR(__wt_close(session, &old_fh));

err:
    return (ret);
}

/*
 * __wt_block_tiered_fh --
 *     Open an object from the shared tier.
 */
int
__wt_block_tiered_fh(WT_SESSION_IMPL *session, WT_BLOCK *block, uint32_t object_id, WT_FH **fhp)
{
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;

    /*
     * FIXME-WT-7470: take a read lock to get a handle, and a write lock to open a handle or extend
     * the array.
     *
     * If the object id isn't larger than the array of file handles, see if it's already opened.
     */
    if (object_id * sizeof(WT_FILE_HANDLE *) < block->ofh_alloc &&
      (*fhp = block->ofh[object_id]) != NULL)
        return (0);

    /* Ensure the array is big enough. */
    WT_RET(__wt_realloc_def(session, &block->ofh_alloc, object_id + 1, &block->ofh));
    if (object_id >= block->max_objectid)
        block->max_objectid = object_id + 1;
    if ((*fhp = block->ofh[object_id]) != NULL)
        return (0);

    WT_RET(__wt_scr_alloc(session, 0, &tmp));
    WT_ERR(block->opener->open(block->opener, session, object_id, WT_FS_OPEN_FILE_TYPE_DATA,
      WT_FS_OPEN_READONLY | block->file_flags, &block->ofh[object_id]));
    *fhp = block->ofh[object_id];
    WT_ASSERT(session, *fhp != NULL);

err:
    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * __wt_block_switch_object --
 *     Modify an object.
 */
int
__wt_block_switch_object(
  WT_SESSION_IMPL *session, WT_BLOCK *block, uint32_t object_id, uint32_t flags)
{
    WT_UNUSED(flags);

    /*
     * FIXME-WT-7596 the flags argument will be used in the future to perform various tasks,
     * to efficiently mark objects in transition (that is during a switch):
     *  - mark this file as the writeable file (what currently happens)
     *  - disallow writes to this object (reads still allowed, we're about to switch)
     *  - close this object (about to move it, don't allow reopens yet)
     *  - allow opens on this object again
     */
    return (__block_switch_writeable(session, block, object_id));
}
