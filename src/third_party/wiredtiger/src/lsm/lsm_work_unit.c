/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __lsm_bloom_create(WT_SESSION_IMPL *, WT_LSM_TREE *, WT_LSM_CHUNK *, u_int);
static int __lsm_discard_handle(WT_SESSION_IMPL *, const char *, const char *);

/*
 * __lsm_copy_chunks --
 *     Take a copy of part of the LSM tree chunk array so that we can work on the contents without
 *     holding the LSM tree handle lock long term.
 */
static int
__lsm_copy_chunks(
  WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, WT_LSM_WORKER_COOKIE *cookie, bool old_chunks)
{
    WT_DECL_RET;
    size_t alloc;
    u_int i, nchunks;

    /* Always return zero chunks on error. */
    cookie->nchunks = 0;

    __wt_lsm_tree_readlock(session, lsm_tree);
    if (!lsm_tree->active) {
        __wt_lsm_tree_readunlock(session, lsm_tree);
        return (0);
    }

    /* Take a copy of the current state of the LSM tree. */
    nchunks = old_chunks ? lsm_tree->nold_chunks : lsm_tree->nchunks;
    alloc = old_chunks ? lsm_tree->old_alloc : lsm_tree->chunk_alloc;
    WT_ASSERT(session, alloc > 0 && nchunks > 0);

    /*
     * If the tree array of active chunks is larger than our current buffer, increase the size of
     * our current buffer to match.
     */
    if (cookie->chunk_alloc < alloc)
        WT_ERR(__wt_realloc(session, &cookie->chunk_alloc, alloc, &cookie->chunk_array));
    if (nchunks > 0)
        memcpy(cookie->chunk_array, old_chunks ? lsm_tree->old_chunks : lsm_tree->chunk,
          nchunks * sizeof(*cookie->chunk_array));

    /*
     * Mark each chunk as active, so we don't drop it until after we know it's safe.
     */
    for (i = 0; i < nchunks; i++)
        (void)__wt_atomic_add32(&cookie->chunk_array[i]->refcnt, 1);

err:
    __wt_lsm_tree_readunlock(session, lsm_tree);

    if (ret == 0)
        cookie->nchunks = nchunks;
    return (ret);
}

/*
 * __wt_lsm_get_chunk_to_flush --
 *     Find and pin a chunk in the LSM tree that is likely to need flushing.
 */
int
__wt_lsm_get_chunk_to_flush(
  WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, bool force, WT_LSM_CHUNK **chunkp)
{
    WT_DECL_RET;
    WT_LSM_CHUNK *chunk, *evict_chunk, *flush_chunk;
    uint32_t i;

    *chunkp = NULL;

    chunk = evict_chunk = flush_chunk = NULL;

    WT_ASSERT(session, lsm_tree->queue_ref > 0);
    __wt_lsm_tree_readlock(session, lsm_tree);
    if (!lsm_tree->active || lsm_tree->nchunks == 0) {
        __wt_lsm_tree_readunlock(session, lsm_tree);
        return (0);
    }

    /* Search for a chunk to evict and/or a chunk to flush. */
    for (i = 0; i < lsm_tree->nchunks; i++) {
        chunk = lsm_tree->chunk[i];
        if (F_ISSET(chunk, WT_LSM_CHUNK_ONDISK)) {
            /*
             * Normally we don't want to force out the last chunk. But if we're doing a forced flush
             * on behalf of a compact, then we want to include the final chunk.
             */
            if (evict_chunk == NULL && !chunk->evicted && !F_ISSET(chunk, WT_LSM_CHUNK_STABLE))
                evict_chunk = chunk;
        } else if (flush_chunk == NULL && chunk->switch_txn != 0 &&
          (force || i < lsm_tree->nchunks - 1))
            flush_chunk = chunk;
    }

    /*
     * Don't be overly zealous about pushing old chunks from cache. Attempting too many drops can
     * interfere with checkpoints.
     *
     * If retrying a discard push an additional work unit so there are enough to trigger
     * checkpoints.
     */
    if (evict_chunk != NULL && flush_chunk != NULL) {
        chunk = (__wt_random(&session->rnd) & 1) ? evict_chunk : flush_chunk;
        WT_ERR(__wt_lsm_manager_push_entry(session, WT_LSM_WORK_FLUSH, 0, lsm_tree));
    } else
        chunk = (evict_chunk != NULL) ? evict_chunk : flush_chunk;

    if (chunk != NULL) {
        __wt_verbose_debug2(session, WT_VERB_LSM,
          "Flush%s: return chunk %" PRIu32 " of %" PRIu32 ": %s", force ? " w/ force" : "", i,
          lsm_tree->nchunks, chunk->uri);

        (void)__wt_atomic_add32(&chunk->refcnt, 1);
    }

err:
    __wt_lsm_tree_readunlock(session, lsm_tree);
    *chunkp = chunk;
    return (ret);
}

