/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#define WT_FORALL_CURSORS(clsm, c, i)     \
    for ((i) = (clsm)->nchunks; (i) > 0;) \
        if (((c) = (clsm)->chunks[--(i)]->cursor) != NULL)

#define WT_LSM_CURCMP(s, lsm_tree, c1, c2, cmp) \
    __wt_compare(s, (lsm_tree)->collator, &(c1)->key, &(c2)->key, &(cmp))

static int __clsm_lookup(WT_CURSOR_LSM *, WT_ITEM *);
static int __clsm_open_cursors(WT_CURSOR_LSM *, bool, u_int, uint32_t);
static int __clsm_reset_cursors(WT_CURSOR_LSM *, WT_CURSOR *);
static int __clsm_search_near(WT_CURSOR *cursor, int *exactp);

/*
 * __wt_clsm_request_switch --
 *     Request an LSM tree switch for a cursor operation.
 */
int
__wt_clsm_request_switch(WT_CURSOR_LSM *clsm)
{
    WT_DECL_RET;
    WT_LSM_TREE *lsm_tree;
    WT_SESSION_IMPL *session;

    lsm_tree = clsm->lsm_tree;
    session = CUR2S(clsm);

    if (!lsm_tree->need_switch) {
        /*
         * Check that we are up-to-date: don't set the switch if the tree has changed since we last
         * opened cursors: that can lead to switching multiple times when only one switch is
         * required, creating very small chunks.
         */
        __wt_lsm_tree_readlock(session, lsm_tree);
        if (lsm_tree->nchunks == 0 ||
          (clsm->dsk_gen == lsm_tree->dsk_gen && !lsm_tree->need_switch)) {
            lsm_tree->need_switch = true;
            ret = __wt_lsm_manager_push_entry(session, WT_LSM_WORK_SWITCH, 0, lsm_tree);
        }
        __wt_lsm_tree_readunlock(session, lsm_tree);
    }

    return (ret);
}

/*
 * __wt_clsm_await_switch --
 *     Wait for a switch to have completed in the LSM tree
 */
int
__wt_clsm_await_switch(WT_CURSOR_LSM *clsm)
{
    WT_LSM_TREE *lsm_tree;
    WT_SESSION_IMPL *session;
    int waited;

    lsm_tree = clsm->lsm_tree;
    session = CUR2S(clsm);

    /*
     * If there is no primary chunk, or a chunk has overflowed the hard limit, which either means a
     * worker thread has fallen behind or there has just been a user-level checkpoint, wait until
     * the tree changes.
     *
     * We used to switch chunks in the application thread here, but that is problematic because
     * there is a transaction in progress and it could roll back, leaving the metadata inconsistent.
     */
    for (waited = 0; lsm_tree->nchunks == 0 || clsm->dsk_gen == lsm_tree->dsk_gen; ++waited) {
        if (waited % WT_THOUSAND == 0)
            WT_RET(__wt_lsm_manager_push_entry(session, WT_LSM_WORK_SWITCH, 0, lsm_tree));
        __wt_sleep(0, 10);
    }
    return (0);
}

/*
 * __clsm_enter_update --
 *     Make sure an LSM cursor is ready to perform an update.
 */
static int
__clsm_enter_update(WT_CURSOR_LSM *clsm)
{
    WT_CURSOR *primary;
    WT_LSM_CHUNK *primary_chunk;
    WT_LSM_TREE *lsm_tree;
    WT_SESSION_IMPL *session;
    bool hard_limit, have_primary, ovfl;

    lsm_tree = clsm->lsm_tree;
    session = CUR2S(clsm);

    if (clsm->nchunks == 0) {
        primary = NULL;
        have_primary = false;
    } else {
        primary = clsm->chunks[clsm->nchunks - 1]->cursor;
        primary_chunk = clsm->primary_chunk;
        WT_ASSERT(session, F_ISSET(session->txn, WT_TXN_HAS_ID));
        have_primary = (primary != NULL && primary_chunk != NULL &&
          (primary_chunk->switch_txn == WT_TXN_NONE ||
            WT_TXNID_LT(session->txn->id, primary_chunk->switch_txn)));
    }

    /*
     * In LSM there are multiple btrees active at one time. The tree switch code needs to use btree
     * API methods, and it wants to operate on the btree for the primary chunk. Set that up now.
     *
     * If the primary chunk has grown too large, set a flag so the worker thread will switch when it
     * gets a chance to avoid introducing high latency into application threads. Don't do this
     * indefinitely: if a chunk grows twice as large as the configured size, block until it can be
     * switched.
     */
    hard_limit = lsm_tree->need_switch;

    if (have_primary) {
        WT_ENTER_PAGE_INDEX(session);
        WT_WITH_BTREE(session, CUR2BT(primary),
          ovfl = __wt_btree_lsm_over_size(
            session, hard_limit ? 2 * lsm_tree->chunk_size : lsm_tree->chunk_size));
        WT_LEAVE_PAGE_INDEX(session);

        /* If there was no overflow, we're done. */
        if (!ovfl)
            return (0);
    }

    /* Request a switch. */
    WT_RET(__wt_clsm_request_switch(clsm));

    /* If we only overflowed the soft limit, we're done. */
    if (have_primary && !hard_limit)
        return (0);

    WT_RET(__wt_clsm_await_switch(clsm));

    return (0);
}

/*
 * __clsm_enter --
 *     Start an operation on an LSM cursor, update if the tree has changed.
 */
static inline int
__clsm_enter(WT_CURSOR_LSM *clsm, bool reset, bool update)
{
    WT_DECL_RET;
    WT_LSM_TREE *lsm_tree;
    WT_SESSION_IMPL *session;
    WT_TXN *txn;
    uint64_t i, pinned_id, switch_txn;

    lsm_tree = clsm->lsm_tree;
    session = CUR2S(clsm);
    txn = session->txn;

    /* Merge cursors never update. */
    if (F_ISSET(clsm, WT_CLSM_MERGE))
        return (0);

    if (reset) {
        WT_ASSERT(session, !F_ISSET(&clsm->iface, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT));
        WT_RET(__clsm_reset_cursors(clsm, NULL));
    }

    for (;;) {
        /* Check if the cursor looks up-to-date. */
        if (clsm->dsk_gen != lsm_tree->dsk_gen && lsm_tree->nchunks != 0)
            goto open;

        /* Update the maximum transaction ID in the primary chunk. */
        if (update) {
            /*
             * Ensure that there is a transaction snapshot active.
             */
            WT_RET(__wt_txn_autocommit_check(session));
            WT_RET(__wt_txn_id_check(session));

            WT_RET(__clsm_enter_update(clsm));
            /*
             * Switching the tree will update the generation before updating the switch transaction.
             * We test the transaction in clsm_enter_update. Now test the disk generation to avoid
             * races.
             */
            if (clsm->dsk_gen != clsm->lsm_tree->dsk_gen)
                goto open;

            if (txn->isolation == WT_ISO_SNAPSHOT)
                __wt_txn_cursor_op(session);

            /*
             * Figure out how many updates are required for snapshot isolation.
             *
             * This is not a normal visibility check on the maximum transaction ID in each chunk:
             * any transaction ID that overlaps with our snapshot is a potential conflict.
             *
             * Note that the pinned ID is correct here: it tracks concurrent transactions excluding
             * special transactions such as checkpoint (which we can't conflict with because
             * checkpoint only writes the metadata, which is not an LSM tree).
             */
            clsm->nupdates = 1;
            if (txn->isolation == WT_ISO_SNAPSHOT && F_ISSET(clsm, WT_CLSM_OPEN_SNAPSHOT)) {
                WT_ASSERT(session, F_ISSET(txn, WT_TXN_HAS_SNAPSHOT));
                pinned_id = WT_SESSION_TXN_SHARED(session)->pinned_id;
                for (i = clsm->nchunks - 2; clsm->nupdates < clsm->nchunks; clsm->nupdates++, i--) {
                    switch_txn = clsm->chunks[i]->switch_txn;
                    if (WT_TXNID_LT(switch_txn, pinned_id))
                        break;
                    WT_ASSERT(session, !__wt_txn_visible_all(session, switch_txn, WT_TS_NONE));
                }
            }
        }

        /*
         * Stop when we are up-to-date, as long as this is:
         *   - a snapshot isolation update and the cursor is set up for
         *     that;
         *   - an update operation with a primary chunk, or
         *   - a read operation and the cursor is open for reading.
         */
        if ((!update || txn->isolation != WT_ISO_SNAPSHOT ||
              F_ISSET(clsm, WT_CLSM_OPEN_SNAPSHOT)) &&
          ((update && clsm->primary_chunk != NULL) ||
            (!update && F_ISSET(clsm, WT_CLSM_OPEN_READ))))
            break;

open:
        WT_WITH_SCHEMA_LOCK(session, ret = __clsm_open_cursors(clsm, update, 0, 0));
        WT_RET(ret);
    }

    if (!F_ISSET(clsm, WT_CLSM_ACTIVE)) {
        /*
         * Opening this LSM cursor has opened a number of btree cursors, ensure other code doesn't
         * think this is the first cursor in a session.
         */
        ++session->ncursors;
        WT_RET(__cursor_enter(session));
        F_SET(clsm, WT_CLSM_ACTIVE);
    }

    return (0);
}

