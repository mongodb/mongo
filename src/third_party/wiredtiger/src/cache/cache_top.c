/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * Rank the tables holding the most cache, without walking the cache or the dhandle list.
 *
 * Five rankings, each a fixed array of slots behind one lock, plus a threshold. Update, dirty leaf
 * and resident bytes are read from a tree's counters and sum to at most the cache size, so a
 * threshold of cache size / slot count caps how many trees can qualify: those arrays are complete
 * rather than a sample, and a tree missing from one is provably below the threshold. Bytes read and
 * evicted accumulate and decay, which bounds them by rate rather than by cache size, so those two
 * arrays hold the largest consumers seen rather than a provably complete set. The threshold starts
 * from the average tree size, which is finer than cache size / slot count when trees outnumber
 * slots, and is adjusted at every report.
 *
 * Each tree records, per metric, the value it must exceed to be worth another look and the slot it
 * occupies. Below threshold or already tracked therefore costs neither the lock nor a scan; only
 * admitting a new tree scans, to find the entry to replace.
 *
 * Nothing removes a tree when its value drops, because both admission and reporting re-read live
 * values before acting on them.
 *
 * Limitation: a tree never evicted from, that never grows past its recheck value, is never
 * reconsidered and can go unseen. Cache pressure implies eviction, which triggers reconsideration,
 * so this does not affect the case this exists for.
 */

/*
 * One line of a report, filled in under the array lock and printed after it is dropped. The name is
 * copied because the tree it came from may be gone by the time the line is printed.
 */
struct __wt_cache_top_report_entry {
    char *name; /* Freed by the caller; NULL when the slot is unused. */
    uint64_t value;
};
typedef struct __wt_cache_top_report_entry WT_CACHE_TOP_REPORT_ENTRY;

/*
 * __cache_top_entries_free --
 *     Free every name a report array owns, leaving it ready to reuse for the next ranking.
 */
static void
__cache_top_entries_free(WT_SESSION_IMPL *session, WT_CACHE_TOP_REPORT_ENTRY *entries)
{
    uint32_t i;

    for (i = 0; i < WT_CACHE_TOP_SLOTS; ++i)
        __wt_free(session, entries[i].name);
}

/*
 * __cache_top_threshold --
 *     Return the value a tree's metric must reach before the tree is added to this ranking,
 *     computing it the first time it is needed for this ranking.
 */
static uint64_t
__cache_top_threshold(WT_SESSION_IMPL *session, WT_CACHE_TOP_ARRAY *array)
{
    WT_CONNECTION_IMPL *conn;
    uint64_t threshold;
    uint32_t trees;

    conn = S2C(session);

    threshold = __wt_atomic_load_uint64_relaxed(&array->threshold);
    if (threshold != 0)
        return (threshold);

    /*
     * Cache size / slot count is the highest threshold that still caps how many trees can qualify,
     * but it is far above the largest tree when trees outnumber slots, so start from the average
     * resident size. Err low as too low fills the array and corrects in one step, while too high
     * leaves it empty and only approximates down a factor per report.
     */
    trees = __wt_atomic_load_uint32_relaxed(&conn->open_btree_count);
    threshold = conn->cache_size / WT_CACHE_TOP_SLOTS;
    if (trees > WT_CACHE_TOP_SLOTS)
        threshold = WT_MIN(threshold, __wt_cache_bytes_inuse(conn->cache) / trees);
    threshold = WT_MAX(threshold, WT_CACHE_TOP_THRESHOLD_FLOOR);

    /* Some callers act only on an already-set threshold, so the first to compute one must save it.
     */
    __wt_atomic_store_uint64_relaxed(&array->threshold, threshold);

    return (threshold);
}

/*
 * __cache_top_flow_storage --
 *     Return pointers to where a tree keeps its decayed byte count and the clock that count was
 *     last decayed against, for a caller that needs to update both. The count is stale until it is
 *     decayed against the current clock. Only bytes read and bytes evicted are stored this way; any
 *     other metric is a caller error.
 */
