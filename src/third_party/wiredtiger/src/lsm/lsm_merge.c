/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __lsm_merge_span(WT_SESSION_IMPL *, WT_LSM_TREE *, u_int, u_int *, u_int *, uint64_t *);

/*
 * __wt_lsm_merge_update_tree --
 *     Merge a set of chunks and populate a new one. Must be called with the LSM lock held.
 */
int
__wt_lsm_merge_update_tree(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, u_int start_chunk,
  u_int nchunks, WT_LSM_CHUNK *chunk)
{
    size_t chunks_after_merge;

    WT_RET(__wt_lsm_tree_retire_chunks(session, lsm_tree, start_chunk, nchunks));

    /* Update the current chunk list. */
    chunks_after_merge = lsm_tree->nchunks - (nchunks + start_chunk);
    memmove(lsm_tree->chunk + start_chunk + 1, lsm_tree->chunk + start_chunk + nchunks,
      chunks_after_merge * sizeof(*lsm_tree->chunk));
    lsm_tree->nchunks -= nchunks - 1;
    memset(lsm_tree->chunk + lsm_tree->nchunks, 0, (nchunks - 1) * sizeof(*lsm_tree->chunk));
    lsm_tree->chunk[start_chunk] = chunk;

    return (0);
}

/*
 * __lsm_merge_aggressive_clear --
 *     We found a merge to do - clear the aggressive timer.
 */
static void
__lsm_merge_aggressive_clear(WT_LSM_TREE *lsm_tree)
{
    lsm_tree->aggressive_timer_enabled = false;
    lsm_tree->merge_aggressiveness = 0;
}

/*
 * __lsm_merge_aggressive_update --
 *     Update the merge aggressiveness for an LSM tree.
 */
static void
__lsm_merge_aggressive_update(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
    struct timespec now;
    uint64_t msec_since_last_merge, msec_to_create_merge;
    uint32_t new_aggressive;

    new_aggressive = 0;

    WT_ASSERT(session, lsm_tree->merge_min != 0);
    /*
     * If the tree is open read-only or we are compacting, be very aggressive. Otherwise, we can
     * spend a long time waiting for merges to start in read-only applications.
     */
    if (!lsm_tree->modified || F_ISSET(lsm_tree, WT_LSM_TREE_COMPACTING)) {
        lsm_tree->merge_aggressiveness = 10;
        return;
    }

    /*
     * Only get aggressive if a reasonable number of flushes have been completed since opening the
     * tree.
     */
    if (lsm_tree->chunks_flushed <= lsm_tree->merge_min) {
        __lsm_merge_aggressive_clear(lsm_tree);
        return;
    }

    /*
     * Start the timer if it isn't running. Use a bool to define whether the timer is running -
     * since clearing and checking a special timer value isn't simple.
     */
    if (!lsm_tree->aggressive_timer_enabled) {
        lsm_tree->aggressive_timer_enabled = true;
        __wt_epoch(session, &lsm_tree->merge_aggressive_time);
    }

    __wt_epoch(session, &now);
    msec_since_last_merge = WT_TIMEDIFF_MS(now, lsm_tree->merge_aggressive_time);

    /*
     * If there is no estimate for how long it's taking to fill chunks pick 10 seconds.
     */
    msec_to_create_merge =
      lsm_tree->merge_min * (lsm_tree->chunk_fill_ms == 0 ? 10000 : lsm_tree->chunk_fill_ms);

    /*
     * Don't consider getting aggressive until enough time has passed that we should have created
     * enough chunks to trigger a new merge. We track average chunk-creation time - hence the
     * "should"; the average fill time may not reflect the actual state if an application generates
     * a variable load.
     */
    if (msec_since_last_merge < msec_to_create_merge)
        return;

    /*
     * Bump how aggressively we look for merges based on how long since the last merge complete. The
     * aggressive setting only increases slowly - triggering merges across generations of chunks
     * isn't an efficient use of resources.
     */
    while ((msec_since_last_merge /= msec_to_create_merge) > 1)
        ++new_aggressive;

    if (new_aggressive > lsm_tree->merge_aggressiveness) {
        __wt_verbose(session, WT_VERB_LSM,
          "LSM merge %s got aggressive (old %" PRIu32 " new %" PRIu32 "), merge_min %u, %" PRIu64
          " / %" PRIu64,
          lsm_tree->name, lsm_tree->merge_aggressiveness, new_aggressive, lsm_tree->merge_min,
          msec_since_last_merge, lsm_tree->chunk_fill_ms);
        lsm_tree->merge_aggressiveness = new_aggressive;
    }
}

