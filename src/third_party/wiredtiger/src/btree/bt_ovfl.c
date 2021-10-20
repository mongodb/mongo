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
     * Read the overflow item from the block manager, then reference the
     * start of the data and set the data's length.
     *
     * Overflow reads are synchronous. That may bite me at some point, but
     * WiredTiger supports large page sizes, overflow items should be rare.
     */
    WT_RET(__wt_bt_read(session, store, addr, addr_size));
    dsk = store->data;
    store->data = WT_PAGE_HEADER_BYTE(btree, dsk);
    store->size = dsk->u.datalen;

    WT_STAT_CONN_INCR(session, cache_read_overflow);
    WT_STAT_DATA_INCR(session, cache_read_overflow);

    return (0);
}

/*
 * __wt_ovfl_read --
 *     Bring an overflow item into memory.
 */
int
__wt_ovfl_read(
  WT_SESSION_IMPL *session, WT_PAGE *page, WT_CELL_UNPACK *unpack, WT_ITEM *store, bool *decoded)
{
    WT_DECL_RET;
    WT_OVFL_TRACK *track;
    size_t i;

    *decoded = false;

    /*
     * If no page specified, there's no need to lock and there's no cache to search, we don't care
     * about WT_CELL_VALUE_OVFL_RM cells.
     */
    if (page == NULL)
        return (__ovfl_read(session, unpack->data, unpack->size, store));

    /*
     * WT_CELL_VALUE_OVFL_RM cells: If reconciliation deleted an overflow
     * value, but there was still a reader in the system that might need it,
     * the on-page cell type will have been reset to WT_CELL_VALUE_OVFL_RM
     * and we will be passed a page so we can check the on-page cell.
     *
     * Acquire the overflow lock, and retest the on-page cell's value inside
     * the lock.
     */
    __wt_readlock(session, &S2BT(session)->ovfl_lock);
    if (__wt_cell_type_raw(unpack->cell) == WT_CELL_VALUE_OVFL_RM) {
        track = page->modify->ovfl_track;
        for (i = 0; i < track->remove_next; ++i)
            if (track->remove[i].cell == unpack->cell) {
                store->data = track->remove[i].data;
                store->size = track->remove[i].size;
                break;
            }
        WT_ASSERT(session, i < track->remove_next);
        *decoded = true;
    } else
        ret = __ovfl_read(session, unpack->data, unpack->size, store);
    __wt_readunlock(session, &S2BT(session)->ovfl_lock);

    return (ret);
}

/*
 * __wt_ovfl_discard_remove --
 *     Free the on-page overflow value cache.
 */
void
__wt_ovfl_discard_remove(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    WT_OVFL_TRACK *track;
    uint32_t i;

    if (page->modify != NULL && (track = page->modify->ovfl_track) != NULL) {
        for (i = 0; i < track->remove_next; ++i)
            __wt_free(session, track->remove[i].data);
        __wt_free(session, page->modify->ovfl_track->remove);
        track->remove_allocated = 0;
        track->remove_next = 0;
    }
}

/*
 * __ovfl_cache --
 *     Cache an overflow value.
 */
static int
__ovfl_cache(WT_SESSION_IMPL *session, WT_PAGE *page, WT_CELL_UNPACK *unpack)
{
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    WT_OVFL_TRACK *track;

    /* Read the overflow value. */
    WT_RET(__wt_scr_alloc(session, 1024, &tmp));
    WT_ERR(__wt_dsk_cell_data_ref(session, page->type, unpack, tmp));

    /* Allocating tracking structures as necessary. */
    if (page->modify->ovfl_track == NULL)
        WT_ERR(__wt_ovfl_track_init(session, page));
    track = page->modify->ovfl_track;

    /* Copy the overflow item into place. */
    WT_ERR(
      __wt_realloc_def(session, &track->remove_allocated, track->remove_next + 1, &track->remove));
    track->remove[track->remove_next].cell = unpack->cell;
    WT_ERR(__wt_memdup(session, tmp->data, tmp->size, &track->remove[track->remove_next].data));
    track->remove[track->remove_next].size = tmp->size;
    ++track->remove_next;

err:
    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * __wt_ovfl_remove --
 *     Remove an overflow value.
 */
int
__wt_ovfl_remove(WT_SESSION_IMPL *session, WT_PAGE *page, WT_CELL_UNPACK *unpack, bool evicting)
{
    /*
     * This function solves two problems in reconciliation.
     *
     * The first problem is snapshot readers needing on-page overflow values
     * that have been removed. The scenario is as follows:
     *
     *     - reconciling a leaf page that references an overflow item
     *     - the item is updated and the update committed
     *     - a checkpoint runs, freeing the backing overflow blocks
     *     - a snapshot transaction wants the original version of the item
     *
     * In summary, we may need the original version of an overflow item for
     * a snapshot transaction after the item was deleted from a page that's
     * subsequently been checkpointed, where the checkpoint must know about
     * the freed blocks.  We don't have any way to delay a free of the
     * underlying blocks until a particular set of transactions exit (and
     * this shouldn't be a common scenario), so cache the overflow value in
     * memory.
     *
     * This gets hard because the snapshot transaction reader might:
     *     - search the WT_UPDATE list and not find an useful entry
     *     - read the overflow value's address from the on-page cell
     *     - go to sleep
     *     - checkpoint runs, caches the overflow value, frees the blocks
     *     - another thread allocates and overwrites the blocks
     *     - the reader wakes up and reads the wrong value
     *
     * Use a read/write lock and the on-page cell to fix the problem: hold
     * a write lock when changing the cell type from WT_CELL_VALUE_OVFL to
     * WT_CELL_VALUE_OVFL_RM and hold a read lock when reading an overflow
     * item.
     *
     * The read/write lock is per btree, but it could be per page or even
     * per overflow item.  We don't do any of that because overflow values
     * are supposed to be rare and we shouldn't see contention for the lock.
     *
     * We only have to do this for checkpoints: in any eviction mode, there
     * can't be threads sitting in our update lists.
     */
    if (!evicting)
        WT_RET(__ovfl_cache(session, page, unpack));

    /*
     * The second problem is to only remove the underlying blocks once,
     * solved by the WT_CELL_VALUE_OVFL_RM flag.
     *
     * Queue the on-page cell to be set to WT_CELL_VALUE_OVFL_RM and the
     * underlying overflow value's blocks to be freed when reconciliation
     * completes.
     */
    return (__wt_ovfl_discard_add(session, page, unpack->cell));
}

/*
 * __wt_ovfl_discard --
 *     Discard an on-page overflow value, and reset the page's cell.
 */
int
__wt_ovfl_discard(WT_SESSION_IMPL *session, WT_CELL *cell)
{
    WT_BM *bm;
    WT_BTREE *btree;
    WT_CELL_UNPACK *unpack, _unpack;

    btree = S2BT(session);
    bm = btree->bm;
    unpack = &_unpack;

    __wt_cell_unpack(cell, unpack);

    /*
     * Finally remove overflow key/value objects, called when reconciliation
     * finishes after successfully writing a page.
     *
     * Keys must have already been instantiated and value objects must have
     * already been cached (if they might potentially still be read by any
     * running transaction).
     *
     * Acquire the overflow lock to avoid racing with a thread reading the
     * backing overflow blocks.
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