static WT_INLINE void
__cache_top_flow_storage(WT_SESSION_IMPL *session, WT_BTREE *btree, WT_CACHE_TOP_METRIC metric,
  uint64_t **valuep, uint64_t **clockp)
{
    /* Set here rather than in a default label, so the switch below can stay free of one. */
    *valuep = *clockp = NULL;

    switch (metric) {
    case WT_CACHE_TOP_READ:
        *valuep = &btree->bytes_read_decayed;
        *clockp = &btree->bytes_read_decay_clock;
        break;
    case WT_CACHE_TOP_EVICT:
        *valuep = &btree->bytes_evict_decayed;
        *clockp = &btree->bytes_evict_decay_clock;
        break;
    case WT_CACHE_TOP_UPDATES:
    case WT_CACHE_TOP_DIRTY:
    case WT_CACHE_TOP_INMEM:
        WT_ASSERT_ALWAYS(session, false, "cache top: %d does not track a flow", (int)metric);
        break;
    }
}

/*
 * __cache_top_decay --
 *     Apply time decay to a flow value, so that old activity fades instead of accumulating without
 *     limit.
 *
 * Decay advances in whole half-lives, so up to one half-life of elapsed time goes unaccounted for.
 *     A caller storing the result back must save the returned clock, not the one it passed in;
 *     otherwise a tree touched more often than once per half-life never decays at all. A caller
 *     only reading the value passes NULL, leaving the recorded time untouched.
 */
static WT_INLINE uint64_t
__cache_top_decay(
  WT_SESSION_IMPL *session, uint64_t value, uint64_t clock, uint64_t now, uint64_t *newclockp)
{
    uint64_t halflife, halvings;

    if (newclockp != NULL)
        *newclockp = clock;

    /*
     * A value that has already decayed away has no window left to account for, so start a new one
     * at now. Leaving the old clock in place would decay the caller's increment against all the
     * time the tree spent idle, and a tree resuming activity would never climb off zero.
     */
    if (value == 0) {
        if (newclockp != NULL)
            *newclockp = now;
        return (value);
    }

    if (clock == 0 || now <= clock)
        return (value);

    halflife = S2C(session)->cache->cache_top.halflife_ticks;
    halvings = (now - clock) / halflife;
    if (halvings == 0)
        return (value);

    if (newclockp != NULL)
        *newclockp = clock + halvings * halflife;

    return (halvings >= WT_CACHE_TOP_DECAY_MAX_HALVINGS ? 0 : value >> halvings);
}

/*
 * __cache_top_flow_value --
 *     Return a flow ranking's current value, decayed up to now.
 */
static uint64_t
__cache_top_flow_value(WT_SESSION_IMPL *session, WT_BTREE *btree, WT_CACHE_TOP_METRIC metric)
{
    uint64_t *clockp, *valuep;

    __cache_top_flow_storage(session, btree, metric, &valuep, &clockp);
    return (__cache_top_decay(session, __wt_atomic_load_uint64_relaxed(valuep),
      __wt_atomic_load_uint64_relaxed(clockp), __wt_clock(session), NULL));
}

/*
 * __cache_top_value --
 *     Return a tree's current value for a ranking.
 */
static uint64_t
__cache_top_value(WT_SESSION_IMPL *session, WT_BTREE *btree, WT_CACHE_TOP_METRIC metric)
{
    switch (metric) {
    case WT_CACHE_TOP_UPDATES:
        return (__wt_atomic_load_uint64_relaxed(&btree->bytes_updates));
    case WT_CACHE_TOP_DIRTY:
        return (__wt_atomic_load_uint64_relaxed(&btree->bytes_dirty_leaf));
    case WT_CACHE_TOP_INMEM:
        return (__wt_atomic_load_uint64_relaxed(&btree->bytes_inmem));
    case WT_CACHE_TOP_READ:
    case WT_CACHE_TOP_EVICT:
        return (__cache_top_flow_value(session, btree, metric));
    }

    return (0);
}

/*
 * __cache_top_recheck_at_set --
 *     Set the value a tree's metric must reach to be reconsidered for a ranking: whichever is
 *     higher of the threshold and one growth step above the tree's current value. The step is what
 *     makes a busy tree call back in once per step of growth rather than on every change. A race
 *     here costs one extra visit, so the store needs no ordering.
 */