/*
 * __clsm_leave --
 *     Finish an operation on an LSM cursor.
 */
static void
__clsm_leave(WT_CURSOR_LSM *clsm)
{
    WT_SESSION_IMPL *session;

    session = CUR2S(clsm);

    if (F_ISSET(clsm, WT_CLSM_ACTIVE)) {
        --session->ncursors;
        __cursor_leave(session);
        F_CLR(clsm, WT_CLSM_ACTIVE);
    }
}

/*
 * We need a tombstone to mark deleted records, and we use the special value below for that purpose.
 * We use two 0x14 (Device Control 4) bytes to minimize the likelihood of colliding with an
 * application-chosen encoding byte, if the application uses two leading DC4 byte for some reason,
 * we'll do a wasted data copy each time a new value is inserted into the object.
 */
static const WT_ITEM __tombstone = {"\x14\x14", 2, NULL, 0, 0};

/*
 * __clsm_deleted --
 *     Check whether the current value is a tombstone.
 */
static inline bool
__clsm_deleted(WT_CURSOR_LSM *clsm, const WT_ITEM *item)
{
    return (!F_ISSET(clsm, WT_CLSM_MINOR_MERGE) && item->size == __tombstone.size &&
      memcmp(item->data, __tombstone.data, __tombstone.size) == 0);
}

/*
 * __clsm_deleted_encode --
 *     Encode values that are in the encoded name space.
 */
static inline int
__clsm_deleted_encode(
  WT_SESSION_IMPL *session, const WT_ITEM *value, WT_ITEM *final_value, WT_ITEM **tmpp)
{
    WT_ITEM *tmp;

    /*
     * If value requires encoding, get a scratch buffer of the right size and create a copy of the
     * data with the first byte of the tombstone appended.
     */
    if (value->size >= __tombstone.size &&
      memcmp(value->data, __tombstone.data, __tombstone.size) == 0) {
        WT_RET(__wt_scr_alloc(session, value->size + 1, tmpp));
        tmp = *tmpp;

        memcpy(tmp->mem, value->data, value->size);
        memcpy((uint8_t *)tmp->mem + value->size, __tombstone.data, 1);
        final_value->data = tmp->mem;
        final_value->size = value->size + 1;
    } else {
        final_value->data = value->data;
        final_value->size = value->size;
    }

    return (0);
}

/*
 * __clsm_deleted_decode --
 *     Decode values that start with the tombstone.
 */
static inline void
__clsm_deleted_decode(WT_CURSOR_LSM *clsm, WT_ITEM *value)
{
    /*
     * Take care with this check: when an LSM cursor is used for a merge, and/or to create a Bloom
     * filter, it is valid to return the tombstone value.
     */
    if (!F_ISSET(clsm, WT_CLSM_MERGE) && value->size > __tombstone.size &&
      memcmp(value->data, __tombstone.data, __tombstone.size) == 0)
        --value->size;
}

/*
 * __clsm_close_cursors --
 *     Close any btree cursors that are not needed.
 */
static int
__clsm_close_cursors(WT_SESSION_IMPL *session, WT_CURSOR_LSM *clsm, u_int start, u_int end)
{
    WT_BLOOM *bloom;
    WT_CURSOR *c;
    u_int i;

    __wt_verbose(session, WT_VERB_LSM,
      "LSM closing cursor session(%p):clsm(%p), start: %u, end: %u", (void *)session, (void *)clsm,
      start, end);

    if (clsm->chunks == NULL || clsm->nchunks == 0)
        return (0);

    /*
     * Walk the cursors, closing any we don't need. Note that the exit condition here is special,
     * don't use WT_FORALL_CURSORS, and be careful with unsigned integer wrapping.
     */
    for (i = start; i < end; i++) {
        if ((c = (clsm)->chunks[i]->cursor) != NULL) {
            clsm->chunks[i]->cursor = NULL;
            WT_RET(c->close(c));
        }
        if ((bloom = clsm->chunks[i]->bloom) != NULL) {
            clsm->chunks[i]->bloom = NULL;
            WT_RET(__wt_bloom_close(bloom));
        }
    }

    return (0);
}

/*
 * __clsm_resize_chunks --
 *     Allocates an array of unit objects for each chunk.
 */
static int
__clsm_resize_chunks(WT_SESSION_IMPL *session, WT_CURSOR_LSM *clsm, u_int nchunks)
{
    WT_LSM_CURSOR_CHUNK *chunk;

    /* Don't allocate more iterators if we don't need them. */
    if (clsm->chunks_count >= nchunks)
        return (0);

    WT_RET(__wt_realloc_def(session, &clsm->chunks_alloc, nchunks, &clsm->chunks));
    for (; clsm->chunks_count < nchunks; clsm->chunks_count++) {
        WT_RET(__wt_calloc_one(session, &chunk));
        clsm->chunks[clsm->chunks_count] = chunk;
    }
    return (0);
}

/*
 * __clsm_free_chunks --
 *     Allocates an array of unit objects for each chunk.
 */
static void
__clsm_free_chunks(WT_SESSION_IMPL *session, WT_CURSOR_LSM *clsm)
{
    size_t i;

    for (i = 0; i < clsm->chunks_count; i++)
        __wt_free(session, clsm->chunks[i]);

    __wt_free(session, clsm->chunks);
}

/*
 * __clsm_open_cursors --
 *     Open cursors for the current set of files.
 */