/*
 * __lsm_unpin_chunks --
 *     Decrement the reference count for a set of chunks. Allowing those chunks to be considered for
 *     deletion.
 */
static void
__lsm_unpin_chunks(WT_SESSION_IMPL *session, WT_LSM_WORKER_COOKIE *cookie)
{
    u_int i;

    for (i = 0; i < cookie->nchunks; i++) {
        if (cookie->chunk_array[i] == NULL)
            continue;
        WT_ASSERT(session, cookie->chunk_array[i]->refcnt > 0);
        (void)__wt_atomic_sub32(&cookie->chunk_array[i]->refcnt, 1);
    }
    /* Ensure subsequent calls don't double decrement. */
    cookie->nchunks = 0;
}

/*
 * __wt_lsm_work_switch --
 *     Do a switch if the LSM tree needs one.
 */
int
__wt_lsm_work_switch(WT_SESSION_IMPL *session, WT_LSM_WORK_UNIT **entryp, bool *ran)
{
    WT_DECL_RET;
    WT_LSM_WORK_UNIT *entry;

    /* We've become responsible for freeing the work unit. */
    entry = *entryp;
    *entryp = NULL;
    *ran = false;

    if (entry->lsm_tree->need_switch) {
        WT_WITH_SCHEMA_LOCK(session, ret = __wt_lsm_tree_switch(session, entry->lsm_tree));
        /* Failing to complete the switch is fine */
        if (ret == EBUSY) {
            if (entry->lsm_tree->need_switch)
                WT_ERR(
                  __wt_lsm_manager_push_entry(session, WT_LSM_WORK_SWITCH, 0, entry->lsm_tree));
            ret = 0;
        } else
            *ran = true;
    }
err:
    __wt_lsm_manager_free_work_unit(session, entry);
    return (ret);
}

/*
 * __wt_lsm_work_bloom --
 *     Try to create a Bloom filter for the newest on-disk chunk that doesn't have one.
 */
int
__wt_lsm_work_bloom(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
    WT_DECL_RET;
    WT_LSM_CHUNK *chunk;
    WT_LSM_WORKER_COOKIE cookie;
    u_int i, merge;

    WT_CLEAR(cookie);

    WT_RET(__lsm_copy_chunks(session, lsm_tree, &cookie, false));

    /* Create bloom filters in all checkpointed chunks. */
    merge = 0;
    for (i = 0; i < cookie.nchunks; i++) {
        chunk = cookie.chunk_array[i];

        /*
         * Skip if a thread is still active in the chunk or it isn't suitable.
         */
        if (!F_ISSET(chunk, WT_LSM_CHUNK_ONDISK) ||
          F_ISSET(chunk, WT_LSM_CHUNK_BLOOM | WT_LSM_CHUNK_MERGING) || chunk->generation > 0 ||
          chunk->count == 0)
            continue;

        /* Never create a bloom filter on the oldest chunk */
        if (chunk == lsm_tree->chunk[0] && !FLD_ISSET(lsm_tree->bloom, WT_LSM_BLOOM_OLDEST))
            continue;
        /*
         * See if we win the race to switch on the "busy" flag and recheck that the chunk still
         * needs a Bloom filter.
         */
        if (__wt_atomic_cas32(&chunk->bloom_busy, 0, 1)) {
            if (!F_ISSET(chunk, WT_LSM_CHUNK_BLOOM)) {
                ret = __lsm_bloom_create(session, lsm_tree, chunk, i);
                /*
                 * Record if we were successful so that we can later push a merge work unit.
                 */
                if (ret == 0)
                    merge = 1;
            }
            chunk->bloom_busy = 0;
            break;
        }
    }
    /*
     * If we created any bloom filters, we push a merge work unit now.
     */
    if (merge)
        WT_ERR(__wt_lsm_manager_push_entry(session, WT_LSM_WORK_MERGE, 0, lsm_tree));

err:
    __lsm_unpin_chunks(session, &cookie);
    __wt_free(session, cookie.chunk_array);
    return (ret);
}