static void
__cache_top_recheck_at_set(
  WT_BTREE *btree, WT_CACHE_TOP_METRIC metric, uint64_t threshold, uint64_t value)
{
    uint64_t spacing;

    spacing = WT_MAX(threshold / WT_CACHE_TOP_RECHECK_DIVISOR, WT_CACHE_TOP_RECHECK_MIN_SPACING);
    __wt_atomic_store_uint64_relaxed(
      &btree->cache_top_recheck_at[metric], WT_MAX(threshold, value + spacing));
}

/*
 * __cache_top_smallest --
 *     Return an unused slot if one exists, otherwise the slot holding the smallest value. Refreshes
 *     every slot's value on the way, being the one place that looks at all of them.
 */
static uint32_t
__cache_top_smallest(
  WT_SESSION_IMPL *session, WT_CACHE_TOP_ARRAY *array, WT_CACHE_TOP_METRIC metric)
{
    uint32_t i, smallest;

    smallest = 0;
    for (i = 0; i < WT_CACHE_TOP_SLOTS; ++i) {
        if (array->slots[i].btree == NULL)
            return (i);
        array->slots[i].value = __cache_top_value(session, array->slots[i].btree, metric);
        if (array->slots[i].value < array->slots[smallest].value)
            smallest = i;
    }

    return (smallest);
}

/*
 * __wt_cache_top_track --
 *     Decide whether a tree belongs in a ranking now. Called from the accounting path only once a
 *     tree's counter reaches its recheck value, so the common case costs the caller a comparison
 *     rather than a call.
 */
void
__wt_cache_top_track(
  WT_SESSION_IMPL *session, WT_BTREE *btree, WT_CACHE_TOP_METRIC metric, uint64_t value)
{
    WT_CACHE_TOP_ARRAY *array;
    uint64_t threshold;
    uint32_t slot;

    array = &S2C(session)->cache->cache_top.arrays[metric];
    threshold = __cache_top_threshold(session, array);

    /*
     * Most visits are a tree that is still below the threshold; it has nothing to add to the
     * ranking, and the recheck value it needs depends only on its own size. Handle that case
     * without touching the array lock at all.
     */
    if (value < threshold) {
        __cache_top_recheck_at_set(btree, metric, threshold, value);
        return;
    }

    __wt_spin_lock(session, &array->lock);

    /*
     * A tree remembers which slot it occupies, so refreshing an already-tracked tree is a direct
     * update, not a search: this field is only ever written under this same lock, so once we hold
     * the lock, the value we read here is guaranteed current, not merely a hint.
     */
    slot = __wt_atomic_load_uint8_relaxed(&btree->cache_top_slot[metric]);
    if (slot < WT_CACHE_TOP_SLOTS) {
        WT_ASSERT(session, array->slots[slot].btree == btree);
        array->slots[slot].value = value;
        goto done;
    }

    slot = __cache_top_smallest(session, array, metric);
    if (array->slots[slot].btree != NULL && array->slots[slot].value >= value)
        goto done;

    if (array->slots[slot].btree != NULL)
        __wt_atomic_store_uint8_relaxed(
          &array->slots[slot].btree->cache_top_slot[metric], WT_CACHE_TOP_NOT_TRACKED);
    array->slots[slot].btree = btree;
    array->slots[slot].value = value;
    __wt_atomic_store_uint8_relaxed(&btree->cache_top_slot[metric], (uint8_t)slot);

done:
    __cache_top_recheck_at_set(btree, metric, threshold, value);

    __wt_spin_unlock(session, &array->lock);
}

/*
 * __cache_top_recheck_refresh --
 *     Bring a tree's recheck value down to the threshold if the threshold has fallen below it.
 *     Growth alone would never reconsider a tree whose recheck value was set while the threshold
 *     was higher, so without this the tree stays out of the ranking however well it now qualifies.
 */