static int
__clsm_open_cursors(WT_CURSOR_LSM *clsm, bool update, u_int start_chunk, uint32_t start_id)
{
    WT_BTREE *btree;
    WT_CURSOR *c, *cursor, *primary;
    WT_DECL_RET;
    WT_LSM_CHUNK *chunk;
    WT_LSM_TREE *lsm_tree;
    WT_SESSION_IMPL *session;
    WT_TXN *txn;
    uint64_t saved_gen;
    u_int close_range_end, close_range_start;
    u_int i, nchunks, ngood, nupdates;
    const char *checkpoint, *ckpt_cfg[3];
    bool locked;

    c = &clsm->iface;
    cursor = NULL;
    session = CUR2S(clsm);
    txn = session->txn;
    chunk = NULL;
    locked = false;
    lsm_tree = clsm->lsm_tree;

    /*
     * Ensure that any snapshot update has cursors on the right set of chunks to guarantee
     * visibility is correct.
     */
    if (update && txn->isolation == WT_ISO_SNAPSHOT)
        F_SET(clsm, WT_CLSM_OPEN_SNAPSHOT);

    /*
     * Query operations need a full set of cursors. Overwrite cursors do queries in service of
     * updates.
     */
    if (!update || !F_ISSET(c, WT_CURSTD_OVERWRITE))
        F_SET(clsm, WT_CLSM_OPEN_READ);

    if (lsm_tree->nchunks == 0)
        return (0);

    ckpt_cfg[0] = WT_CONFIG_BASE(session, WT_SESSION_open_cursor);
    ckpt_cfg[1] = "checkpoint=" WT_CHECKPOINT ",raw";
    ckpt_cfg[2] = NULL;

    /*
     * If the key is pointing to memory that is pinned by a chunk cursor, take a copy before closing
     * cursors.
     */
    if (F_ISSET(c, WT_CURSTD_KEY_INT))
        WT_ERR(__cursor_needkey(c));

    F_CLR(clsm, WT_CLSM_ITERATE_NEXT | WT_CLSM_ITERATE_PREV);

    __wt_lsm_tree_readlock(session, lsm_tree);
    locked = true;

/* Merge cursors have already figured out how many chunks they need. */
retry:
    if (F_ISSET(clsm, WT_CLSM_MERGE)) {
        nchunks = clsm->nchunks;
        ngood = 0;
        WT_ERR(__clsm_resize_chunks(session, clsm, nchunks));
        /*
         * We may have raced with another merge completing. Check that we're starting at the right
         * offset in the chunk array.
         */
        if (start_chunk >= lsm_tree->nchunks || lsm_tree->chunk[start_chunk]->id != start_id) {
            for (start_chunk = 0; start_chunk < lsm_tree->nchunks; start_chunk++) {
                chunk = lsm_tree->chunk[start_chunk];
                if (chunk->id == start_id)
                    break;
            }
            /* We have to find the start chunk: merge locked it. */
            WT_ASSERT(session, start_chunk < lsm_tree->nchunks);
        }
    } else {
        nchunks = lsm_tree->nchunks;
        WT_ERR(__clsm_resize_chunks(session, clsm, nchunks));

        /*
         * If we are only opening the cursor for updates, only open the primary chunk, plus any
         * other chunks that might be required to detect snapshot isolation conflicts.
         */
        if (F_ISSET(clsm, WT_CLSM_OPEN_READ))
            ngood = nupdates = 0;
        else if (F_ISSET(clsm, WT_CLSM_OPEN_SNAPSHOT)) {
            /*
             * Keep going until all updates in the next chunk are globally visible. Copy the maximum
             * transaction IDs into the cursor as we go.
             */
            for (ngood = nchunks - 1, nupdates = 1; ngood > 0; ngood--, nupdates++) {
                chunk = lsm_tree->chunk[ngood - 1];
                clsm->chunks[ngood - 1]->switch_txn = chunk->switch_txn;
                if (__wt_lsm_chunk_visible_all(session, chunk))
                    break;
            }
        } else {
            nupdates = 1;
            ngood = nchunks - 1;
        }

        /* Check how many cursors are already open. */
        for (; ngood < clsm->nchunks && ngood < nchunks; ngood++) {
            chunk = lsm_tree->chunk[ngood];
            cursor = clsm->chunks[ngood]->cursor;

            /* If the cursor isn't open yet, we're done. */
            if (cursor == NULL)
                break;

            /* Easy case: the URIs don't match. */
            if (strcmp(cursor->uri, chunk->uri) != 0)
                break;

            /*
             * Make sure the checkpoint config matches when not using a custom data source.
             */
            if (lsm_tree->custom_generation == 0 ||
              chunk->generation < lsm_tree->custom_generation) {
                checkpoint = ((WT_CURSOR_BTREE *)cursor)->dhandle->checkpoint;
                if (checkpoint == NULL && F_ISSET(chunk, WT_LSM_CHUNK_ONDISK) && !chunk->empty)
                    break;
            }

            /* Make sure the Bloom config matches. */
            if (clsm->chunks[ngood]->bloom == NULL && F_ISSET(chunk, WT_LSM_CHUNK_BLOOM))
                break;
        }

        /* Spurious generation bump? */
        if (ngood == clsm->nchunks && clsm->nchunks == nchunks) {
            clsm->dsk_gen = lsm_tree->dsk_gen;
            goto err;
        }

        /*
         * Close any cursors we no longer need.
         *
         * Drop the LSM tree lock while we do this: if the cache is full, we may block while closing
         * a cursor. Save the generation number and retry if it has changed under us.
         */
        if (clsm->chunks != NULL && ngood < clsm->nchunks) {
            close_range_start = ngood;
            close_range_end = clsm->nchunks;
        } else if (!F_ISSET(clsm, WT_CLSM_OPEN_READ) && nupdates > 0) {
            close_range_start = 0;
            close_range_end = WT_MIN(nchunks, clsm->nchunks);
            if (close_range_end > nupdates)
                close_range_end -= nupdates;
            else
                close_range_end = 0;
            WT_ASSERT(session, ngood >= close_range_end);
        } else {
            close_range_end = 0;
            close_range_start = 0;
        }
        if (close_range_end > close_range_start) {
            saved_gen = lsm_tree->dsk_gen;
            locked = false;
            __wt_lsm_tree_readunlock(session, lsm_tree);
            WT_ERR(__clsm_close_cursors(session, clsm, close_range_start, close_range_end));
            __wt_lsm_tree_readlock(session, lsm_tree);
            locked = true;
            if (lsm_tree->dsk_gen != saved_gen)
                goto retry;
        }

        /* Detach from our old primary. */
        clsm->primary_chunk = NULL;
        clsm->current = NULL;
    }

    WT_ASSERT(session, start_chunk + nchunks <= lsm_tree->nchunks);
    clsm->nchunks = nchunks;

    /* Open the cursors for chunks that have changed. */
    __wt_verbose(session, WT_VERB_LSM,
      "LSM opening cursor session(%p):clsm(%p)%s, chunks: %u, good: %u", (void *)session,
      (void *)clsm, update ? ", update" : "", nchunks, ngood);
    for (i = ngood; i != nchunks; i++) {
        chunk = lsm_tree->chunk[i + start_chunk];
        /* Copy the maximum transaction ID. */
        if (F_ISSET(clsm, WT_CLSM_OPEN_SNAPSHOT))
            clsm->chunks[i]->switch_txn = chunk->switch_txn;

        /*
         * Read from the checkpoint if the file has been written. Once all cursors switch, the
         * in-memory tree can be evicted.
         */
        WT_ASSERT(session, clsm->chunks[i]->cursor == NULL);
        ret = __wt_open_cursor(session, chunk->uri, c,
          (F_ISSET(chunk, WT_LSM_CHUNK_ONDISK) && !chunk->empty) ? ckpt_cfg : NULL,
          &clsm->chunks[i]->cursor);

        /*
         * XXX kludge: we may have an empty chunk where no checkpoint was written. If so, try to
         * open the ordinary handle on that chunk instead.
         */
        if (ret == WT_NOTFOUND && F_ISSET(chunk, WT_LSM_CHUNK_ONDISK)) {
            ret = __wt_open_cursor(session, chunk->uri, c, NULL, &clsm->chunks[i]->cursor);
            if (ret == 0)
                chunk->empty = 1;
        }
        WT_ERR(ret);

        /*
         * Setup all cursors other than the primary to only do conflict checks on insert operations.
         * This allows us to execute inserts on non-primary chunks as a way of checking for write
         * conflicts with concurrent updates.
         */
        if (i != nchunks - 1)
            clsm->chunks[i]->cursor->insert = __wt_curfile_insert_check;

        if (!F_ISSET(clsm, WT_CLSM_MERGE) && F_ISSET(chunk, WT_LSM_CHUNK_BLOOM))
            WT_ERR(__wt_bloom_open(session, chunk->bloom_uri, lsm_tree->bloom_bit_count,
              lsm_tree->bloom_hash_count, c, &clsm->chunks[i]->bloom));

        /* Child cursors always use overwrite and raw mode. */
        F_SET(clsm->chunks[i]->cursor, WT_CURSTD_OVERWRITE | WT_CURSTD_RAW);
    }

    /* Setup the count values for each chunk in the chunks */
    for (i = 0; i != clsm->nchunks; i++)
        clsm->chunks[i]->count = lsm_tree->chunk[i + start_chunk]->count;

    /* The last chunk is our new primary. */
    if (chunk != NULL && !F_ISSET(chunk, WT_LSM_CHUNK_ONDISK) && chunk->switch_txn == WT_TXN_NONE) {
        primary = clsm->chunks[clsm->nchunks - 1]->cursor;
        btree = CUR2BT(primary);

        /*
         * If the primary is not yet set as the primary, do that now. Note that eviction was
         * configured off when the underlying object was created, which is what we want, leave it
         * alone.
         *
         * We don't have to worry about races here: every thread that modifies the tree will have to
         * come through here, at worse we set the flag repeatedly. We don't use a WT_BTREE handle
         * flag, however, we could race doing the read-modify-write of the flags field.
         *
         * If something caused the chunk to be closed and reopened since it was created, we can no
         * longer use it as a primary chunk and we need to force a switch. We detect the tree was
         * created when it was opened by checking the "original" flag.
         */
        if (!btree->lsm_primary && btree->original)
            btree->lsm_primary = true;
        if (btree->lsm_primary)
            clsm->primary_chunk = chunk;
    }

    clsm->dsk_gen = lsm_tree->dsk_gen;

err:
#ifdef HAVE_DIAGNOSTIC
    /* Check that all cursors are open as expected. */
    if (ret == 0 && F_ISSET(clsm, WT_CLSM_OPEN_READ)) {
        for (i = 0; i != clsm->nchunks; i++) {
            cursor = clsm->chunks[i]->cursor;
            chunk = lsm_tree->chunk[i + start_chunk];

            /* Make sure the first cursor is open. */
            WT_ASSERT(session, cursor != NULL);

            /* Easy case: the URIs should match. */
            WT_ASSERT(session, strcmp(cursor->uri, chunk->uri) == 0);

            /*
             * Make sure the checkpoint config matches when not using a custom data source.
             */
            if (lsm_tree->custom_generation == 0 ||
              chunk->generation < lsm_tree->custom_generation) {
                checkpoint = ((WT_CURSOR_BTREE *)cursor)->dhandle->checkpoint;
                WT_ASSERT(session,
                  (F_ISSET(chunk, WT_LSM_CHUNK_ONDISK) && !chunk->empty) ? checkpoint != NULL :
                                                                           checkpoint == NULL);
            }

            /* Make sure the Bloom config matches. */
            WT_ASSERT(session,
              (F_ISSET(chunk, WT_LSM_CHUNK_BLOOM) && !F_ISSET(clsm, WT_CLSM_MERGE)) ?
                clsm->chunks[i]->bloom != NULL :
                clsm->chunks[i]->bloom == NULL);
        }
    }
#endif
    if (locked)
        __wt_lsm_tree_readunlock(session, lsm_tree);
    return (ret);
}