/*
 * __wt_lsm_chunk_visible_all --
 *     Setup a timestamp and check visibility for a chunk, can be called from multiple threads in
 *     parallel
 */
bool
__wt_lsm_chunk_visible_all(WT_SESSION_IMPL *session, WT_LSM_CHUNK *chunk)
{
    WT_TXN_GLOBAL *txn_global;

    txn_global = &S2C(session)->txn_global;

    /* Once a chunk has been flushed it's contents must be visible */
    if (F_ISSET(chunk, WT_LSM_CHUNK_ONDISK | WT_LSM_CHUNK_STABLE))
        return (true);

    if (chunk->switch_txn == WT_TXN_NONE ||
      !__wt_txn_visible_all(session, chunk->switch_txn, WT_TS_NONE))
        return (false);

    /*
     * Once all transactions with updates in the chunk are visible all timestamps associated with
     * those updates are assigned so setup a timestamp for visibility checking.
     */
    if (txn_global->has_durable_timestamp || txn_global->has_pinned_timestamp) {
        if (!F_ISSET(chunk, WT_LSM_CHUNK_HAS_TIMESTAMP)) {
            __wt_spin_lock(session, &chunk->timestamp_spinlock);
            /* Set the timestamp if we won the race */
            if (!F_ISSET(chunk, WT_LSM_CHUNK_HAS_TIMESTAMP)) {
                __wt_readlock(session, &txn_global->rwlock);
                chunk->switch_timestamp = txn_global->durable_timestamp;
                __wt_readunlock(session, &txn_global->rwlock);
                F_SET(chunk, WT_LSM_CHUNK_HAS_TIMESTAMP);
            }
            __wt_spin_unlock(session, &chunk->timestamp_spinlock);
        }
        if (!__wt_txn_visible_all(session, chunk->switch_txn, chunk->switch_timestamp))
            return (false);
    } else
        /*
         * If timestamps aren't in use when the chunk becomes visible use the zero timestamp for
         * visibility checks. Otherwise there could be confusion if timestamps start being used.
         */
        F_SET(chunk, WT_LSM_CHUNK_HAS_TIMESTAMP);

    return (true);
}

/*
 * __lsm_set_chunk_evictable --
 *     Enable eviction in an LSM chunk.
 */
static int
__lsm_set_chunk_evictable(WT_SESSION_IMPL *session, WT_LSM_CHUNK *chunk, bool need_handle)
{
    WT_BTREE *btree;
    WT_DECL_RET;

    if (chunk->evict_enabled != 0)
        return (0);

    /* See if we win the race to enable eviction. */
    if (__wt_atomic_cas32(&chunk->evict_enabled, 0, 1)) {
        if (need_handle)
            WT_RET(__wt_session_get_dhandle(session, chunk->uri, NULL, NULL, 0));
        btree = session->dhandle->handle;
        if (btree->evict_disabled_open) {
            btree->evict_disabled_open = false;
            __wt_evict_file_exclusive_off(session);
        }

        if (need_handle)
            WT_TRET(__wt_session_release_dhandle(session));
    }
    return (ret);
}

/*
 * __lsm_checkpoint_chunk --
 *     Checkpoint an LSM chunk, separated out to make locking easier.
 */
static int
__lsm_checkpoint_chunk(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;

    /*
     * Turn on metadata tracking to ensure the checkpoint gets the necessary handle locks.
     */
    WT_RET(__wt_meta_track_on(session));
    ret = __wt_checkpoint(session, NULL);
    WT_TRET(__wt_meta_track_off(session, false, ret != 0));

    return (ret);
}

/*
 * __wt_lsm_checkpoint_chunk --
 *     Flush a single LSM chunk to disk.
 */
