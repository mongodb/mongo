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
    WT_FH *new_fh, *old_fh;

    /*
     * FIXME-WT-7470: write lock while opening a new write handle.
     *
     * The block manager must always have valid file handle since other threads may have concurrent
     * requests in flight.
     */
    old_fh = block->fh;
    WT_RET(block->opener->open(
      block->opener, session, object_id, WT_FS_OPEN_FILE_TYPE_DATA, block->file_flags, &new_fh));
    block->fh = new_fh;
    block->objectid = object_id;

    return (__wt_close(session, &old_fh));
}

/*
 * __wt_block_fh --
 *     Get a block file handle.
 */
int
__wt_block_fh(WT_SESSION_IMPL *session, WT_BLOCK *block, uint32_t object_id, WT_FH **fhp)
{
    WT_DECL_RET;

    /* It's the local object if there's no object ID or the object ID matches our own. */
    if (object_id == 0 || object_id == block->objectid) {
        *fhp = block->fh;
        return (0);
    }

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

    /*
     * Fail gracefully if we don't have an opener, or if the opener fails: a release that can't read
     * tiered storage blocks might have been pointed at a file that it can read, but that references
     * files it doesn't know about, or there may have been some other mismatch. Regardless, we want
     * to log a specific error message, we're missing a file.
     */
    ret = block->opener->open == NULL ?
      WT_NOTFOUND :
      block->opener->open(block->opener, session, object_id, WT_FS_OPEN_FILE_TYPE_DATA,
        WT_FS_OPEN_READONLY | block->file_flags, &block->ofh[object_id]);
    if (ret == 0) {
        *fhp = block->ofh[object_id];
        return (0);
    }

    WT_RET_MSG(session, ret,
      "object %s with ID %" PRIu32 " referenced unknown object with ID %" PRIu32, block->name,
      block->objectid, object_id);
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