/*
 * __wt_clsm_init_merge --
 *     Initialize an LSM cursor for a merge.
 */
int
__wt_clsm_init_merge(WT_CURSOR *cursor, u_int start_chunk, uint32_t start_id, u_int nchunks)
{
    WT_CURSOR_LSM *clsm;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    clsm = (WT_CURSOR_LSM *)cursor;
    session = CUR2S(cursor);

    F_SET(clsm, WT_CLSM_MERGE);
    if (start_chunk != 0)
        F_SET(clsm, WT_CLSM_MINOR_MERGE);
    clsm->nchunks = nchunks;

    WT_WITH_SCHEMA_LOCK(session, ret = __clsm_open_cursors(clsm, false, start_chunk, start_id));
    return (ret);
}

/*
 * __clsm_get_current --
 *     Find the smallest / largest of the cursors and copy its key/value.
 */
static int
__clsm_get_current(WT_SESSION_IMPL *session, WT_CURSOR_LSM *clsm, bool smallest, bool *deletedp)
{
    WT_CURSOR *c, *current;
    u_int i;
    int cmp;
    bool multiple;

    current = NULL;
    multiple = false;

    WT_FORALL_CURSORS(clsm, c, i)
    {
        if (!F_ISSET(c, WT_CURSTD_KEY_INT))
            continue;
        if (current == NULL) {
            current = c;
            continue;
        }
        WT_RET(WT_LSM_CURCMP(session, clsm->lsm_tree, c, current, cmp));
        if (smallest ? cmp < 0 : cmp > 0) {
            current = c;
            multiple = false;
        } else if (cmp == 0)
            multiple = true;
    }

    c = &clsm->iface;
    if ((clsm->current = current) == NULL) {
        F_CLR(c, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
        return (WT_NOTFOUND);
    }

    if (multiple)
        F_SET(clsm, WT_CLSM_MULTIPLE);
    else
        F_CLR(clsm, WT_CLSM_MULTIPLE);

    WT_RET(current->get_key(current, &c->key));
    WT_RET(current->get_value(current, &c->value));

    F_CLR(c, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
    if ((*deletedp = __clsm_deleted(clsm, &c->value)) == false)
        F_SET(c, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);

    return (0);
}

/*
 * __clsm_compare --
 *     WT_CURSOR->compare implementation for the LSM cursor type.
 */
static int
__clsm_compare(WT_CURSOR *a, WT_CURSOR *b, int *cmpp)
{
    WT_CURSOR_LSM *alsm;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    /* There's no need to sync with the LSM tree, avoid WT_LSM_ENTER. */
    alsm = (WT_CURSOR_LSM *)a;
    CURSOR_API_CALL(a, session, compare, NULL);

    /*
     * Confirm both cursors refer to the same source and have keys, then compare the keys.
     */
    if (strcmp(a->uri, b->uri) != 0)
        WT_ERR_MSG(session, EINVAL, "comparison method cursors must reference the same object");

    WT_ERR(__cursor_needkey(a));
    WT_ERR(__cursor_needkey(b));

    WT_ERR(__wt_compare(session, alsm->lsm_tree->collator, &a->key, &b->key, cmpp));

err:
    API_END_RET(session, ret);
}

/*
 * __clsm_position_chunk --
 *     Position a chunk cursor.
 */
static int
__clsm_position_chunk(WT_CURSOR_LSM *clsm, WT_CURSOR *c, bool forward, int *cmpp)
{
    WT_CURSOR *cursor;
    WT_SESSION_IMPL *session;

    cursor = &clsm->iface;
    session = CUR2S(cursor);

    c->set_key(c, &cursor->key);
    WT_RET(c->search_near(c, cmpp));

    while (forward ? *cmpp < 0 : *cmpp > 0) {
        WT_RET(forward ? c->next(c) : c->prev(c));

        /*
         * With higher isolation levels, where we have stable reads, we're done: the cursor is now
         * positioned as expected.
         *
         * With read-uncommitted isolation, a new record could have appeared in between the search
         * and stepping forward / back. In that case, keep going until we see a key in the expected
         * range.
         */
        if (session->txn->isolation != WT_ISO_READ_UNCOMMITTED)
            return (0);

        WT_RET(WT_LSM_CURCMP(session, clsm->lsm_tree, c, cursor, *cmpp));
    }

    return (0);
}

/*
 * __clsm_next --
 *     WT_CURSOR->next method for the LSM cursor type.
 */
static int
__clsm_next(WT_CURSOR *cursor)
{
    WT_CURSOR *c;
    WT_CURSOR_LSM *clsm;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    u_int i;
    int cmp;
    bool deleted;

    clsm = (WT_CURSOR_LSM *)cursor;

    CURSOR_API_CALL(cursor, session, next, NULL);
    __cursor_novalue(cursor);
    WT_ERR(__clsm_enter(clsm, false, false));

    /* If we aren't positioned for a forward scan, get started. */
    if (clsm->current == NULL || !F_ISSET(clsm, WT_CLSM_ITERATE_NEXT)) {
        WT_FORALL_CURSORS(clsm, c, i)
        {
            if (!F_ISSET(cursor, WT_CURSTD_KEY_SET)) {
                WT_ERR(c->reset(c));
                ret = c->next(c);
            } else if (c != clsm->current &&
              (ret = __clsm_position_chunk(clsm, c, true, &cmp)) == 0 && cmp == 0 &&
              clsm->current == NULL)
                clsm->current = c;
            WT_ERR_NOTFOUND_OK(ret, false);
        }
        F_SET(clsm, WT_CLSM_ITERATE_NEXT | WT_CLSM_MULTIPLE);
        F_CLR(clsm, WT_CLSM_ITERATE_PREV);

        /* We just positioned *at* the key, now move. */
        if (clsm->current != NULL)
            goto retry;
    } else {
retry:
        /*
         * If there are multiple cursors on that key, move them forward.
         */
        if (F_ISSET(clsm, WT_CLSM_MULTIPLE)) {
            WT_FORALL_CURSORS(clsm, c, i)
            {
                if (!F_ISSET(c, WT_CURSTD_KEY_INT))
                    continue;
                if (c != clsm->current) {
                    WT_ERR(WT_LSM_CURCMP(session, clsm->lsm_tree, c, clsm->current, cmp));
                    if (cmp == 0)
                        WT_ERR_NOTFOUND_OK(c->next(c), false);
                }
            }
        }

        /* Move the smallest cursor forward. */
        c = clsm->current;
        WT_ERR_NOTFOUND_OK(c->next(c), false);
    }

    /* Find the cursor(s) with the smallest key. */
    if ((ret = __clsm_get_current(session, clsm, true, &deleted)) == 0 && deleted)
        goto retry;

err:
    __clsm_leave(clsm);
    if (ret == 0)
        __clsm_deleted_decode(clsm, &cursor->value);
    API_END_RET(session, ret);
}

/*
 * __clsm_random_chunk --
 *     Pick a chunk at random, weighted by the size of all chunks. Weighting proportional to
 *     documents avoids biasing towards small chunks. Then return the cursor on the chunk we have
 *     picked.
 */
static int
__clsm_random_chunk(WT_SESSION_IMPL *session, WT_CURSOR_LSM *clsm, WT_CURSOR **cursor)
{
    uint64_t checked_docs, i, rand_doc, total_docs;

    /*
     * If the tree is empty we cannot do a random lookup, so return a WT_NOTFOUND.
     */
    if (clsm->nchunks == 0)
        return (WT_NOTFOUND);
    for (total_docs = i = 0; i < clsm->nchunks; i++) {
        total_docs += clsm->chunks[i]->count;
    }
    if (total_docs == 0)
        return (WT_NOTFOUND);

    rand_doc = __wt_random(&session->rnd) % total_docs;

    for (checked_docs = i = 0; i < clsm->nchunks; i++) {
        checked_docs += clsm->chunks[i]->count;
        if (rand_doc <= checked_docs) {
            *cursor = clsm->chunks[i]->cursor;
            break;
        }
    }
    return (0);
}

/*
 * __clsm_next_random --
 *     WT_CURSOR->next method for the LSM cursor type when configured with next_random.
 */
static int
__clsm_next_random(WT_CURSOR *cursor)
{
    WT_CURSOR *c;
    WT_CURSOR_LSM *clsm;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    int exact;

    c = NULL;
    clsm = (WT_CURSOR_LSM *)cursor;

    CURSOR_API_CALL(cursor, session, next, NULL);
    __cursor_novalue(cursor);
    WT_ERR(__clsm_enter(clsm, false, false));

    for (;;) {
        WT_ERR(__clsm_random_chunk(session, clsm, &c));
        /*
         * This call to next_random on the chunk can potentially end in WT_NOTFOUND if the chunk we
         * picked is empty. We want to retry in that case.
         */
        WT_ERR_NOTFOUND_OK(__wt_curfile_next_random(c), true);
        if (ret == WT_NOTFOUND)
            continue;

        F_SET(cursor, WT_CURSTD_KEY_INT);
        WT_ERR(c->get_key(c, &cursor->key));
        /*
         * Search near the current key to resolve any tombstones and position to a valid document.
         * If we see a WT_NOTFOUND here that is valid, as the tree has no documents visible to us.
         */
        WT_ERR(__clsm_search_near(cursor, &exact));
        break;
    }

    /* We have found a valid doc. Set that we are now positioned */
    if (0) {
err:
        F_CLR(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);
    }
    __clsm_leave(clsm);
    API_END_RET(session, ret);
}

/*
 * __clsm_prev --
 *     WT_CURSOR->prev method for the LSM cursor type.
 */
static int
__clsm_prev(WT_CURSOR *cursor)
{
    WT_CURSOR *c;
    WT_CURSOR_LSM *clsm;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    u_int i;
    int cmp;
    bool deleted;

    clsm = (WT_CURSOR_LSM *)cursor;

    CURSOR_API_CALL(cursor, session, prev, NULL);
    __cursor_novalue(cursor);
    WT_ERR(__clsm_enter(clsm, false, false));

    /* If we aren't positioned for a reverse scan, get started. */
    if (clsm->current == NULL || !F_ISSET(clsm, WT_CLSM_ITERATE_PREV)) {
        WT_FORALL_CURSORS(clsm, c, i)
        {
            if (!F_ISSET(cursor, WT_CURSTD_KEY_SET)) {
                WT_ERR(c->reset(c));
                ret = c->prev(c);
            } else if (c != clsm->current &&
              (ret = __clsm_position_chunk(clsm, c, false, &cmp)) == 0 && cmp == 0 &&
              clsm->current == NULL)
                clsm->current = c;
            WT_ERR_NOTFOUND_OK(ret, false);
        }
        F_SET(clsm, WT_CLSM_ITERATE_PREV | WT_CLSM_MULTIPLE);
        F_CLR(clsm, WT_CLSM_ITERATE_NEXT);

        /* We just positioned *at* the key, now move. */
        if (clsm->current != NULL)
            goto retry;
    } else {
retry:
        /*
         * If there are multiple cursors on that key, move them backwards.
         */
        if (F_ISSET(clsm, WT_CLSM_MULTIPLE)) {
            WT_FORALL_CURSORS(clsm, c, i)
            {
                if (!F_ISSET(c, WT_CURSTD_KEY_INT))
                    continue;
                if (c != clsm->current) {
                    WT_ERR(WT_LSM_CURCMP(session, clsm->lsm_tree, c, clsm->current, cmp));
                    if (cmp == 0)
                        WT_ERR_NOTFOUND_OK(c->prev(c), false);
                }
            }
        }

        /* Move the largest cursor backwards. */
        c = clsm->current;
        WT_ERR_NOTFOUND_OK(c->prev(c), false);
    }

    /* Find the cursor(s) with the largest key. */
    if ((ret = __clsm_get_current(session, clsm, false, &deleted)) == 0 && deleted)
        goto retry;

err:
    __clsm_leave(clsm);
    if (ret == 0)
        __clsm_deleted_decode(clsm, &cursor->value);
    API_END_RET(session, ret);
}

/*
 * __clsm_reset_cursors --
 *     Reset any positioned chunk cursors. If the skip parameter is non-NULL, that cursor is about
 *     to be used, so there is no need to reset it.
 */
static int
__clsm_reset_cursors(WT_CURSOR_LSM *clsm, WT_CURSOR *skip)
{
    WT_CURSOR *c;
    WT_DECL_RET;
    u_int i;

    /* Fast path if the cursor is not positioned. */
    if ((clsm->current == NULL || clsm->current == skip) &&
      !F_ISSET(clsm, WT_CLSM_ITERATE_NEXT | WT_CLSM_ITERATE_PREV))
        return (0);

    WT_FORALL_CURSORS(clsm, c, i)
    {
        if (c == skip)
            continue;
        if (F_ISSET(c, WT_CURSTD_KEY_INT))
            WT_TRET(c->reset(c));
    }

    clsm->current = NULL;
    F_CLR(clsm, WT_CLSM_ITERATE_NEXT | WT_CLSM_ITERATE_PREV);

    return (ret);
}

/*
 * __clsm_reset --
 *     WT_CURSOR->reset method for the LSM cursor type.
 */
static int
__clsm_reset(WT_CURSOR *cursor)
{
    WT_CURSOR_LSM *clsm;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    /*
     * Don't use the normal __clsm_enter path: that is wasted work when all we want to do is give up
     * our position.
     */
    clsm = (WT_CURSOR_LSM *)cursor;
    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, reset, NULL);
    F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

    WT_TRET(__clsm_reset_cursors(clsm, NULL));

    /* In case we were left positioned, clear that. */
    __clsm_leave(clsm);

err:
    API_END_RET(session, ret);
}

/*
 * __clsm_lookup --
 *     Position an LSM cursor.
 */
static int
__clsm_lookup(WT_CURSOR_LSM *clsm, WT_ITEM *value)
{
    WT_BLOOM *bloom;
    WT_BLOOM_HASH bhash;
    WT_CURSOR *c, *cursor;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    u_int i;
    bool have_hash;

    c = NULL;
    cursor = &clsm->iface;
    have_hash = false;
    session = CUR2S(cursor);

    WT_FORALL_CURSORS(clsm, c, i)
    {
        /* If there is a Bloom filter, see if we can skip the read. */
        bloom = NULL;
        if ((bloom = clsm->chunks[i]->bloom) != NULL) {
            if (!have_hash) {
                __wt_bloom_hash(bloom, &cursor->key, &bhash);
                have_hash = true;
            }

            WT_ERR_NOTFOUND_OK(__wt_bloom_hash_get(bloom, &bhash), true);
            if (ret == WT_NOTFOUND) {
                WT_LSM_TREE_STAT_INCR(session, clsm->lsm_tree->bloom_miss);
                continue;
            }
            if (ret == 0)
                WT_LSM_TREE_STAT_INCR(session, clsm->lsm_tree->bloom_hit);
        }
        c->set_key(c, &cursor->key);
        if ((ret = c->search(c)) == 0) {
            WT_ERR(c->get_key(c, &cursor->key));
            WT_ERR(c->get_value(c, value));
            if (__clsm_deleted(clsm, value))
                ret = WT_NOTFOUND;
            goto done;
        }
        WT_ERR_NOTFOUND_OK(ret, false);
        F_CLR(c, WT_CURSTD_KEY_SET);
        /* Update stats: the active chunk can't have a bloom filter. */
        if (bloom != NULL)
            WT_LSM_TREE_STAT_INCR(session, clsm->lsm_tree->bloom_false_positive);
        else if (clsm->primary_chunk == NULL || i != clsm->nchunks)
            WT_LSM_TREE_STAT_INCR(session, clsm->lsm_tree->lsm_lookup_no_bloom);
    }
    WT_ERR(WT_NOTFOUND);

done:
err:
    if (ret == 0) {
        F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
        F_SET(cursor, WT_CURSTD_KEY_INT);
        clsm->current = c;
        if (value == &cursor->value)
            F_SET(cursor, WT_CURSTD_VALUE_INT);
    } else if (c != NULL)
        WT_TRET(c->reset(c));

    return (ret);
}

/*
 * __clsm_search --
 *     WT_CURSOR->search method for the LSM cursor type.
 */
static int
__clsm_search(WT_CURSOR *cursor)
{
    WT_CURSOR_LSM *clsm;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    clsm = (WT_CURSOR_LSM *)cursor;

    CURSOR_API_CALL(cursor, session, search, NULL);
    WT_ERR(__cursor_needkey(cursor));
    __cursor_novalue(cursor);
    WT_ERR(__clsm_enter(clsm, true, false));
    F_CLR(clsm, WT_CLSM_ITERATE_NEXT | WT_CLSM_ITERATE_PREV);

    ret = __clsm_lookup(clsm, &cursor->value);

err:
    __clsm_leave(clsm);
    if (ret == 0)
        __clsm_deleted_decode(clsm, &cursor->value);
    API_END_RET(session, ret);
}

/*
 * __clsm_search_near --
 *     WT_CURSOR->search_near method for the LSM cursor type.
 */
static int
__clsm_search_near(WT_CURSOR *cursor, int *exactp)
{
    WT_CURSOR *c, *closest;
    WT_CURSOR_LSM *clsm;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    u_int i;
    int cmp, exact;
    bool deleted;

    closest = NULL;
    clsm = (WT_CURSOR_LSM *)cursor;
    exact = 0;

    CURSOR_API_CALL(cursor, session, search_near, NULL);
    WT_ERR(__cursor_needkey(cursor));
    __cursor_novalue(cursor);
    WT_ERR(__clsm_enter(clsm, true, false));
    F_CLR(clsm, WT_CLSM_ITERATE_NEXT | WT_CLSM_ITERATE_PREV);

    /*
     * search_near is somewhat fiddly: we can't just use a nearby key from the in-memory chunk
     * because there could be a closer key on disk.
     *
     * As we search down the chunks, we stop as soon as we find an exact match. Otherwise, we
     * maintain the smallest cursor larger than the search key and the largest cursor smaller than
     * the search key. At the end, we prefer the larger cursor, but if no record is larger, position
     * on the last record in the tree.
     */
    WT_FORALL_CURSORS(clsm, c, i)
    {
        c->set_key(c, &cursor->key);
        if ((ret = c->search_near(c, &cmp)) == WT_NOTFOUND) {
            ret = 0;
            continue;
        }
        if (ret != 0)
            goto err;

        /* Do we have an exact match? */
        if (cmp == 0) {
            closest = c;
            exact = 1;
            break;
        }

        /*
         * Prefer larger cursors. There are two reasons: (1) we expect prefix searches to be a
         * common case (as in our own indices); and (2) we need a way to unambiguously know we have
         * the "closest" result.
         */
        if (cmp < 0) {
            if ((ret = c->next(c)) == WT_NOTFOUND) {
                ret = 0;
                continue;
            }
            if (ret != 0)
                goto err;
        }

        /*
         * We are trying to find the smallest cursor greater than the search key.
         */
        if (closest == NULL)
            closest = c;
        else {
            WT_ERR(WT_LSM_CURCMP(session, clsm->lsm_tree, c, closest, cmp));
            if (cmp < 0)
                closest = c;
        }
    }

    /*
     * At this point, we either have an exact match, or closest is the smallest cursor larger than
     * the search key, or it is NULL if the search key is larger than any record in the tree.
     */
    cmp = exact ? 0 : 1;

    /*
     * If we land on a deleted item, try going forwards or backwards to find one that isn't deleted.
     * If the whole tree is empty, we'll end up with WT_NOTFOUND, as expected.
     */
    if (closest == NULL)
        deleted = true;
    else {
        WT_ERR(closest->get_key(closest, &cursor->key));
        WT_ERR(closest->get_value(closest, &cursor->value));
        clsm->current = closest;
        closest = NULL;
        deleted = __clsm_deleted(clsm, &cursor->value);
        if (!deleted)
            __clsm_deleted_decode(clsm, &cursor->value);
        else {
            /*
             * We have a key pointing at memory that is pinned by the current chunk cursor. In the
             * unlikely event that we have to reopen cursors to move to the next record, make sure
             * the cursor flags are set so a copy is made before the current chunk cursor releases
             * its position.
             */
            F_CLR(cursor, WT_CURSTD_KEY_SET);
            F_SET(cursor, WT_CURSTD_KEY_INT);
            /*
             * We call __clsm_next here as we want to advance forward. If we are a random LSM cursor
             * calling next on the cursor will not advance as we intend.
             */
            if ((ret = __clsm_next(cursor)) == 0) {
                cmp = 1;
                deleted = false;
            }
        }
        WT_ERR_NOTFOUND_OK(ret, false);
    }
    if (deleted) {
        clsm->current = NULL;
        /*
         * We call prev directly here as cursor->prev may be "invalid" if this is a random cursor.
         */
        WT_ERR(__clsm_prev(cursor));
        cmp = -1;
    }
    *exactp = cmp;

err:
    __clsm_leave(clsm);
    if (closest != NULL)
        WT_TRET(closest->reset(closest));

    F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
    if (ret == 0) {
        F_SET(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);
    } else
        clsm->current = NULL;

    API_END_RET(session, ret);
}

/*
 * __clsm_put --
 *     Put an entry into the in-memory tree, trigger a file switch if necessary.
 */
static inline int
__clsm_put(WT_SESSION_IMPL *session, WT_CURSOR_LSM *clsm, const WT_ITEM *key, const WT_ITEM *value,
  bool position, bool reserve)
{
    WT_CURSOR *c, *primary;
    WT_LSM_TREE *lsm_tree;
    u_int i, slot;
    int (*func)(WT_CURSOR *);

    lsm_tree = clsm->lsm_tree;

    WT_ASSERT(session,
      F_ISSET(session->txn, WT_TXN_HAS_ID) && clsm->primary_chunk != NULL &&
        (clsm->primary_chunk->switch_txn == WT_TXN_NONE ||
          WT_TXNID_LE(session->txn->id, clsm->primary_chunk->switch_txn)));

    /*
     * Clear the existing cursor position. Don't clear the primary cursor: we're about to use it
     * anyway.
     */
    primary = clsm->chunks[clsm->nchunks - 1]->cursor;
    WT_RET(__clsm_reset_cursors(clsm, primary));

    /* If necessary, set the position for future scans. */
    if (position)
        clsm->current = primary;

    for (i = 0, slot = clsm->nchunks - 1; i < clsm->nupdates; i++, slot--) {
        /* Check if we need to keep updating old chunks. */
        if (i > 0 && __wt_txn_visible(session, clsm->chunks[slot]->switch_txn, WT_TS_NONE)) {
            clsm->nupdates = i;
            break;
        }

        c = clsm->chunks[slot]->cursor;
        c->set_key(c, key);
        func = c->insert;
        if (i == 0 && position)
            func = reserve ? c->reserve : c->update;
        if (func != c->reserve)
            c->set_value(c, value);
        WT_RET(func(c));
    }

    /*
     * Update the record count. It is in a shared structure, but it's only approximate, so don't
     * worry about protecting access.
     *
     * Throttle if necessary. Every 100 update operations on each cursor, check if throttling is
     * required. Don't rely only on the shared counter because it can race, and because for some
     * workloads, there may not be enough records per chunk to get effective throttling.
     */
    if ((++clsm->primary_chunk->count % 100 == 0 || ++clsm->update_count >= 100) &&
      lsm_tree->merge_throttle + lsm_tree->ckpt_throttle > 0) {
        clsm->update_count = 0;
        WT_LSM_TREE_STAT_INCRV(session, lsm_tree->lsm_checkpoint_throttle, lsm_tree->ckpt_throttle);
        WT_STAT_CONN_INCRV(session, lsm_checkpoint_throttle, lsm_tree->ckpt_throttle);
        WT_LSM_TREE_STAT_INCRV(session, lsm_tree->lsm_merge_throttle, lsm_tree->merge_throttle);
        WT_STAT_CONN_INCRV(session, lsm_merge_throttle, lsm_tree->merge_throttle);
        __wt_sleep(0, lsm_tree->ckpt_throttle + lsm_tree->merge_throttle);
    }

    return (0);
}

/*
 * __clsm_insert --
 *     WT_CURSOR->insert method for the LSM cursor type.
 */
static int
__clsm_insert(WT_CURSOR *cursor)
{
    WT_CURSOR_LSM *clsm;
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    WT_ITEM value;
    WT_SESSION_IMPL *session;

    clsm = (WT_CURSOR_LSM *)cursor;

    CURSOR_UPDATE_API_CALL(cursor, session, insert);
    WT_ERR(__cursor_needkey(cursor));
    WT_ERR(__cursor_needvalue(cursor));
    WT_ERR(__clsm_enter(clsm, false, true));

    /*
     * It isn't necessary to copy the key out after the lookup in this case because any non-failed
     * lookup results in an error, and a failed lookup leaves the original key intact.
     */
    if (!F_ISSET(cursor, WT_CURSTD_OVERWRITE) &&
      (ret = __clsm_lookup(clsm, &value)) != WT_NOTFOUND) {
        if (ret == 0)
            ret = WT_DUPLICATE_KEY;
        goto err;
    }

    WT_ERR(__clsm_deleted_encode(session, &cursor->value, &value, &buf));
    WT_ERR(__clsm_put(session, clsm, &cursor->key, &value, false, false));

    /*
     * WT_CURSOR.insert doesn't leave the cursor positioned, and the application may want to free
     * the memory used to configure the insert; don't read that memory again (matching the
     * underlying file object cursor insert semantics).
     */
    F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

err:
    __wt_scr_free(session, &buf);
    __clsm_leave(clsm);
    CURSOR_UPDATE_API_END(session, ret);
    return (ret);
}

/*
 * __clsm_update --
 *     WT_CURSOR->update method for the LSM cursor type.
 */
static int
__clsm_update(WT_CURSOR *cursor)
{
    WT_CURSOR_LSM *clsm;
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    WT_ITEM value;
    WT_SESSION_IMPL *session;

    clsm = (WT_CURSOR_LSM *)cursor;

    CURSOR_UPDATE_API_CALL(cursor, session, update);
    WT_ERR(__cursor_needkey(cursor));
    WT_ERR(__cursor_needvalue(cursor));
    WT_ERR(__clsm_enter(clsm, false, true));

    if (!F_ISSET(cursor, WT_CURSTD_OVERWRITE)) {
        WT_ERR(__clsm_lookup(clsm, &value));
        /*
         * Copy the key out, since the insert resets non-primary chunk cursors which our lookup may
         * have landed on.
         */
        WT_ERR(__cursor_needkey(cursor));
    }
    WT_ERR(__clsm_deleted_encode(session, &cursor->value, &value, &buf));
    WT_ERR(__clsm_put(session, clsm, &cursor->key, &value, true, false));

    /*
     * Set the cursor to reference the internal key/value of the positioned cursor.
     */
    F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
    WT_ITEM_SET(cursor->key, clsm->current->key);
    WT_ITEM_SET(cursor->value, clsm->current->value);
    WT_ASSERT(session, F_MASK(clsm->current, WT_CURSTD_KEY_SET) == WT_CURSTD_KEY_INT);
    WT_ASSERT(session, F_MASK(clsm->current, WT_CURSTD_VALUE_SET) == WT_CURSTD_VALUE_INT);
    F_SET(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);

err:
    __wt_scr_free(session, &buf);
    __clsm_leave(clsm);
    CURSOR_UPDATE_API_END(session, ret);
    return (ret);
}

/*
 * __clsm_remove --
 *     WT_CURSOR->remove method for the LSM cursor type.
 */
static int
__clsm_remove(WT_CURSOR *cursor)
{
    WT_CURSOR_LSM *clsm;
    WT_DECL_RET;
    WT_ITEM value;
    WT_SESSION_IMPL *session;
    bool positioned;

    clsm = (WT_CURSOR_LSM *)cursor;

    /* Check if the cursor is positioned. */
    positioned = F_ISSET(cursor, WT_CURSTD_KEY_INT);

    CURSOR_REMOVE_API_CALL(cursor, session, NULL);
    WT_ERR(__cursor_needkey(cursor));
    __cursor_novalue(cursor);
    WT_ERR(__clsm_enter(clsm, false, true));

    if (!F_ISSET(cursor, WT_CURSTD_OVERWRITE)) {
        WT_ERR(__clsm_lookup(clsm, &value));
        /*
         * Copy the key out, since the insert resets non-primary chunk cursors which our lookup may
         * have landed on.
         */
        WT_ERR(__cursor_needkey(cursor));
    }
    WT_ERR(__clsm_put(session, clsm, &cursor->key, &__tombstone, positioned, false));

    /*
     * If the cursor was positioned, it stays positioned with a key but no no value, otherwise,
     * there's no position, key or value. This isn't just cosmetic, without a reset, iteration on
     * this cursor won't start at the beginning/end of the table.
     */
    F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
    if (positioned)
        F_SET(cursor, WT_CURSTD_KEY_INT);
    else
        WT_TRET(cursor->reset(cursor));

err:
    __clsm_leave(clsm);
    CURSOR_UPDATE_API_END(session, ret);
    return (ret);
}

/*
 * __clsm_reserve --
 *     WT_CURSOR->reserve method for the LSM cursor type.
 */
static int
__clsm_reserve(WT_CURSOR *cursor)
{
    WT_CURSOR_LSM *clsm;
    WT_DECL_RET;
    WT_ITEM value;
    WT_SESSION_IMPL *session;

    clsm = (WT_CURSOR_LSM *)cursor;

    CURSOR_UPDATE_API_CALL(cursor, session, reserve);
    WT_ERR(__cursor_needkey(cursor));
    __cursor_novalue(cursor);
    WT_ERR(__wt_txn_context_check(session, true));
    WT_ERR(__clsm_enter(clsm, false, true));

    WT_ERR(__clsm_lookup(clsm, &value));
    /*
     * Copy the key out, since the insert resets non-primary chunk cursors which our lookup may have
     * landed on.
     */
    WT_ERR(__cursor_needkey(cursor));
    ret = __clsm_put(session, clsm, &cursor->key, NULL, true, true);

err:
    __clsm_leave(clsm);
    CURSOR_UPDATE_API_END(session, ret);

    /*
     * The application might do a WT_CURSOR.get_value call when we return, so we need a value and
     * the underlying functions didn't set one up. For various reasons, those functions may not have
     * done a search and any previous value in the cursor might race with WT_CURSOR.reserve (and in
     * cases like LSM, the reserve never encountered the original key). For simplicity, repeat the
     * search here.
     */
    return (ret == 0 ? cursor->search(cursor) : ret);
}

/*
 * __wt_clsm_close --
 *     WT_CURSOR->close method for the LSM cursor type.
 */
int
__wt_clsm_close(WT_CURSOR *cursor)
{
    WT_CURSOR_LSM *clsm;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    /*
     * Don't use the normal __clsm_enter path: that is wasted work when closing, and the cursor may
     * never have been used.
     */
    clsm = (WT_CURSOR_LSM *)cursor;
    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, close, NULL);
err:

    WT_TRET(__clsm_close_cursors(session, clsm, 0, clsm->nchunks));
    __clsm_free_chunks(session, clsm);

    /* In case we were somehow left positioned, clear that. */
    __clsm_leave(clsm);

    if (clsm->lsm_tree != NULL)
        __wt_lsm_tree_release(session, clsm->lsm_tree);
    __wt_cursor_close(cursor);

    API_END_RET(session, ret);
}