int
__wt_lsm_checkpoint_chunk(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, WT_LSM_CHUNK *chunk)
{
    WT_DECL_RET;
    WT_TXN_ISOLATION saved_isolation;
    bool flush_set, release_dhandle;

    WT_NOT_READ(flush_set, false);
    release_dhandle = false;

    /*
     * If the chunk is already checkpointed, make sure it is also evicted. Either way, there is no
     * point trying to checkpoint it again.
     */
    if (F_ISSET(chunk, WT_LSM_CHUNK_ONDISK) && !F_ISSET(chunk, WT_LSM_CHUNK_STABLE) &&
      !chunk->evicted) {
        WT_WITH_HANDLE_LIST_WRITE_LOCK(
          session, ret = __lsm_discard_handle(session, chunk->uri, NULL));
        if (ret == 0)
            chunk->evicted = 1;
        else if (ret == EBUSY) {
            WT_NOT_READ(ret, 0);
        } else
            WT_RET_MSG(session, ret, "discard handle");
    }
    if (F_ISSET(chunk, WT_LSM_CHUNK_ONDISK)) {
        __wt_verbose_debug2(session, WT_VERB_LSM, "LSM worker %s already on disk", chunk->uri);
        return (0);
    }

    /* Stop if a running transaction needs the chunk. */
    WT_RET(__wt_txn_update_oldest(session, WT_TXN_OLDEST_STRICT | WT_TXN_OLDEST_WAIT));
    if (!__wt_lsm_chunk_visible_all(session, chunk)) {
        /*
         * If there is cache pressure consider making a chunk evictable to avoid the cache getting
         * stuck when history is required.
         */
        if (__wt_eviction_needed(session, false, false, NULL))
            WT_ERR(__wt_lsm_manager_push_entry(session, WT_LSM_WORK_ENABLE_EVICT, 0, lsm_tree));

        __wt_verbose_debug2(
          session, WT_VERB_LSM, "LSM worker %s: running transaction, return", chunk->uri);
        return (0);
    }
    if (!__wt_atomic_cas8(&chunk->flushing, 0, 1))
        return (0);
    flush_set = true;

    __wt_verbose_debug2(session, WT_VERB_LSM, "LSM worker flushing %s", chunk->uri);

    /*
     * Flush the file before checkpointing: this is the expensive part in
     * terms of I/O.
     *
     * !!!
     * We can wait here for checkpoints and fsyncs to complete, which can
     * take a long time.
     */
    WT_ERR(__wt_session_get_dhandle(session, chunk->uri, NULL, NULL, 0));
    release_dhandle = true;

    /*
     * Set read-uncommitted: we have already checked that all of the updates in this chunk are
     * globally visible, use the cheapest possible check in reconciliation.
     */
    saved_isolation = session->txn->isolation;
    session->txn->isolation = WT_ISO_READ_UNCOMMITTED;
    ret = __wt_sync_file(session, WT_SYNC_WRITE_LEAVES);
    session->txn->isolation = saved_isolation;
    WT_ERR(ret);

    __wt_verbose_debug2(session, WT_VERB_LSM, "LSM worker checkpointing %s", chunk->uri);

    /*
     * Ensure we don't race with a running checkpoint: the checkpoint lock protects against us
     * racing with an application checkpoint in this chunk.
     */
    WT_WITH_CHECKPOINT_LOCK(
      session, WT_WITH_SCHEMA_LOCK(session, ret = __lsm_checkpoint_chunk(session)));
    if (ret != 0)
        WT_ERR_MSG(session, ret, "LSM checkpoint");

    /* Now the file is written, get the chunk size. */
    WT_ERR(__wt_lsm_tree_set_chunk_size(session, lsm_tree, chunk));

    ++lsm_tree->chunks_flushed;

    /* Lock the tree, mark the chunk as on disk and update the metadata. */
    __wt_lsm_tree_writelock(session, lsm_tree);
    /* Update the flush timestamp to help track ongoing progress. */
    __wt_epoch(session, &lsm_tree->last_flush_time);
    F_SET(chunk, WT_LSM_CHUNK_ONDISK);
    ret = __wt_lsm_meta_write(session, lsm_tree, NULL);
    ++lsm_tree->dsk_gen;

    /* Update the throttle time. */
    __wt_lsm_tree_throttle(session, lsm_tree, true);
    __wt_lsm_tree_writeunlock(session, lsm_tree);
    if (ret != 0)
        WT_ERR_MSG(session, ret, "LSM metadata write");

    /*
     * Enable eviction on the live chunk so it doesn't block the cache. Future reads should direct
     * to the on-disk chunk anyway.
     */
    WT_ERR(__lsm_set_chunk_evictable(session, chunk, false));

    release_dhandle = false;
    WT_ERR(__wt_session_release_dhandle(session));

    WT_PUBLISH(chunk->flushing, 0);
    flush_set = false;

    /* Make sure we aren't pinning a transaction ID. */
    __wt_txn_release_snapshot(session);

    __wt_verbose_debug2(session, WT_VERB_LSM, "LSM worker checkpointed %s", chunk->uri);

    /* Schedule a bloom filter create for our newly flushed chunk. */
    if (!FLD_ISSET(lsm_tree->bloom, WT_LSM_BLOOM_OFF))
        WT_ERR(__wt_lsm_manager_push_entry(session, WT_LSM_WORK_BLOOM, 0, lsm_tree));
    else
        WT_ERR(__wt_lsm_manager_push_entry(session, WT_LSM_WORK_MERGE, 0, lsm_tree));

err:
    if (flush_set)
        WT_PUBLISH(chunk->flushing, 0);
    if (release_dhandle)
        WT_TRET(__wt_session_release_dhandle(session));

    return (ret);
}

