/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __ovfl_read --
 *     Read an overflow item from the disk.
 */
static int
__ovfl_read(WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size, WT_ITEM *store)
{
    WT_BTREE *btree;
    const WT_PAGE_HEADER *dsk;

    btree = S2BT(session);

    /*
     * Read the overflow item from the block manager, then reference the start of the data and set
     * the data's length.
     *
     * Overflow reads are synchronous. That may bite me at some point, but WiredTiger supports large
     * page sizes, overflow items should be rare.
     */
    WT_RET(__wt_blkcache_read(session, store, addr, addr_size));
    dsk = store->data;
    store->data = WT_PAGE_HEADER_BYTE(btree, dsk);
    store->size = dsk->u.datalen;

    WT_STAT_CONN_DATA_INCR(session, cache_read_overflow);

    return (0);
}

/*
 * __wt_ovfl_read --
 *     Bring an overflow item into memory.
 */
int
__wt_ovfl_read(WT_SESSION_IMPL *session, WT_PAGE *page, WT_CELL_UNPACK_COMMON *unpack,
  WT_ITEM *store, bool *decoded)
{
    WT_DECL_RET;

    *decoded = false;

    /*
     * If no page specified, there's no need to lock and there's no cache to search, we don't care
     * about WT_CELL_VALUE_OVFL_RM cells.
     */
    if (page == NULL)
        return (__ovfl_read(session, unpack->data, unpack->size, store));

    /*
     * WT_CELL_VALUE_OVFL_RM cells: if reconciliation deletes an overflow value, the on-page cell
     * type is reset to WT_CELL_VALUE_OVFL_RM. Any values required by an existing reader will be
     * copied into the HS file, which means this value should never be read. It's possible to race
     * with checkpoints doing that work, lock before testing the removed flag.
     */
    __wt_readlock(session, &S2BT(session)->ovfl_lock);
    if (__wt_cell_type_raw(unpack->cell) == WT_CELL_VALUE_OVFL_RM) {
        ret = __wt_buf_setstr(session, store, "WT_CELL_VALUE_OVFL_RM");
        *decoded = true;
    } else
        ret = __ovfl_read(session, unpack->data, unpack->size, store);
    __wt_readunlock(session, &S2BT(session)->ovfl_lock);

    return (ret);
}

/*
 * __wt_ovfl_remove --
 *     Remove an overflow value.
 */
int
__wt_ovfl_remove(WT_SESSION_IMPL *session, WT_PAGE *page, WT_CELL_UNPACK_KV *unpack)
{
    /*
     * This function solves two problems in reconciliation.
     *
     * The first problem is snapshot readers needing on-page overflow values that have been removed.
     * If the overflow value is required by a reader, it will be copied into the HS file before the
     * backing blocks are removed. However, this gets hard because the snapshot transaction reader
     * might:
     *     - search the update list and not find a useful entry
     *     - read the overflow value's address from the on-page cell
     *     - go to sleep
     *     - checkpoint runs, frees the backing blocks
     *     - another thread allocates and overwrites the blocks
     *     - the reader wakes up and uses the on-page cell to read the blocks
     *
     * Use a read/write lock and the on-page cell to fix the problem: get a write lock when changing
     * the cell type from WT_CELL_VALUE_OVFL to WT_CELL_VALUE_OVFL_RM, get a read lock when reading
     * an overflow item.
     *
     * The read/write lock is per btree (but could be per page or even per overflow item). We don't
     * bother because overflow values are supposed to be rare and contention isn't expected.
     *
     * The second problem is to only remove the underlying blocks once, also solved by checking the
     * flag before doing any work.
     *
     * Queue the on-page cell to be set to WT_CELL_VALUE_OVFL_RM and the underlying overflow value's
     * blocks to be freed when reconciliation completes.
     */
    return (__wt_ovfl_discard_add(session, page, unpack->cell));
}

/*
 * __wt_ovfl_discard --
 *     Discard an on-page overflow value, and reset the page's cell.
 */
int
__wt_ovfl_discard(WT_SESSION_IMPL *session, WT_PAGE *page, WT_CELL *cell)
{
    WT_BM *bm;
    WT_BTREE *btree;
    WT_CELL_UNPACK_KV *unpack, _unpack;

    btree = S2BT(session);
    bm = btree->bm;
    unpack = &_unpack;

    __wt_cell_unpack_kv(session, page->dsk, cell, unpack);

    /*
     * Remove overflow key/value objects, called when reconciliation finishes after successfully
     * reconciling a page.
     *
     * Keys must have already been instantiated and value objects must have already been written to
     * the HS file (if they might potentially still be read by any running transaction).
     *
     * Acquire the overflow lock to avoid racing with a thread reading the backing overflow blocks.
     */
    __wt_writelock(session, &btree->ovfl_lock);

    switch (unpack->raw) {
    case WT_CELL_KEY_OVFL:
        __wt_cell_type_reset(session, unpack->cell, WT_CELL_KEY_OVFL, WT_CELL_KEY_OVFL_RM);
        break;
    case WT_CELL_VALUE_OVFL:
        __wt_cell_type_reset(session, unpack->cell, WT_CELL_VALUE_OVFL, WT_CELL_VALUE_OVFL_RM);
        break;
    default:
        return (__wt_illegal_value(session, unpack->raw));
    }

    __wt_writeunlock(session, &btree->ovfl_lock);

    /* Free the backing disk blocks. */
    return (bm->free(bm, session, unpack->data, unpack->size));
}
