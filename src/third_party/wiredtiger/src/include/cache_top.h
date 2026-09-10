/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/* The rankings this file maintains, one metric per ranking. */
typedef enum {
    WT_CACHE_TOP_DIRTY = 0, /* Dirty leaf bytes held by the tree. */
    WT_CACHE_TOP_EVICT,     /* Recent bytes evicted from the tree. */
    WT_CACHE_TOP_INMEM,     /* Total resident bytes held by the tree. */
    WT_CACHE_TOP_READ,      /* Recent bytes read into cache by the tree. */
    WT_CACHE_TOP_UPDATES    /* Update bytes held by the tree. */
} WT_CACHE_TOP_METRIC;

/*
 * The number of rankings above, which has to name the last of them. Kept as a plain constant rather
 * than a trailing enumerator, so that a switch over WT_CACHE_TOP_METRIC only ever has to list
 * values that can actually occur, instead of also handling a sentinel that -Wswitch-enum would
 * otherwise require every such switch to name.
 */
#define WT_CACHE_TOP_METRICS (WT_CACHE_TOP_UPDATES + 1)

/*
 * The level metrics (update, dirty leaf and resident bytes) sum to at most the cache size. That
 * bound is what lets a fixed number of slots hold every tree worth reporting rather than a sample:
 * set the threshold to cache size divided by the slot count, and at most that many trees can be
 * above it, so a tree not in the ranking is provably below the threshold. Decay bounds the flow
 * metrics (bytes read and evicted) by rate rather than by cache size, so their rankings hold the
 * largest consumers seen and carry no completeness guarantee. The threshold is adjusted over time
 * to keep a ranking reasonably full, so cache usage spread thin across many tables still produces a
 * useful ranking.
 */
#define WT_CACHE_TOP_SLOTS 32

/*
 * The value WT_BTREE.cache_top_slot holds for a metric the tree is not currently part of. Equal to
 * the slot count, so every valid slot index (0 to WT_CACHE_TOP_SLOTS - 1) is distinguishable from
 * it. A tree's slot fields must be set to this explicitly when the tree is opened: zero is a valid
 * slot index, so relying on the struct's zero-initialization would wrongly claim slot 0.
 */
#define WT_CACHE_TOP_NOT_TRACKED WT_CACHE_TOP_SLOTS

/*
 * How many tables a report names per ranking when the verbose category is on but below DEBUG_2. The
 * rankings hold far more than an operator watching a log wants to read every time the sweep server
 * comes around. The full ranking is still there at DEBUG_2 and on request.
 */
#define WT_CACHE_TOP_VERBOSE_ENTRIES 5

/*
 * A tree is reconsidered for a ranking once it has grown by threshold / this value since its last
 * check. A smaller divisor means the rankings track growth more closely, but trees touch the shared
 * ranking state more often; a larger divisor means the opposite.
 */
#define WT_CACHE_TOP_RECHECK_DIVISOR 8

/* How long it takes a flow metric's value (bytes read, bytes evicted) to decay by half. */
#define WT_CACHE_TOP_FLOW_HALFLIFE_US (30 * WT_MILLION)

/*
 * The lowest a ranking's threshold is ever allowed to go. This has nothing to do with recheck
 * spacing (see WT_CACHE_TOP_RECHECK_MIN_SPACING below, a separate concern) - it only keeps the
 * threshold above zero, which the adjustment arithmetic in a report assumes. Deliberately small, so
 * a connection whose real tables are genuinely this size is not permanently unable to produce a
 * nonempty ranking.
 */
#define WT_CACHE_TOP_THRESHOLD_FLOOR (4 * WT_KILOBYTE)

/*
 * The least a tree is ever allowed to grow before being reconsidered, regardless of how low the
 * threshold itself has fallen. Deriving recheck spacing purely from threshold / RECHECK_DIVISOR
 * would tie two unrelated things together: how small a table is worth naming, which can reasonably
 * be quite small, and how often a busy table calls back into the tracking function, which must not
 * scale down with it - a table growing at 100MB/s should not place tens of thousands of calls a
 * second just because its ranking's threshold happens to be low.
 */
#define WT_CACHE_TOP_RECHECK_MIN_SPACING WT_MEGABYTE

/*
 * The width, in bits, of the counters this decay operates on. Shifting a value right by this many
 * bits or more is undefined behavior in C, so it is also the point past which decay stops trying to
 * compute a shift and just reports zero; any real value reaches zero in far fewer halvings than
 * this.
 */
#define WT_CACHE_TOP_DECAY_MAX_HALVINGS 64

struct __wt_cache_top_entry {
    WT_BTREE *btree; /* The tree in this slot, or NULL if the slot is unused. */
    uint64_t value;  /* That tree's value, as of the last time this slot was updated. */
};

struct __wt_cache_top_array {
    WT_SPINLOCK lock; /* Guards everything below, including which tree each slot points at. */

    /* Read and updated without the lock: a caller that reads a stale value wastes one visit. */
    wt_shared uint64_t threshold;
    WT_CACHE_TOP_ENTRY slots[WT_CACHE_TOP_SLOTS];
};

struct __wt_cache_top {
    WT_CACHE_TOP_ARRAY arrays[WT_CACHE_TOP_METRICS];

    /* WT_CACHE_TOP_FLOW_HALFLIFE_US converted into the same units __wt_clock returns. */
    uint64_t halflife_ticks;
};