/*
 * __wt_lsm_work_enable_evict --
 *     LSM usually pins live chunks in memory - preferring to force them out via a checkpoint when
 *     they are no longer required. For applications that keep data pinned for a long time this can
 *     lead to the cache being pinned full. This work unit detects that case, and enables regular
 *     eviction in chunks that can be correctly evicted.
 */
int
__wt_lsm_work_enable_evict(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
    WT_DECL_RET;
    WT_LSM_CHUNK *chunk;
    WT_LSM_WORKER_COOKIE cookie;
    u_int i;

    WT_CLEAR(cookie);

    /* Only do this if there is cache pressure */
    if (!__wt_eviction_needed(session, false, false, NULL))
        return (0);

    WT_RET(__lsm_copy_chunks(session, lsm_tree, &cookie, false));

    /*
     * Turn on eviction in chunks that have had some chance to checkpoint if there is cache
     * pressure.
     */
    for (i = 0; cookie.nchunks > 2 && i < cookie.nchunks - 2; i++) {
        chunk = cookie.chunk_array[i];

        /*
         * Skip if the chunk isn't on disk yet, or if it's still in cache for a reason other than
         * transaction visibility.
         */
        if (!F_ISSET(chunk, WT_LSM_CHUNK_ONDISK) || chunk->evict_enabled != 0 ||
          __wt_lsm_chunk_visible_all(session, chunk))
            continue;

        WT_ERR(__lsm_set_chunk_evictable(session, chunk, true));
    }

err:
    __lsm_unpin_chunks(session, &cookie);
    __wt_free(session, cookie.chunk_array);
    return (ret);
}

/*
 * __lsm_bloom_create --
 *     Create a bloom filter for a chunk of the LSM tree that has been checkpointed but not yet been
 *     merged.
 */