/*
 * __lsm_merge_clear --
 *     Clear merge flag on chunks that was set during __lsm_merge_span.
 */
static void
__lsm_merge_clear(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, u_int start, u_int nchunks)
{
    WT_LSM_CHUNK *chunk;
    u_int i;

    for (i = 0; i < nchunks; i++) {
        chunk = lsm_tree->chunk[start + i];
        WT_ASSERT(session, F_ISSET(chunk, WT_LSM_CHUNK_MERGING));
        F_CLR(chunk, WT_LSM_CHUNK_MERGING);
    }
}

/*
 * __lsm_merge_span --
 *     Figure out the best span of chunks to merge. Return an error if there is no need to do any
 *     merges. Called with the LSM tree locked.
 */
static int
__lsm_merge_span(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, u_int id, u_int *start,
  u_int *end, uint64_t *records)
{
    WT_LSM_CHUNK *chunk, *youngest;
    uint64_t chunk_size, record_count;
    uint32_t aggressive, max_gap, max_level;
    u_int end_chunk, merge_max, merge_min, nchunks, start_chunk;
    u_int oldest_gen, youngest_gen;
#ifdef HAVE_DIAGNOSTIC
    u_int i;
#endif
    /* Clear the return parameters */
    *start = *end = 0;
    *records = 0;

    chunk = youngest = NULL;

    aggressive = lsm_tree->merge_aggressiveness;
    merge_max = (aggressive > WT_LSM_AGGRESSIVE_THRESHOLD) ? 100 : lsm_tree->merge_max;
    merge_min = (aggressive > WT_LSM_AGGRESSIVE_THRESHOLD) ? 2 : lsm_tree->merge_min;
    max_gap = (aggressive + 4) / 5;
    max_level = (lsm_tree->merge_throttle > 0) ? 0 : id + aggressive;

    /*
     * If there aren't any chunks to merge, or some of the chunks aren't yet written, we're done. A
     * non-zero error indicates that the worker should assume there is no work to do: if there are
     * unwritten chunks, the worker should write them immediately.
     */
    if (lsm_tree->nchunks < merge_min)
        return (WT_NOTFOUND);

    /*
     * Only include chunks that already have a Bloom filter or are the result of a merge and not
     * involved in a merge.
     */
    for (end_chunk = lsm_tree->nchunks - 1; end_chunk > 0; --end_chunk) {
        chunk = lsm_tree->chunk[end_chunk];
        WT_ASSERT(session, chunk != NULL);
        if (F_ISSET(chunk, WT_LSM_CHUNK_MERGING))
            continue;
        if (F_ISSET(chunk, WT_LSM_CHUNK_BLOOM) || chunk->generation > 0)
            break;
        if (FLD_ISSET(lsm_tree->bloom, WT_LSM_BLOOM_OFF) && F_ISSET(chunk, WT_LSM_CHUNK_ONDISK))
            break;
    }

    /*
     * Give up immediately if there aren't enough on disk chunks in the tree for a merge.
     */
    if (end_chunk < merge_min - 1)
        return (WT_NOTFOUND);

/*
 * Look for the most efficient merge we can do. We define efficiency as collapsing as many levels as
 * possible while processing the smallest number of rows.
 *
 * We make a distinction between "major" and "minor" merges. The difference is whether the oldest
 * chunk is involved: if it is, we can discard tombstones, because there can be no older record to
 * marked deleted.
 *
 * Respect the configured limit on the number of chunks to merge: start with the most recent set of
 * chunks and work backwards until going further becomes significantly less efficient.
 */
retry_find:
    oldest_gen = youngest_gen = lsm_tree->chunk[end_chunk]->generation;
    for (chunk_size = record_count = 0, start_chunk = end_chunk + 1; start_chunk > 0;) {
        chunk = lsm_tree->chunk[start_chunk - 1];
        youngest = lsm_tree->chunk[end_chunk];
        nchunks = (end_chunk + 1) - start_chunk;

        /*
         * If the chunk is already involved in a merge or a Bloom filter is being built for it,
         * stop.
         */
        if (F_ISSET(chunk, WT_LSM_CHUNK_MERGING) || chunk->bloom_busy)
            break;

        /*
         * Look for small merges before trying a big one: some threads should stay in low levels
         * until we get more aggressive.
         */
        if (chunk->generation > max_level)
            break;

        /*
         * If we have enough chunks for a merge and the next chunk is in too high a generation,
         * stop.
         */
        if (nchunks >= merge_min && chunk->generation > youngest_gen + max_gap)
            break;

        /*
         * If the size of the chunks selected so far exceeds the configured maximum chunk size,
         * stop. Keep going if we can slide the window further into the tree: we don't want to leave
         * small chunks in the middle.
         */
        if ((chunk_size += chunk->size) > lsm_tree->chunk_max)
            if (nchunks < merge_min ||
              (chunk->generation > youngest->generation &&
                chunk_size - youngest->size > lsm_tree->chunk_max))
                break;

        /* Track chunk generations seen while looking for a merge */
        if (chunk->generation < youngest_gen)
            youngest_gen = chunk->generation;
        else if (chunk->generation > oldest_gen)
            oldest_gen = chunk->generation;

        if (oldest_gen - youngest_gen > max_gap)
            break;

        F_SET(chunk, WT_LSM_CHUNK_MERGING);
        record_count += chunk->count;
        --start_chunk;
        ++nchunks;

        /*
         * If the merge would be too big, or we have a full window and we could include an older
         * chunk if the window wasn't full, remove the youngest chunk.
         */
        if (chunk_size > lsm_tree->chunk_max ||
          (nchunks == merge_max && start_chunk > 0 &&
            chunk->generation == lsm_tree->chunk[start_chunk - 1]->generation)) {
            /*
             * Try again with smaller range. Unfortunately all the intermediate state will be reset.
             * Since there's no easy way to restore youngest_gen and oldest_gen.
             */
            __lsm_merge_clear(session, lsm_tree, start_chunk, nchunks);
            --end_chunk;
            goto retry_find;
        } else if (nchunks == merge_max)
            /* We've found the best full merge we can */
            break;
    }
    nchunks = (end_chunk + 1) - start_chunk;

    /* Be paranoid, check that we setup the merge properly. */
    WT_ASSERT(session, start_chunk + nchunks <= lsm_tree->nchunks);
#ifdef HAVE_DIAGNOSTIC
    for (i = 0; i < nchunks; i++) {
        chunk = lsm_tree->chunk[start_chunk + i];
        WT_ASSERT(session, F_ISSET(chunk, WT_LSM_CHUNK_MERGING));
    }
#endif

    WT_ASSERT(session, nchunks == 0 || (chunk != NULL && youngest != NULL));

    /*
     * Don't do merges that are too small or across too many generations.
     */
    if (nchunks < merge_min || oldest_gen - youngest_gen > max_gap) {
        __lsm_merge_clear(session, lsm_tree, start_chunk, nchunks);
        /*
         * If we didn't find a merge with appropriate gaps, try again with a smaller range.
         */
        if (end_chunk > lsm_tree->merge_min && oldest_gen - youngest_gen > max_gap) {
            --end_chunk;
            goto retry_find;
        }
        /* Consider getting aggressive if no merge was found */
        __lsm_merge_aggressive_update(session, lsm_tree);
        return (WT_NOTFOUND);
    }

    __lsm_merge_aggressive_clear(lsm_tree);
    *records = record_count;
    *start = start_chunk;
    *end = end_chunk;
    return (0);
}