static WT_INLINE void
__cache_top_recheck_refresh(WT_SESSION_IMPL *session, WT_BTREE *btree, WT_CACHE_TOP_METRIC metric)
{
    WT_CACHE_TOP *top;
    uint64_t recheck_at, threshold;

    /* A tree already in a slot is visible; checking again would only cost a lock. */
    if (__wt_atomic_load_uint8_relaxed(&btree->cache_top_slot[metric]) < WT_CACHE_TOP_SLOTS)
        return;

    /* The maximum value means this tree is permanently excluded, not merely overdue. */
    recheck_at = __wt_atomic_load_uint64_relaxed(&btree->cache_top_recheck_at[metric]);
    if (recheck_at == UINT64_MAX)
        return;

    top = &S2C(session)->cache->cache_top;
    threshold = __wt_atomic_load_uint64_relaxed(&top->arrays[metric].threshold);
    if (threshold != 0 && recheck_at > threshold)
        __wt_atomic_store_uint64_relaxed(&btree->cache_top_recheck_at[metric], threshold);
}

/*
 * __cache_top_levels_refresh --
 *     Reconsider a tree for the rankings read from its counters, which a tree can hold a large
 *     value in while completely idle. Eviction on the tree is the trigger, since it proves the tree
 *     is still resident.
 */
static WT_INLINE void
__cache_top_levels_refresh(WT_SESSION_IMPL *session, WT_BTREE *btree)
{
    static const WT_CACHE_TOP_METRIC levels[] = {
      WT_CACHE_TOP_UPDATES, WT_CACHE_TOP_DIRTY, WT_CACHE_TOP_INMEM};
    u_int i;

    for (i = 0; i < WT_ELEMENTS(levels); ++i)
        __cache_top_recheck_refresh(session, btree, levels[i]);
}

/*
 * __wt_cache_top_flow_incr --
 *     Record that a tree just read or evicted some number of bytes, and check whether that changes
 *     its standing in the corresponding ranking.
 */
void
__wt_cache_top_flow_incr(
  WT_SESSION_IMPL *session, WT_BTREE *btree, WT_CACHE_TOP_METRIC metric, size_t size)
{
    uint64_t clock, now, value, *clockp, *valuep;

    /*
     * An excluded tree has its recheck value pinned at the maximum. Check that before anything
     * else.
     */
    if (btree == NULL ||
      __wt_atomic_load_uint64_relaxed(&btree->cache_top_recheck_at[metric]) == UINT64_MAX)
        return;

    __cache_top_flow_storage(session, btree, metric, &valuep, &clockp);

    /*
     * Concurrent callers on the same tree can lose an increment here. The rankings are a diagnostic
     * ordering, not accounting that has to balance.
     */
    now = __wt_clock(session);
    clock = __wt_atomic_load_uint64_relaxed(clockp);
    value =
      __cache_top_decay(session, __wt_atomic_load_uint64_relaxed(valuep), clock, now, &clock) +
      size;
    __wt_atomic_store_uint64_relaxed(valuep, value);
    __wt_atomic_store_uint64_relaxed(clockp, clock == 0 ? now : clock);

    /*
     * Lower the recheck value before testing against it. Decay caps how high a flow value can get,
     * so a recheck value above that cap left behind by a threshold that has fallen would never be
     * reached.
     */
    __cache_top_recheck_refresh(session, btree, metric);

    if (value >= __wt_atomic_load_uint64_relaxed(&btree->cache_top_recheck_at[metric]))
        __wt_cache_top_track(session, btree, metric, value);

    if (metric == WT_CACHE_TOP_EVICT)
        __cache_top_levels_refresh(session, btree);
}

/*
 * __wt_cache_top_btree_open --
 *     Set up a tree's ranking state at open. Slot 0 is valid, so a tree must be marked as outside
 *     every ranking explicitly. Identity comes from the URI because the history store and
 *     disaggregated metadata flags are not set yet.
 */
void
__wt_cache_top_btree_open(WT_SESSION_IMPL *session, WT_BTREE *btree)
{
    u_int metric;
    bool excluded;

    WT_UNUSED(session);
    excluded = WT_IS_URI_METADATA(btree->dhandle->name) || WT_IS_URI_HS(btree->dhandle->name);

    for (metric = 0; metric < WT_CACHE_TOP_METRICS; ++metric) {
        btree->cache_top_slot[metric] = WT_CACHE_TOP_NOT_TRACKED;
        if (excluded)
            btree->cache_top_recheck_at[metric] = UINT64_MAX;
    }
}