static int
__lsm_bloom_create(
  WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, WT_LSM_CHUNK *chunk, u_int chunk_off)
{
    WT_BLOOM *bloom;
    WT_CURSOR *src;
    WT_DECL_RET;
    WT_ITEM key;
    uint64_t insert_count;

    WT_RET(__wt_lsm_tree_setup_bloom(session, lsm_tree, chunk));

    bloom = NULL;
    /*
     * This is merge-like activity, and we don't want compacts to give up because we are creating a
     * bunch of bloom filters before merging.
     */
    ++lsm_tree->merge_progressing;
    WT_RET(__wt_bloom_create(session, chunk->bloom_uri, lsm_tree->bloom_config, chunk->count,
      lsm_tree->bloom_bit_count, lsm_tree->bloom_hash_count, &bloom));

    /* Open a special merge cursor just on this chunk. */
    WT_ERR(__wt_open_cursor(session, lsm_tree->name, NULL, NULL, &src));
    F_SET(src, WT_CURSTD_RAW);
    WT_ERR(__wt_clsm_init_merge(src, chunk_off, chunk->id, 1));

    /*
     * Setup so that we don't hold pages we read into cache, and so that we don't get stuck if the
     * cache is full. If we allow ourselves to get stuck creating bloom filters, the entire tree can
     * stall since there may be no worker threads available to flush.
     */
    F_SET(session, WT_SESSION_IGNORE_CACHE_SIZE | WT_SESSION_READ_WONT_NEED);
    for (insert_count = 0; (ret = src->next(src)) == 0; insert_count++) {
        WT_ERR(src->get_key(src, &key));
        __wt_bloom_insert(bloom, &key);
    }
    WT_ERR_NOTFOUND_OK(ret, false);
    WT_TRET(src->close(src));

    WT_TRET(__wt_bloom_finalize(bloom));
    WT_ERR(ret);

    F_CLR(session, WT_SESSION_READ_WONT_NEED);

    /* Load the new Bloom filter into cache. */
    WT_CLEAR(key);
    WT_ERR_NOTFOUND_OK(__wt_bloom_get(bloom, &key), false);

    __wt_verbose(session, WT_VERB_LSM,
      "LSM worker created bloom filter %s. Expected %" PRIu64 " items, got %" PRIu64,
      chunk->bloom_uri, chunk->count, insert_count);

    /* Ensure the bloom filter is in the metadata. */
    __wt_lsm_tree_writelock(session, lsm_tree);
    F_SET(chunk, WT_LSM_CHUNK_BLOOM);
    ret = __wt_lsm_meta_write(session, lsm_tree, NULL);
    ++lsm_tree->dsk_gen;
    __wt_lsm_tree_writeunlock(session, lsm_tree);

    if (ret != 0)
        WT_ERR_MSG(session, ret, "LSM bloom worker metadata write");

err:
    if (bloom != NULL)
        WT_TRET(__wt_bloom_close(bloom));
    F_CLR(session, WT_SESSION_IGNORE_CACHE_SIZE | WT_SESSION_READ_WONT_NEED);
    return (ret);
}

/*
 * __lsm_discard_handle --
 *     Try to discard a handle from cache.
 */
static int
__lsm_discard_handle(WT_SESSION_IMPL *session, const char *uri, const char *checkpoint)
{
    /* This will fail with EBUSY if the file is still in use. */
    WT_RET(__wt_session_get_dhandle(
      session, uri, checkpoint, NULL, WT_DHANDLE_EXCLUSIVE | WT_DHANDLE_LOCK_ONLY));

    F_SET(session->dhandle, WT_DHANDLE_DISCARD_KILL);
    return (__wt_session_release_dhandle(session));
}

/*
 * __lsm_drop_file --
 *     Helper function to drop part of an LSM tree.
 */
static int
__lsm_drop_file(WT_SESSION_IMPL *session, const char *uri)
{
    WT_DECL_RET;
    const char *drop_cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_drop), "remove_files=false", NULL};

    /*
     * We need to grab the schema lock to drop the file, so first try to make sure there is minimal
     * work to freeing space in the cache. Only bother trying to discard the checkpoint handle: the
     * in-memory handle should have been closed already.
     *
     * This will fail with EBUSY if the file is still in use.
     */
    WT_WITH_HANDLE_LIST_WRITE_LOCK(
      session, ret = __lsm_discard_handle(session, uri, WT_CHECKPOINT));
    WT_RET(ret);

    /*
     * Take the schema lock for the drop operation. Since __wt_schema_drop results in the hot backup
     * lock being taken when it updates the metadata (which would be too late to prevent our drop).
     */
    WT_WITH_SCHEMA_LOCK(session, ret = __wt_schema_drop(session, uri, drop_cfg));

    if (ret == 0)
        ret = __wt_fs_remove(session, uri + strlen("file:"), false);
    __wt_verbose(session, WT_VERB_LSM, "Dropped %s", uri);

    if (ret == EBUSY || ret == ENOENT)
        __wt_verbose(session, WT_VERB_LSM, "LSM worker drop of %s failed with %d", uri, ret);

    return (ret);
}

/*
 * __lsm_free_chunks --
 *     Try to drop chunks from the tree that are no longer required.
 */