/*
 * __wt_clsm_open --
 *     WT_SESSION->open_cursor method for LSM cursors.
 */
int
__wt_clsm_open(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *owner, const char *cfg[],
  WT_CURSOR **cursorp)
{
    WT_CONFIG_ITEM cval;
    WT_CURSOR_STATIC_INIT(iface, __wt_cursor_get_key, /* get-key */
      __wt_cursor_get_value,                          /* get-value */
      __wt_cursor_set_key,                            /* set-key */
      __wt_cursor_set_value,                          /* set-value */
      __clsm_compare,                                 /* compare */
      __wt_cursor_equals,                             /* equals */
      __clsm_next,                                    /* next */
      __clsm_prev,                                    /* prev */
      __clsm_reset,                                   /* reset */
      __clsm_search,                                  /* search */
      __clsm_search_near,                             /* search-near */
      __clsm_insert,                                  /* insert */
      __wt_cursor_modify_value_format_notsup,         /* modify */
      __clsm_update,                                  /* update */
      __clsm_remove,                                  /* remove */
      __clsm_reserve,                                 /* reserve */
      __wt_cursor_reconfigure,                        /* reconfigure */
      __wt_cursor_notsup,                             /* largest_key */
      __wt_cursor_notsup,                             /* cache */
      __wt_cursor_reopen_notsup,                      /* reopen */
      __wt_clsm_close);                               /* close */
    WT_CURSOR *cursor;
    WT_CURSOR_LSM *clsm;
    WT_DECL_RET;
    WT_LSM_TREE *lsm_tree;
    bool bulk;

    WT_STATIC_ASSERT(offsetof(WT_CURSOR_LSM, iface) == 0);

    clsm = NULL;
    cursor = NULL;
    lsm_tree = NULL;

    if (!WT_PREFIX_MATCH(uri, "lsm:"))
        return (__wt_unexpected_object_type(session, uri, "lsm:"));

    WT_RET(__wt_inmem_unsupported_op(session, "LSM trees"));

    WT_RET(__wt_config_gets_def(session, cfg, "checkpoint", 0, &cval));
    if (cval.len != 0)
        WT_RET_MSG(session, EINVAL, "LSM does not support opening by checkpoint");

    WT_RET(__wt_config_gets_def(session, cfg, "bulk", 0, &cval));
    bulk = cval.val != 0;

    /* Get the LSM tree. */
    ret = __wt_lsm_tree_get(session, uri, bulk, &lsm_tree);

    /*
     * Check whether the exclusive open for a bulk load succeeded, and if it did ensure that it's
     * safe to bulk load into the tree.
     */
    if (bulk && (ret == EBUSY || (ret == 0 && lsm_tree->nchunks > 1)))
        WT_ERR_MSG(session, EINVAL, "bulk-load is only supported on newly created LSM trees");
    /* Flag any errors from the tree get. */
    WT_ERR(ret);

    /* Make sure we have exclusive access if and only if we want it */
    WT_ASSERT(session, !bulk || lsm_tree->excl_session != NULL);

    WT_ERR(__wt_calloc_one(session, &clsm));
    cursor = (WT_CURSOR *)clsm;
    *cursor = iface;
    cursor->session = (WT_SESSION *)session;
    WT_ERR(__wt_strdup(session, lsm_tree->name, &cursor->uri));
    cursor->key_format = lsm_tree->key_format;
    cursor->value_format = lsm_tree->value_format;

    clsm->lsm_tree = lsm_tree;
    lsm_tree = NULL;

    /*
     * The tree's dsk_gen starts at one, so starting the cursor on zero will force a call into
     * open_cursors on the first operation.
     */
    clsm->dsk_gen = 0;

    /* If the next_random option is set, configure a random cursor */
    WT_ERR(__wt_config_gets_def(session, cfg, "next_random", 0, &cval));
    if (cval.val != 0) {
        __wt_cursor_set_notsup(cursor);
        cursor->next = __clsm_next_random;
    }

    WT_ERR(__wt_cursor_init(cursor, cursor->uri, owner, cfg, cursorp));

    if (bulk)
        WT_ERR(__wt_clsm_open_bulk(clsm, cfg));

    if (0) {
err:
        if (clsm != NULL)
            WT_TRET(__wt_clsm_close(cursor));
        else if (lsm_tree != NULL)
            __wt_lsm_tree_release(session, lsm_tree);

        *cursorp = NULL;
    }

    return (ret);
}