/*
 * __wt_cache_top_btree_discard --
 *     Remove a tree from every ranking it may be part of. Must be called before the tree's memory
 *     is freed, since a ranking can otherwise be left holding a pointer to it.
 */
void
__wt_cache_top_btree_discard(WT_SESSION_IMPL *session, WT_BTREE *btree)
{
    WT_CACHE *cache;
    WT_CACHE_TOP_ARRAY *array;
    uint32_t i;
    u_int metric;

    if ((cache = S2C(session)->cache) == NULL)
        return;

    for (metric = 0; metric < WT_CACHE_TOP_METRICS; ++metric) {
        array = &cache->cache_top.arrays[metric];
        __wt_spin_lock(session, &array->lock);
        for (i = 0; i < WT_CACHE_TOP_SLOTS; ++i)
            if (array->slots[i].btree == btree) {
                __wt_atomic_store_uint8_relaxed(
                  &btree->cache_top_slot[metric], WT_CACHE_TOP_NOT_TRACKED);
                array->slots[i].btree = NULL;
                array->slots[i].value = 0;
            }
        __wt_spin_unlock(session, &array->lock);
    }
}

/*
 * __cache_top_entry_cmp --
 *     qsort comparator: descending order by value.
 */
static int
__cache_top_entry_cmp(const void *a, const void *b)
{
    const WT_CACHE_TOP_REPORT_ENTRY *ea = (const WT_CACHE_TOP_REPORT_ENTRY *)a;
    const WT_CACHE_TOP_REPORT_ENTRY *eb = (const WT_CACHE_TOP_REPORT_ENTRY *)b;

    if (ea->value > eb->value)
        return (-1);
    if (ea->value < eb->value)
        return (1);
    return (0);
}

/*
 * __cache_top_scan --
 *     Copy one ranking's current entries into a caller-supplied array, largest first. Along the
 *     way, drop any tracked tree that has fallen below the threshold, and adjust the threshold so
 *     the ranking stays usefully full. Naming the tables is what makes this allocate, and the only
 *     thing that can make it fail. The names are copied under the lock because a tree's name is
 *     only guaranteed to be there while the lock keeps the tree in its slot.
 */
static int
__cache_top_scan(WT_SESSION_IMPL *session, WT_CACHE_TOP_METRIC metric,
  WT_CACHE_TOP_REPORT_ENTRY *entries, bool with_names, uint32_t *countp, uint64_t *thresholdp,
  uint64_t *listedp)
{
    WT_CACHE_TOP_ARRAY *array;
    WT_DATA_HANDLE *dhandle;
    WT_DECL_RET;
    uint64_t in_force, listed, smallest, threshold, value;
    uint32_t count, i;

    array = &S2C(session)->cache->cache_top.arrays[metric];
    in_force = threshold = __cache_top_threshold(session, array);
    count = 0;
    listed = 0;
    smallest = UINT64_MAX;

    __wt_spin_lock(session, &array->lock);
    for (i = 0; i < WT_CACHE_TOP_SLOTS; ++i) {
        if (array->slots[i].btree == NULL)
            continue;

        /*
         * A dropped or closed handle is not cleaned up right away, and neither is a superseded
         * disaggregated generation. Report either as holding nothing rather than naming a table
         * that is gone, or the same table once per generation.
         */
        dhandle = array->slots[i].btree->dhandle;
        value = !WT_DHANDLE_CAN_REOPEN(dhandle) ?
          0 :
          __cache_top_value(session, array->slots[i].btree, metric);
        if (value < threshold) {
            __wt_atomic_store_uint8_relaxed(
              &array->slots[i].btree->cache_top_slot[metric], WT_CACHE_TOP_NOT_TRACKED);
            array->slots[i].btree = NULL;
            array->slots[i].value = 0;
            continue;
        }
        array->slots[i].value = value;
        smallest = WT_MIN(smallest, value);

        if (with_names)
            WT_ERR(__wt_strdup(session, dhandle->name, &entries[count].name));
        entries[count].value = value;
        listed += value;
        ++count;
    }

    /*
     * Move the bar towards the smallest table worth ranking. A full ranking places it exactly, just
     * above the smallest entry kept. Below that the bar is too high and has to come down by
     * guesswork: an empty ranking says nothing about how far off it is, so it falls by a larger
     * factor than a sparse one. Either way the ranking refills only as trees grow past the new bar.
     */
    if (count == WT_CACHE_TOP_SLOTS)
        threshold = smallest + 1;
    else if (count == 0)
        threshold = WT_MAX(threshold / 8, WT_CACHE_TOP_THRESHOLD_FLOOR);
    else if (count < WT_CACHE_TOP_SLOTS / 2)
        threshold = WT_MAX(threshold / 2, WT_CACHE_TOP_THRESHOLD_FLOOR);
    __wt_atomic_store_uint64_relaxed(&array->threshold, threshold);

err:
    __wt_spin_unlock(session, &array->lock);
    if (ret != 0) {
        /* This function owns whatever it allocated so far; it failed before telling the caller. */
        if (with_names)
            __cache_top_entries_free(session, entries);
        return (ret);
    }

    __wt_qsort(entries, count, sizeof(WT_CACHE_TOP_REPORT_ENTRY), __cache_top_entry_cmp);

    *countp = count;
    *listedp = listed;

    /* The threshold the entries were selected against, not the one adjusted for the next report. */
    *thresholdp = in_force;
    return (0);
}

