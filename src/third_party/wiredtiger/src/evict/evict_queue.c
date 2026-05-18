/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * LRU eviction queue management: maintaining the two rotating LRU queues and the urgent queue,
 * sorting candidates by eviction score, and removing pages that can no longer be evicted.
 *
 * The eviction subsystem keeps two ordinary queues that alternate roles: one is being drained by
 * eviction workers (__wti_evict_lru_pages) while the other is being refilled by the server via
 * __wti_evict_lru_walk. __wti_evict_lru_walk drives the eviction walk to populate the fill queue,
 * then sorts it by score using __evict_lru_cmp and sets evict_candidates to control how many
 * entries workers will attempt. __wti_evict_queue_clear_page and its locked variant search all
 * queues and clear a specific page when it transitions out of the WT_PAGE_EVICT_LRU state (e.g.
 * because it was freed or reconciled by another path before the eviction worker reached it).
 */
#include "wt_internal.h"

static int WT_CDECL __evict_lru_cmp(const void *, const void *);

/*
 * __evict_lru_cmp_debug --
 *     Qsort function: sort the eviction array. Version for eviction debug mode.
 */
static int WT_CDECL
__evict_lru_cmp_debug(const void *a_arg, const void *b_arg)
{
    const WTI_EVICT_ENTRY *a, *b;
    uint64_t a_score, b_score;

    a = a_arg;
    b = b_arg;
    a_score = (a->ref == NULL ? UINT64_MAX : 0);
    b_score = (b->ref == NULL ? UINT64_MAX : 0);

    return ((a_score < b_score) ? -1 : (a_score == b_score) ? 0 : 1);
}

/*
 * __evict_lru_cmp --
 *     Qsort function: sort the eviction array.
 */
static int WT_CDECL
__evict_lru_cmp(const void *a_arg, const void *b_arg)
{
    const WTI_EVICT_ENTRY *a, *b;
    uint64_t a_score, b_score;

    a = a_arg;
    b = b_arg;
    a_score = (a->ref == NULL ? UINT64_MAX : a->score);
    b_score = (b->ref == NULL ? UINT64_MAX : b->score);

    return ((a_score < b_score) ? -1 : (a_score == b_score) ? 0 : 1);
}

/*
 * __wti_evict_queue_clear_page --
 *     Check whether a page is present in the LRU eviction list. If the page is found in the list,
 *     remove it. This is called from the page eviction code to make sure there is no attempt to
 *     evict a child page multiple times.
 */
void
__wti_evict_queue_clear_page(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_EVICT *evict;

    WT_ASSERT(session, __wt_ref_is_root(ref) || WT_REF_GET_STATE(ref) == WT_REF_LOCKED);

    /* Fast path: if the page isn't in the queue, don't bother searching. */
    if (!F_ISSET_ATOMIC_16(ref->page, WT_PAGE_EVICT_LRU))
        return;
    evict = S2C(session)->evict;

    __wt_spin_lock(session, &evict->evict_queue_lock);

    /* Remove the reference from the eviction queues. */
    __wti_evict_queue_clear_page_locked(session, ref, false);

    __wt_spin_unlock(session, &evict->evict_queue_lock);
}

/*
 * __wti_evict_queue_clear_page_locked --
 *     This function searches for the page in all the eviction queues (skipping the urgent queue if
 *     requested) and clears it if found. It does not take the eviction queue lock, so the caller
 *     should hold the appropriate locks before calling this function.
 */
void
__wti_evict_queue_clear_page_locked(WT_SESSION_IMPL *session, WT_REF *ref, bool exclude_urgent)
{
    WT_EVICT *evict;
    WTI_EVICT_ENTRY *evict_entry;
    uint32_t elem, i, q, last_queue_idx;
    bool found;

    last_queue_idx = exclude_urgent ? WTI_EVICT_URGENT_QUEUE : WTI_EVICT_QUEUE_MAX;
    evict = S2C(session)->evict;
    found = false;

    WT_ASSERT_SPINLOCK_OWNED(session, &evict->evict_queue_lock);

    for (q = 0; q < last_queue_idx && !found; q++) {
        __wt_spin_lock(session, &evict->evict_queues[q].evict_lock);
        elem = evict->evict_queues[q].evict_max;
        for (i = 0, evict_entry = evict->evict_queues[q].evict_queue; i < elem; i++, evict_entry++)
            if (evict_entry->ref == ref) {
                found = true;
                __evict_list_clear(session, evict_entry);
                break;
            }
        __wt_spin_unlock(session, &evict->evict_queues[q].evict_lock);
    }
    WT_ASSERT(session, !F_ISSET_ATOMIC_16(ref->page, WT_PAGE_EVICT_LRU));
}