static int
__lsm_free_chunks(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
    WT_DECL_RET;
    WT_LSM_CHUNK *chunk;
    WT_LSM_WORKER_COOKIE cookie;
    u_int i, skipped;
    int drop_ret;
    bool flush_metadata;

    flush_metadata = false;

    /*
     * Take a copy of the current state of the LSM tree and look for chunks to drop. We do it this
     * way to avoid holding the LSM tree lock while doing I/O or waiting on the schema lock.
     *
     * This is safe because only one thread will be in this function at a time. Merges may complete
     * concurrently, and the old_chunks array may be extended, but we shuffle down the pointers each
     * time we free one to keep the non-NULL slots at the beginning of the array.
     */
    WT_CLEAR(cookie);
    WT_RET(__lsm_copy_chunks(session, lsm_tree, &cookie, true));
    for (i = skipped = 0; i < cookie.nchunks; i++) {
        chunk = cookie.chunk_array[i];
        WT_ASSERT(session, chunk != NULL);
        /* Skip the chunk if another worker is using it. */
        if (chunk->refcnt > 1) {
            ++skipped;
            continue;
        }

        /*
         * Drop any bloom filters and chunks we can. Don't try to drop
         * a chunk if the bloom filter drop fails.
         *  An EBUSY return indicates that a cursor is still open in
         *       the tree - move to the next chunk in that case.
         * An ENOENT return indicates that the LSM tree metadata was
         *       out of sync with the on disk state. Update the
         *       metadata to match in that case.
         */
        if (F_ISSET(chunk, WT_LSM_CHUNK_BLOOM)) {
            drop_ret = __lsm_drop_file(session, chunk->bloom_uri);
            if (drop_ret == EBUSY) {
                ++skipped;
                continue;
            }
            if (drop_ret != ENOENT)
                WT_ERR(drop_ret);

            flush_metadata = true;
            F_CLR(chunk, WT_LSM_CHUNK_BLOOM);
        }
        if (chunk->uri != NULL) {
            drop_ret = __lsm_drop_file(session, chunk->uri);
            if (drop_ret == EBUSY) {
                ++skipped;
                continue;
            }
            if (drop_ret != ENOENT)
                WT_ERR(drop_ret);
            flush_metadata = true;
        }

        /* Lock the tree to clear out the old chunk information. */
        __wt_lsm_tree_writelock(session, lsm_tree);

        /*
         * The chunk we are looking at should be the first one in the tree that we haven't already
         * skipped over.
         */
        WT_ASSERT(session, lsm_tree->old_chunks[skipped] == chunk);
        __wt_free(session, chunk->bloom_uri);
        __wt_free(session, chunk->uri);
        __wt_free(session, lsm_tree->old_chunks[skipped]);

        /* Shuffle down to keep all occupied slots at the beginning. */
        if (--lsm_tree->nold_chunks > skipped) {
            memmove(lsm_tree->old_chunks + skipped, lsm_tree->old_chunks + skipped + 1,
              (lsm_tree->nold_chunks - skipped) * sizeof(WT_LSM_CHUNK *));
            lsm_tree->old_chunks[lsm_tree->nold_chunks] = NULL;
        }

        __wt_lsm_tree_writeunlock(session, lsm_tree);

        /*
         * Clear the chunk in the cookie so we don't attempt to decrement the reference count.
         */
        cookie.chunk_array[i] = NULL;
    }

err:
    /* Flush the metadata unless the system is in panic */
    if (flush_metadata && ret != WT_PANIC) {
        __wt_lsm_tree_writelock(session, lsm_tree);
        WT_TRET(__wt_lsm_meta_write(session, lsm_tree, NULL));
        __wt_lsm_tree_writeunlock(session, lsm_tree);
    }
    __lsm_unpin_chunks(session, &cookie);
    __wt_free(session, cookie.chunk_array);

    /* Returning non-zero means there is no work to do. */
    if (!flush_metadata)
        WT_TRET(WT_NOTFOUND);

    return (ret);
}

/*
 * __wt_lsm_free_chunks --
 *     Try to drop chunks from the tree that are no longer required.
 */
int
__wt_lsm_free_chunks(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
    WT_DECL_RET;

    if (lsm_tree->nold_chunks == 0)
        return (0);

    /*
     * Make sure only a single thread is freeing the old chunk array at any time.
     */
    if (!__wt_atomic_cas32(&lsm_tree->freeing_old_chunks, 0, 1))
        return (0);

    ret = __lsm_free_chunks(session, lsm_tree);

    lsm_tree->freeing_old_chunks = 0;
    return (ret);
}