/*
 * __cache_top_snapshot --
 *     Snapshot a ranking with each table named, for a caller that is going to print them.
 */
static int
__cache_top_snapshot(WT_SESSION_IMPL *session, WT_CACHE_TOP_METRIC metric,
  WT_CACHE_TOP_REPORT_ENTRY *entries, uint32_t *countp, uint64_t *thresholdp, uint64_t *listedp)
{
    return (__cache_top_scan(session, metric, entries, true, countp, thresholdp, listedp));
}

/*
 * __cache_top_totals --
 *     Snapshot a ranking for its totals alone: what every tracked table holds, and what the largest
 *     few of them hold. Naming nothing means this allocates nothing, so it cannot fail.
 */
static void
__cache_top_totals(
  WT_SESSION_IMPL *session, WT_CACHE_TOP_METRIC metric, uint64_t *listedp, uint64_t *largestp)
{
    WT_CACHE_TOP_REPORT_ENTRY entries[WT_CACHE_TOP_SLOTS];
    uint64_t threshold;
    uint32_t count, i;

    WT_CLEAR(entries);
    WT_IGNORE_RET(__cache_top_scan(session, metric, entries, false, &count, &threshold, listedp));

    /* Entries come back largest first, so the head of the array is the largest few. */
    *largestp = 0;
    for (i = 0; i < WT_MIN(count, WT_CACHE_TOP_VERBOSE_ENTRIES); ++i)
        *largestp += entries[i].value;
}

/*
 * __cache_top_emit --
 *     Print one line of a report to the log for an explicitly requested report, tagged with the
 *     verbose category otherwise.
 */
static int
__cache_top_emit(WT_SESSION_IMPL *session, bool force, const char *line)
{
    if (force)
        return (__wt_msg(session, "%s", line));

    __wt_verbose_worker(session, WT_VERB_CACHE_TOP, WT_VERBOSE_INFO, "%s", line);
    return (0);
}

/*
 * __cache_top_metric_desc --
 *     Return the name of a ranking, for the report.
 */
static const char *
__cache_top_metric_desc(WT_CACHE_TOP_METRIC metric)
{
    switch (metric) {
    case WT_CACHE_TOP_DIRTY:
        return ("dirty leaf bytes");
    case WT_CACHE_TOP_EVICT:
        return ("recent bytes evicted");
    case WT_CACHE_TOP_INMEM:
        return ("total cache bytes");
    case WT_CACHE_TOP_READ:
        return ("recent bytes read");
    case WT_CACHE_TOP_UPDATES:
        return ("update bytes");
    }

    return ("unknown");
}