/*
 * __wti_evict_lru_pages --
 *     Get pages from the LRU queue to evict.
 */
int
__wti_evict_lru_pages(WT_SESSION_IMPL *session, bool is_server)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_TRACK_OP_DECL;

    WT_TRACK_OP_INIT(session);
    conn = S2C(session);

    /*
     * Reconcile and discard some pages: EBUSY is returned if a page fails eviction because it's
     * unavailable, continue in that case.
     */
    while (FLD_ISSET(conn->server_flags, WT_CONN_SERVER_EVICTION) && ret == 0)
        if ((ret = __wti_evict_page(session, is_server)) == EBUSY)
            ret = 0;

    /* If any resources are pinned, release them now. */
    WT_TRET(__wt_session_release_resources(session));

    /* If a worker thread found the queue empty, pause. */
    if (ret == WT_NOTFOUND && !is_server && FLD_ISSET(conn->server_flags, WT_CONN_SERVER_EVICTION))
        __wt_cond_wait(session, conn->evict_config.threads.wait_cond, 10 * WT_THOUSAND, NULL);

    WT_TRACK_OP_END(session);
    return (ret == WT_NOTFOUND ? 0 : ret);
}

/*
 * __wti_evict_lru_walk --
 *     Add pages to the LRU queue to be evicted from cache.
 */