/*
 * __wt_lsm_merge --
 *     Merge a set of chunks of an LSM tree.
 */
int
__wt_lsm_merge(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, u_int id)
{
    WT_BLOOM *bloom;
    WT_CURSOR *dest, *src;
    WT_DECL_RET;
    WT_ITEM key, value;
    WT_LSM_CHUNK *chunk;
    uint64_t insert_count, record_count;
    uint32_t generation;
    u_int dest_id, end_chunk, i, nchunks, start_chunk, start_id, verb;
    int tret;
    const char *cfg[3];
    const char *drop_cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_drop), "force", NULL};
    bool created_chunk, create_bloom, locked, in_sync;

    bloom = NULL;
    chunk = NULL;
    dest = src = NULL;
    created_chunk = create_bloom = in_sync = false;

    /* Fast path if it's obvious no merges could be done. */
    if (lsm_tree->nchunks < lsm_tree->merge_min &&
      lsm_tree->merge_aggressiveness < WT_LSM_AGGRESSIVE_THRESHOLD)
        return (WT_NOTFOUND);

    /*
     * Use the lsm_tree lock to read the chunks (so no switches occur), but avoid holding it while
     * the merge is in progress: that may take a long time.
     */
    __wt_lsm_tree_writelock(session, lsm_tree);
    locked = true;

    WT_ERR(__lsm_merge_span(session, lsm_tree, id, &start_chunk, &end_chunk, &record_count));
    nchunks = (end_chunk + 1) - start_chunk;

    WT_ASSERT(session, nchunks > 0);
    start_id = lsm_tree->chunk[start_chunk]->id;

    /* Find the merge generation. */
    for (generation = 0, i = 0; i < nchunks; i++)
        generation = WT_MAX(generation, lsm_tree->chunk[start_chunk + i]->generation + 1);

    __wt_lsm_tree_writeunlock(session, lsm_tree);
    locked = false;

    /* Allocate an ID for the merge. */
    dest_id = __wt_atomic_add32(&lsm_tree->last, 1);

    /*
     * We only want to do the chunk loop if we're running with verbose, so we wrap these statements
     * in the conditional. Avoid the loop in the normal path.
     */
    if (WT_VERBOSE_ISSET(session, WT_VERB_LSM)) {
        __wt_verbose(session, WT_VERB_LSM,
          "Merging %s chunks %u-%u into %u (%" PRIu64 " records), generation %" PRIu32,
          lsm_tree->name, start_chunk, end_chunk, dest_id, record_count, generation);
        for (verb = start_chunk; verb < end_chunk + 1; verb++)
            __wt_verbose(session, WT_VERB_LSM,
              "Merging %s: Chunk[%u] id %" PRIu32 ", gen: %" PRIu32 ", size: %" PRIu64
              ", records: %" PRIu64,
              lsm_tree->name, verb, lsm_tree->chunk[verb]->id, lsm_tree->chunk[verb]->generation,
              lsm_tree->chunk[verb]->size, lsm_tree->chunk[verb]->count);
    }

    WT_ERR(__wt_calloc_one(session, &chunk));
    created_chunk = true;
    chunk->id = dest_id;
    chunk->generation = generation;

    if (FLD_ISSET(lsm_tree->bloom, WT_LSM_BLOOM_MERGED) &&
      (FLD_ISSET(lsm_tree->bloom, WT_LSM_BLOOM_OLDEST) || start_chunk > 0) && record_count > 0)
        create_bloom = true;

    /*
     * Special setup for the merge cursor: first, reset to open the dependent cursors; then restrict
     * the cursor to a specific number of chunks; then set MERGE so the cursor doesn't track updates
     * to the tree.
     */
    WT_ERR(__wt_open_cursor(session, lsm_tree->name, NULL, NULL, &src));
    F_SET(src, WT_CURSTD_RAW);
    WT_ERR(__wt_clsm_init_merge(src, start_chunk, start_id, nchunks));

    WT_WITH_SCHEMA_LOCK(session, ret = __wt_lsm_tree_setup_chunk(session, lsm_tree, chunk));
    WT_ERR(ret);
    if (create_bloom) {
        WT_ERR(__wt_lsm_tree_setup_bloom(session, lsm_tree, chunk));

        WT_ERR(__wt_bloom_create(session, chunk->bloom_uri, lsm_tree->bloom_config, record_count,
          lsm_tree->bloom_bit_count, lsm_tree->bloom_hash_count, &bloom));
    }

    /* Discard pages we read as soon as we're done with them. */
    F_SET(session, WT_SESSION_READ_WONT_NEED);

    cfg[0] = WT_CONFIG_BASE(session, WT_SESSION_open_cursor);
    cfg[1] = "bulk,raw,skip_sort_check";
    cfg[2] = NULL;
    WT_ERR(__wt_open_cursor(session, chunk->uri, NULL, cfg, &dest));

    if (lsm_tree->custom_generation != 0 && chunk->generation >= lsm_tree->custom_generation) {
        WT_DATA_SOURCE *dsrc = __wt_schema_get_source(session, chunk->uri);

        if (dsrc != NULL && dsrc->lsm_pre_merge != NULL) {
            /* Call the callback. */
            WT_ERR(dsrc->lsm_pre_merge(dsrc, src, dest));

            /* Make sure the source is ready to start the scan. */
            WT_ERR(src->reset(src));
        }
    }