/*
 * __cache_top_report --
 *     Build and print all of the rankings. Callers only reach this when they already intend to
 *     print something, so it always does both.
 */
static int
__cache_top_report(WT_SESSION_IMPL *session, bool force)
{
    WT_CACHE *cache;
    WT_CACHE_TOP_REPORT_ENTRY *entries;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_ITEM *line;
    uint64_t listed, threshold;
    uint64_t connection_total = 0;
    u_int metric;
    uint32_t count, i, listing;
    bool has_total;

    conn = S2C(session);
    cache = conn->cache;

    listing = force || WT_VERBOSE_LEVEL_ISSET(session, WT_VERB_CACHE_TOP, WT_VERBOSE_DEBUG_2) ?
      WT_CACHE_TOP_SLOTS :
      WT_CACHE_TOP_VERBOSE_ENTRIES;

    WT_RET(__wt_calloc_def(session, WT_CACHE_TOP_SLOTS, &entries));
    WT_ERR(__wt_scr_alloc(session, 0, &line));

    for (metric = 0; metric < WT_CACHE_TOP_METRICS; ++metric) {
        WT_ERR(__cache_top_snapshot(
          session, (WT_CACHE_TOP_METRIC)metric, entries, &count, &threshold, &listed));

        /*
         * A level ranking shows its listed tables against both what the connection currently holds
         * and the configured cache size: the first tells a reader whether the rest is spread across
         * tables too small to rank, the second whether any of it is worth worrying about. A decayed
         * ranking has no connection-wide equivalent. The listed bytes come from the trees and the
         * totals include the cache overhead percentage, so the two disagree slightly: these
         * rankings name tables, they do not reconcile byte counts.
         */
        has_total = true;
        switch ((WT_CACHE_TOP_METRIC)metric) {
        case WT_CACHE_TOP_UPDATES:
            connection_total = __wt_cache_bytes_updates(cache);
            break;
        case WT_CACHE_TOP_DIRTY:
            connection_total = __wt_cache_dirty_leaf_inuse(cache);
            break;
        case WT_CACHE_TOP_INMEM:
            connection_total = __wt_cache_bytes_inuse(cache);
            break;
        case WT_CACHE_TOP_READ:
        case WT_CACHE_TOP_EVICT:
            has_total = false;
            break;
        }

        if (has_total)
            WT_ERR(__wt_buf_fmt(session, line,
              "cache top %s: %" PRIu32 " tables above %" PRIu64 "B hold %" PRIu64 "B of %" PRIu64
              "B in use, %" PRIu64 "B configured",
              __cache_top_metric_desc((WT_CACHE_TOP_METRIC)metric), count, threshold, listed,
              connection_total, conn->cache_size));
        else
            WT_ERR(__wt_buf_fmt(session, line,
              "cache top %s: %" PRIu32 " tables above %" PRIu64 "B hold %" PRIu64 "B",
              __cache_top_metric_desc((WT_CACHE_TOP_METRIC)metric), count, threshold, listed));
        WT_ERR(__cache_top_emit(session, force, (const char *)line->data));

        for (i = 0; i < WT_MIN(count, listing); ++i) {
            WT_ERR(__wt_buf_fmt(
              session, line, "    %" PRIu64 "B %s", entries[i].value, entries[i].name));
            WT_ERR(__cache_top_emit(session, force, (const char *)line->data));
        }

        __cache_top_entries_free(session, entries);
    }

err:
    /* Whatever the metric in progress had allocated when something failed still needs freeing. */
    __cache_top_entries_free(session, entries);
    __wt_scr_free(session, &line);
    __wt_free(session, entries);
    return (ret);
}

/*
 * __cache_top_pct --
 *     Calculate the percentage of part relative to whole. Returns 0 if whole is zero.
 */
static WT_INLINE int64_t
__cache_top_pct(uint64_t part, uint64_t whole)
{
    if (whole == 0)
        return (0);

    return ((int64_t)(part * 100 / whole));
}