int
__wti_evict_lru_walk(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_EVICT *evict;
    WTI_EVICT_QUEUE *other_queue, *queue;
    WT_TRACK_OP_DECL;
    uint64_t read_gen_oldest;
    uint32_t candidates, entries;

    WT_TRACK_OP_INIT(session);
    conn = S2C(session);
    evict = conn->evict;

    /* Age out the score of how much the queue has been empty recently. */
    if (evict->evict_empty_score > 0)
        --evict->evict_empty_score;

    /* Fill the next queue (that isn't the urgent queue). */
    queue = evict->evict_fill_queue;
    other_queue = evict->evict_queues + (1 - (queue - evict->evict_queues));
    evict->evict_fill_queue = other_queue;

    /* If this queue is full, try the other one. */
    if (__evict_queue_full(queue) && !__evict_queue_full(other_queue))
        queue = other_queue;

    /* If both queues are full and haven't been empty on recent refills, we're done. */
    if (__evict_queue_full(queue) && evict->evict_empty_score < WT_EVICT_SCORE_CUTOFF) {
        WT_STAT_CONN_INCR(session, eviction_queue_not_empty);
        goto err;
    }
    /*
     * If the queue we are filling is empty, pages are being requested faster than they are being
     * queued.
     */
    if (__evict_queue_empty(queue, false)) {
        if (F_ISSET(evict, WT_EVICT_CACHE_HARD))
            evict->evict_empty_score =
              WT_MIN(evict->evict_empty_score + WT_EVICT_SCORE_BUMP, WT_EVICT_SCORE_MAX);
        WT_STAT_CONN_INCR(session, eviction_queue_empty);
    } else {
        WT_STAT_CONN_INCR(session, eviction_queue_not_empty);
        WT_STAT_CONN_INCRV(session, eviction_pages_remaining_in_queue, queue->evict_candidates);
    }

    /*
     * Get some more pages to consider for eviction.
     *
     * If the walk is interrupted, we still need to sort the queue: the next walk assumes there are
     * no entries beyond WTI_EVICT_WALK_BASE.
     */
    if ((ret = __wti_evict_walk(evict->walk_session, queue)) == EBUSY)
        ret = 0;
    WT_ERR_NOTFOUND_OK(ret, false);

    /* Sort the list into LRU order and restart. */
    __wt_spin_lock(session, &queue->evict_lock);

    /*
     * We have locked the queue: in the (unusual) case where we are filling the current queue, mark
     * it empty so that subsequent requests switch to the other queue.
     */
    if (queue == evict->evict_current_queue)
        queue->evict_current = NULL;

    entries = queue->evict_entries;
    /*
     * Style note: __wt_qsort is a macro that can leave a dangling else. Full curly braces are
     * needed here for the compiler.
     */
    if (FLD_ISSET(conn->debug_flags, WT_CONN_DEBUG_EVICT_AGGRESSIVE_MODE)) {
        __wt_qsort(queue->evict_queue, entries, sizeof(WTI_EVICT_ENTRY), __evict_lru_cmp_debug);
    } else {
        __wt_qsort(queue->evict_queue, entries, sizeof(WTI_EVICT_ENTRY), __evict_lru_cmp);
    }

    /* Trim empty entries from the end. */
    while (entries > 0 && queue->evict_queue[entries - 1].ref == NULL)
        --entries;

    /*
     * If we have more entries than the maximum tracked between walks, clear them. Do this before
     * figuring out how many of the entries are candidates so we never end up with more candidates
     * than entries.
     */
    while (entries > WTI_EVICT_WALK_BASE)
        __evict_list_clear(session, &queue->evict_queue[--entries]);

    queue->evict_entries = entries;

    if (entries == 0) {
        /*
         * If there are no entries, there cannot be any candidates. Make sure application threads
         * don't read past the end of the candidate list, or they may race with the next walk.
         */
        queue->evict_candidates = 0;
        queue->evict_current = NULL;
        __wt_spin_unlock(session, &queue->evict_lock);
        goto err;
    }

    /* Decide how many of the candidates we're going to try and evict. */
    if (__wt_evict_aggressive(session))
        queue->evict_candidates = entries;
    else {
        /*
         * Find the oldest read generation apart that we have in the queue, used to set the initial
         * value for pages read into the system. The queue is sorted, find the first "normal"
         * generation.
         */
        read_gen_oldest = WT_READGEN_START_VALUE;
        for (candidates = 0; candidates < entries; ++candidates) {
            WT_READ_ONCE(read_gen_oldest, queue->evict_queue[candidates].score);
            if (!__wti_evict_readgen_is_soon_or_wont_need(&read_gen_oldest))
                break;
        }

        /*
         * Take all candidates if we only gathered pages with an oldest
         * read generation set.
         *
         * We normally never take more than 50% of the entries but if
         * 50% of the entries were at the oldest read generation, take
         * all of them.
         */
        if (__wti_evict_readgen_is_soon_or_wont_need(&read_gen_oldest))
            queue->evict_candidates = entries;
        else if (candidates > entries / 2)
            queue->evict_candidates = candidates;
        else {
            /*
             * Take all of the urgent pages plus a third of ordinary candidates (which could be
             * expressed as WTI_EVICT_WALK_INCR / WTI_EVICT_WALK_BASE). In the steady state, we want
             * to get as many candidates as the eviction walk adds to the queue.
             *
             * That said, if there is only one entry, which is normal when populating an empty file,
             * don't exclude it.
             */
            queue->evict_candidates = 1 + candidates + ((entries - candidates) - 1) / 3;
            if (queue->evict_candidates > entries / 2)
                queue->evict_candidates = entries / 2;

            evict->read_gen_oldest = read_gen_oldest;
        }
    }

    WT_STAT_CONN_INCRV(session, eviction_pages_queued_post_lru, queue->evict_candidates);

    /* Add stats about pages that have been queued. */
    for (candidates = 0; candidates < queue->evict_candidates; ++candidates) {
        WT_PAGE *page = queue->evict_queue[candidates].ref->page;
        if (__wt_page_is_modified(page))
            WT_STAT_CONN_DSRC_INCR(session, cache_eviction_pages_queued_dirty);
        else
            WT_STAT_CONN_DSRC_INCR(session, cache_eviction_pages_queued_clean);

        if (__evict_page_updates_candidate(page))
            WT_STAT_CONN_DSRC_INCR(session, cache_eviction_pages_queued_updates);
    }
    queue->evict_current = queue->evict_queue;
    __wt_spin_unlock(session, &queue->evict_lock);

    /* Signal any application or helper threads that may be waiting to help with eviction. */
    __wt_cond_signal(session, conn->evict_config.threads.wait_cond);

err:
    WT_TRACK_OP_END(session);
    return (ret);
}