#define LSM_MERGE_CHECK_INTERVAL WT_THOUSAND
    for (insert_count = 0; (ret = src->next(src)) == 0; insert_count++) {
        if (insert_count % LSM_MERGE_CHECK_INTERVAL == 0) {
            if (!lsm_tree->active)
                WT_ERR(EINTR);

            WT_STAT_CONN_INCRV(session, lsm_rows_merged, LSM_MERGE_CHECK_INTERVAL);
            ++lsm_tree->merge_progressing;
        }

        WT_ERR(src->get_key(src, &key));
        dest->set_key(dest, &key);
        WT_ERR(src->get_value(src, &value));
        dest->set_value(dest, &value);
        WT_ERR(dest->insert(dest));
        if (create_bloom)
            __wt_bloom_insert(bloom, &key);
    }
    WT_ERR_NOTFOUND_OK(ret, false);

    WT_STAT_CONN_INCRV(session, lsm_rows_merged, insert_count % LSM_MERGE_CHECK_INTERVAL);
    ++lsm_tree->merge_progressing;
    __wt_verbose(session, WT_VERB_LSM, "Bloom size for %" PRIu64 " has %" PRIu64 " items inserted",
      record_count, insert_count);

    /*
     * Closing and syncing the files can take a while. Set the merge_syncing field so that compact
     * knows it is still in progress.
     */
    (void)__wt_atomic_add32(&lsm_tree->merge_syncing, 1);
    in_sync = true;
    /*
     * We've successfully created the new chunk. Now install it. We need to ensure that the NO_CACHE
     * flag is cleared and the bloom filter is closed (even if a step fails), so track errors but
     * don't return until we've cleaned up.
     */
    WT_TRET(src->close(src));
    WT_TRET(dest->close(dest));
    src = dest = NULL;

    F_CLR(session, WT_SESSION_READ_WONT_NEED);

    /*
     * We're doing advisory reads to fault the new trees into cache. Don't block if the cache is
     * full: our next unit of work may be to discard some trees to free space.
     */
    F_SET(session, WT_SESSION_IGNORE_CACHE_SIZE);

    if (create_bloom) {
        if (ret == 0)
            WT_TRET(__wt_bloom_finalize(bloom));

        /*
         * Read in a key to make sure the Bloom filters btree handle is open before it becomes
         * visible to application threads. Otherwise application threads will stall while it is
         * opened and internal pages are read into cache.
         */
        if (ret == 0) {
            WT_CLEAR(key);
            WT_TRET_NOTFOUND_OK(__wt_bloom_get(bloom, &key));
        }

        WT_TRET(__wt_bloom_close(bloom));
        bloom = NULL;
    }
    WT_ERR(ret);

    /*
     * Open a handle on the new chunk before application threads attempt to access it, opening it
     * pre-loads internal pages into the file system cache.
     */
    cfg[1] = "checkpoint=" WT_CHECKPOINT;
    WT_ERR(__wt_open_cursor(session, chunk->uri, NULL, cfg, &dest));
    WT_TRET(dest->close(dest));
    dest = NULL;
    ++lsm_tree->merge_progressing;
    (void)__wt_atomic_sub32(&lsm_tree->merge_syncing, 1);
    in_sync = false;
    WT_ERR_NOTFOUND_OK(ret, false);

    WT_ERR(__wt_lsm_tree_set_chunk_size(session, lsm_tree, chunk));
    __wt_lsm_tree_writelock(session, lsm_tree);
    locked = true;

    /*
     * Check whether we raced with another merge, and adjust the chunk array offset as necessary.
     */
    if (start_chunk >= lsm_tree->nchunks || lsm_tree->chunk[start_chunk]->id != start_id)
        for (start_chunk = 0; start_chunk < lsm_tree->nchunks; start_chunk++)
            if (lsm_tree->chunk[start_chunk]->id == start_id)
                break;

    /*
     * It is safe to error out here - since the update can only fail prior to making updates to the
     * tree.
     */
    WT_ERR(__wt_lsm_merge_update_tree(session, lsm_tree, start_chunk, nchunks, chunk));

    if (create_bloom)
        F_SET(chunk, WT_LSM_CHUNK_BLOOM);
    chunk->count = insert_count;
    F_SET(chunk, WT_LSM_CHUNK_ONDISK);

    /*
     * We have no current way of continuing if the metadata update fails, so we will panic in that
     * case. Put some effort into cleaning up after ourselves here - so things have a chance of
     * shutting down.
     *
     * Any errors that happened after the tree was locked are fatal - we can't guarantee the state
     * of the tree.
     */
    if ((ret = __wt_lsm_meta_write(session, lsm_tree, NULL)) != 0)
        WT_ERR_PANIC(session, ret, "Failed finalizing LSM merge");

    lsm_tree->dsk_gen++;

    /* Update the throttling while holding the tree lock. */
    __wt_lsm_tree_throttle(session, lsm_tree, true);

    /* Schedule a pass to discard old chunks */
    WT_ERR(__wt_lsm_manager_push_entry(session, WT_LSM_WORK_DROP, 0, lsm_tree));