/*
 * __wt_cache_top_stats_update --
 *     Refresh every ranking and publish how much of the cache the ranked tables hold. Called
 *     periodically rather than on demand.
 */
void
__wt_cache_top_stats_update(WT_SESSION_IMPL *session)
{
    uint64_t cache_size, largest[WT_CACHE_TOP_METRICS], listed[WT_CACHE_TOP_METRICS];
    u_int metric;

    for (metric = 0; metric < WT_CACHE_TOP_METRICS; ++metric)
        __cache_top_totals(session, (WT_CACHE_TOP_METRIC)metric, &listed[metric], &largest[metric]);

    /*
     * Every share is measured against the configured cache size rather than against what the cache
     * currently holds, which would make any table in an almost empty cache look like it held all of
     * it. The largest few alongside the whole ranking then say how concentrated the cache is: two
     * figures that are similar mean a handful of tables hold it, two far apart mean it is spread
     * across many.
     */
    cache_size = S2C(session)->cache_size;

    /* WT_STAT_CONN_SET publishes into one bucket and empties the rest: a gauge, not a sum. */

    WT_STAT_CONN_SET(
      session, cache_top_inuse_pct, __cache_top_pct(listed[WT_CACHE_TOP_INMEM], cache_size));
    WT_STAT_CONN_SET(
      session, cache_top5_inuse_pct, __cache_top_pct(largest[WT_CACHE_TOP_INMEM], cache_size));

    WT_STAT_CONN_SET(
      session, cache_top_updates_pct, __cache_top_pct(listed[WT_CACHE_TOP_UPDATES], cache_size));
    WT_STAT_CONN_SET(
      session, cache_top5_updates_pct, __cache_top_pct(largest[WT_CACHE_TOP_UPDATES], cache_size));

    WT_STAT_CONN_SET(
      session, cache_top_dirty_pct, __cache_top_pct(listed[WT_CACHE_TOP_DIRTY], cache_size));
    WT_STAT_CONN_SET(
      session, cache_top5_dirty_pct, __cache_top_pct(largest[WT_CACHE_TOP_DIRTY], cache_size));
}

/*
 * __wt_cache_top_report --
 *     Print all of the rankings, unconditionally. Used by WT_CONNECTION::debug_info.
 */
int
__wt_cache_top_report(WT_SESSION_IMPL *session)
{
    return (__cache_top_report(session, true));
}

/*
 * __wt_cache_top_maintain --
 *     Called periodically, reporting only while the verbose category is on. Skipping a tick loses
 *     nothing: a report reads live values, so the next one catches up.
 */
int
__wt_cache_top_maintain(WT_SESSION_IMPL *session)
{
    if (!WT_VERBOSE_LEVEL_ISSET(session, WT_VERB_CACHE_TOP, WT_VERBOSE_INFO))
        return (0);

    return (__cache_top_report(session, false));
}

/*
 * __wti_cache_top_init --
 *     Set up the cache consumption rankings for a newly created cache.
 */
int
__wti_cache_top_init(WT_SESSION_IMPL *session)
{
    WT_CACHE_TOP *top;
    u_int metric;

    top = &S2C(session)->cache->cache_top;

    top->halflife_ticks = (uint64_t)((double)WT_CACHE_TOP_FLOW_HALFLIFE_US * (double)WT_THOUSAND *
      __wt_process.tsc_nsec_ratio);
    top->halflife_ticks = WT_MAX(top->halflife_ticks, 1);

    for (metric = 0; metric < WT_CACHE_TOP_METRICS; ++metric)
        WT_RET(__wt_spin_init(session, &top->arrays[metric].lock, "cache top consumers"));

    return (0);
}

/*
 * __wti_cache_top_destroy --
 *     Tear down the cache consumption rankings when the cache is destroyed.
 */
void
__wti_cache_top_destroy(WT_SESSION_IMPL *session)
{
    WT_CACHE_TOP *top;
    u_int metric;

    top = &S2C(session)->cache->cache_top;

    for (metric = 0; metric < WT_CACHE_TOP_METRICS; ++metric)
        __wt_spin_destroy(session, &top->arrays[metric].lock);
}