err:
    if (locked)
        __wt_lsm_tree_writeunlock(session, lsm_tree);
    if (in_sync)
        (void)__wt_atomic_sub32(&lsm_tree->merge_syncing, 1);
    if (src != NULL)
        WT_TRET(src->close(src));
    if (dest != NULL)
        WT_TRET(dest->close(dest));
    if (bloom != NULL)
        WT_TRET(__wt_bloom_close(bloom));
    if (ret != 0 && created_chunk) {
        /* Drop the newly-created files on error. */
        if (chunk->uri != NULL) {
            WT_WITH_SCHEMA_LOCK(session, tret = __wt_schema_drop(session, chunk->uri, drop_cfg));
            WT_TRET(tret);
        }
        if (create_bloom && chunk->bloom_uri != NULL) {
            WT_WITH_SCHEMA_LOCK(
              session, tret = __wt_schema_drop(session, chunk->bloom_uri, drop_cfg));
            WT_TRET(tret);
        }
        __wt_free(session, chunk->bloom_uri);
        __wt_free(session, chunk->uri);
        __wt_free(session, chunk);

        if (ret == EINTR)
            __wt_verbose(session, WT_VERB_LSM, "%s", "Merge aborted due to close");
        else
            __wt_verbose(
              session, WT_VERB_LSM, "Merge failed with %s", __wt_strerror(session, ret, NULL, 0));
    }
    F_CLR(session, WT_SESSION_IGNORE_CACHE_SIZE | WT_SESSION_READ_WONT_NEED);
    return (ret);
}
