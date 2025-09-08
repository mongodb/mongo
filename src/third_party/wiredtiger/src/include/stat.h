/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*
 * Statistics counters:
 *
 * We use an array of statistics structures; threads write different structures to avoid writing the
 * same cache line and incurring cache coherency overheads, which can dramatically slow fast and
 * otherwise read-mostly workloads.
 *
 * With an 8B statistics value and 64B cache-line alignment, 8 values share the same cache line.
 * There are collisions when different threads choose the same statistics structure and update
 * values that live on the cache line. There is likely some locality however: a thread updating the
 * cursor search statistic is likely to update other cursor statistics with a chance of hitting
 * already cached values.
 *
 * The actual statistic value must be signed, because one thread might increment the value in its
 * structure, and then another thread might decrement the same value in another structure (where the
 * value was initially zero), so the value in the second thread's slot will go negative.
 *
 * When reading a statistics value, the array values are summed and returned to the caller. The
 * summation is performed without locking, so the value read may be inconsistent (and might be
 * negative, if increments/decrements race with the reader).
 *
 * Choosing how many structures isn't easy: obviously, a smaller number creates more conflicts while
 * a larger number uses more memory.
 *
 * Ideally, if the application running on the system is CPU-intensive, and using all CPUs on the
 * system, we want to use the same number of slots as there are CPUs (because their L1 caches are
 * the units of coherency). However, in practice we cannot easily determine how many CPUs are
 * actually available to the application.
 *
 * Our next best option is to use the number of threads in the application as a heuristic for the
 * number of CPUs (presumably, the application architect has figured out how many CPUs are
 * available). However, inside WiredTiger we don't know when the application creates its threads.
 *
 * For now, we use a fixed number of slots. Ideally, we would approximate the largest number of
 * cores we expect on any machine where WiredTiger might be run, however, we don't want to waste
 * that much memory on smaller machines. As of 2015, machines with more than 24 CPUs are relatively
 * rare.
 *
 * Default hash table size; use a prime number of buckets rather than assuming a good hash
 * (Reference Sedgewick, Algorithms in C, "Hash Functions").
 *
 * The counter slots are split into two separate counters, one for connection and the other for
 * data-source. This is because we want to be able to independently increase one counter slot
 * without increasing the other, as for example, increasing the data-source counter by a small
 * number would have a greater impact than increasing the connection counter by the same number -
 * depending on the number of dhandles in the system.
 *
 */
#define WT_STAT_CONN_COUNTER_SLOTS 23
#define WT_STAT_DSRC_COUNTER_SLOTS 23

/*
 * WT_STATS_###_SLOT_ID is the thread's slot ID for the array of structures.
 *
 * Ideally, we want a slot per CPU, and we want each thread to index the slot corresponding to the
 * CPU it runs on. Unfortunately, getting the ID of the current CPU is difficult: some operating
 * systems provide a system call to acquire a CPU ID, but not all (regardless, making a system call
 * to increment a statistics value is far too expensive).
 *
 * Our second-best option is to use the thread ID. Unfortunately, there is no portable way to obtain
 * a unique thread ID that's a small-enough number to be used as an array index (portable thread IDs
 * are usually a pointer or an opaque chunk, not a simple integer).
 *
 * Our solution is to use the session ID; there is normally a session per thread and the session ID
 * is a small, monotonically increasing number.
 */
#define WT_STATS_CONN_SLOT_ID(session) (((session)->id) % WT_STAT_CONN_COUNTER_SLOTS)
#define WT_STATS_DSRC_SLOT_ID(session) (((session)->id) % WT_STAT_DSRC_COUNTER_SLOTS)

/*
 * Statistic structures are arrays of int64_t's. We have functions to read/write those structures
 * regardless of the specific statistic structure we're working with, by translating statistics
 * structure field names to structure offsets.
 *
 * Translate a statistic's value name to an offset in the array.
 */
#define WT_STATS_FIELD_TO_OFFSET(stats, fld) (int)(&(stats)[0]->fld - (int64_t *)(stats)[0])

#define WT_SESSION_STATS_FIELD_TO_OFFSET(stats, fld) (int)(&(stats)->fld - (int64_t *)(stats))

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_STAT_CLEAR 0x01u
#define WT_STAT_JSON 0x02u
#define WT_STAT_ON_CLOSE 0x04u
#define WT_STAT_TYPE_ALL 0x08u
#define WT_STAT_TYPE_CACHE_WALK 0x10u
#define WT_STAT_TYPE_FAST 0x20u
#define WT_STAT_TYPE_SIZE 0x40u
#define WT_STAT_TYPE_TREE_WALK 0x80u
/* AUTOMATIC FLAG VALUE GENERATION STOP 32 */

/*
 * Sum the values from all structures in the array.
 */
static WT_INLINE int64_t
__wt_stats_aggregate_internal(void *stats_arg, int slot, u_int num_slots)
{
    int64_t **stats, aggr_v;
    u_int i;

    stats = (int64_t **)stats_arg;
    for (aggr_v = 0, i = 0; i < num_slots; i++)
        aggr_v += stats[i][slot];

    /*
     * This can race. However, any implementation with a single value can race as well, different
     * threads could set the same counter value simultaneously. While we are making races more
     * likely, we are not fundamentally weakening the isolation semantics found in updating a single
     * value.
     *
     * Additionally, the aggregation can go negative (imagine a thread incrementing a value after
     * aggregation has passed its slot and a second thread decrementing a value before aggregation
     * has reached its slot).
     *
     * For historic API compatibility, the external type is a uint64_t; limit our return to positive
     * values, negative numbers would just look really, really large.
     */
    if (aggr_v < 0)
        aggr_v = 0;
    return (aggr_v);
}

static WT_INLINE int64_t
__wt_stats_aggregate_conn(void *stats_arg, int slot)
{
    return (__wt_stats_aggregate_internal(stats_arg, slot, WT_STAT_CONN_COUNTER_SLOTS));
}

static WT_INLINE int64_t
__wt_stats_aggregate_dsrc(void *stats_arg, int slot)
{
    return (__wt_stats_aggregate_internal(stats_arg, slot, WT_STAT_DSRC_COUNTER_SLOTS));
}

/*
 * Clear the values in all structures in the array for connection statistics.
 */
static WT_INLINE void
__wt_stats_clear_conn(void *stats_arg, int slot)
{
    int64_t **stats;
    int i;

    stats = (int64_t **)stats_arg;
    for (i = 0; i < WT_STAT_CONN_COUNTER_SLOTS; i++)
        stats[i][slot] = 0;
}

/*
 * Clear the values in all structures in the array for data-source statistics.
 */
static WT_INLINE void
__wt_stats_clear_dsrc(void *stats_arg, int slot)
{
    int64_t **stats;
    int i;

    stats = (int64_t **)stats_arg;
    for (i = 0; i < WT_STAT_DSRC_COUNTER_SLOTS; i++)
        stats[i][slot] = 0;
}

/*
 * Read/write statistics if statistics gathering is enabled. Reading and writing the field requires
 * different actions: reading sums the values across the array of structures, writing updates a
 * single structure's value.
 *
 * The read statistics are separated into data-source or connection statistics as the counter slots
 * for the statistics are separate. The write statistics do not rely on counter slots in this way so
 * they do not need to be split.
 */
#define WT_STAT_ENABLED(session) (S2C(session)->stat_flags != 0)

#define WT_STAT_CONN_READ(stats, fld) \
    __wt_stats_aggregate_conn(stats, WT_STATS_FIELD_TO_OFFSET(stats, fld))
#define WT_STAT_DSRC_READ(stats, fld) \
    __wt_stats_aggregate_dsrc(stats, WT_STATS_FIELD_TO_OFFSET(stats, fld))
#define WT_STAT_SESSION_READ(stats, fld) ((stats)->fld)
#define WT_STAT_WRITE(session, stats, fld, v) \
    do {                                      \
        if (WT_STAT_ENABLED(session))         \
            (stats)->fld = (int64_t)(v);      \
    } while (0)

#define WT_STAT_SET_BASE(session, stat, fld, value) \
    do {                                            \
        if (WT_STAT_ENABLED(session))               \
            (stat)->fld = (int64_t)(value);         \
    } while (0)
#define WT_STAT_DECRV_BASE(session, stat, fld, value) \
    do {                                              \
        if (WT_STAT_ENABLED(session))                 \
            (stat)->fld -= (int64_t)(value);          \
    } while (0)
#define WT_STAT_DECRV_ATOMIC_BASE(session, stat, fld, value)          \
    do {                                                              \
        if (WT_STAT_ENABLED(session))                                 \
            (void)__wt_atomic_subi64(&(stat)->fld, (int64_t)(value)); \
    } while (0)
#define WT_STAT_INCRV_BASE(session, stat, fld, value) \
    do {                                              \
        if (WT_STAT_ENABLED(session))                 \
            (stat)->fld += (int64_t)(value);          \
    } while (0)
#define WT_STAT_INCRV_ATOMIC_BASE(session, stat, fld, value)          \
    do {                                                              \
        if (WT_STAT_ENABLED(session))                                 \
            (void)__wt_atomic_addi64(&(stat)->fld, (int64_t)(value)); \
    } while (0)

/*
 * The following connection and data-source statistic updates are done to their separate statistic
 * buckets. WT_STATP_### should be used when you want to give a statistic pointer, in cases where
 * the statistic structure is not tied to the current session or data-source. WT_STAT_### is used
 * when there is no pointer given, and the implied statistic bucket is tied to the session.
 *
 * Update connection handle statistics if statistics gathering is enabled.
 */
#define WT_STAT_CONN_DECRV(session, fld, value) \
    WT_STAT_DECRV_BASE(session, S2C(session)->stats[(session)->stat_conn_bucket], fld, value)
#define WT_STAT_CONN_DECR_ATOMIC(session, fld) \
    WT_STAT_DECRV_ATOMIC_BASE(session, S2C(session)->stats[(session)->stat_conn_bucket], fld, 1)
#define WT_STAT_CONN_DECRV_ATOMIC(session, fld, value) \
    WT_STAT_DECRV_ATOMIC_BASE(session, S2C(session)->stats[(session)->stat_conn_bucket], fld, value)
#define WT_STAT_CONN_DECR(session, fld) WT_STAT_CONN_DECRV(session, fld, 1)

#define WT_STAT_CONN_INCRV(session, fld, value) \
    WT_STAT_INCRV_BASE(session, S2C(session)->stats[(session)->stat_conn_bucket], fld, value)
#define WT_STAT_CONN_INCR_ATOMIC(session, fld) \
    WT_STAT_INCRV_ATOMIC_BASE(session, S2C(session)->stats[(session)->stat_conn_bucket], fld, 1)
#define WT_STAT_CONN_INCRV_ATOMIC(session, fld, value) \
    WT_STAT_INCRV_ATOMIC_BASE(session, S2C(session)->stats[(session)->stat_conn_bucket], fld, value)
#define WT_STAT_CONN_INCR(session, fld) WT_STAT_CONN_INCRV(session, fld, 1)

#define WT_STATP_CONN_SET(session, stats, fld, value)                           \
    do {                                                                        \
        if (WT_STAT_ENABLED(session)) {                                         \
            __wt_stats_clear_conn(stats, WT_STATS_FIELD_TO_OFFSET(stats, fld)); \
            WT_STAT_SET_BASE(session, (stats)[0], fld, value);                  \
        }                                                                       \
    } while (0)
#define WT_STAT_CONN_SET(session, fld, value) \
    WT_STATP_CONN_SET(session, S2C(session)->stats, fld, value)

/*
 * Update data-source handle statistics if statistics gathering is enabled and the data-source
 * handle is set.
 *
 * XXX We shouldn't have to check if the data-source handle is NULL, but it's necessary until
 * everything is converted to using data-source handles.
 */
#define WT_STATP_DSRC_INCRV(session, stats, fld, value)                                \
    do {                                                                               \
        WT_STAT_INCRV_BASE(session, (stats)[(session)->stat_dsrc_bucket], fld, value); \
    } while (0)
#define WT_STATP_DSRC_INCR(session, stats, fld) WT_STATP_DSRC_INCRV(session, stats, fld, 1)
#define WT_STAT_DSRC_INCRV(session, fld, value)                                   \
    do {                                                                          \
        if ((session)->dhandle != NULL && (session)->dhandle->stat_array != NULL) \
            WT_STATP_DSRC_INCRV(session, (session)->dhandle->stats, fld, value);  \
    } while (0)
#define WT_STAT_DSRC_INCR(session, fld) WT_STAT_DSRC_INCRV(session, fld, 1)

#define WT_STATP_DSRC_DECRV(session, stats, fld, value)                                \
    do {                                                                               \
        WT_STAT_DECRV_BASE(session, (stats)[(session)->stat_dsrc_bucket], fld, value); \
    } while (0)
#define WT_STATP_DSRC_DECR(session, stats, fld) WT_STATP_DSRC_DECRV(session, stats, fld, 1)
#define WT_STAT_DSRC_DECRV(session, fld, value)                                   \
    do {                                                                          \
        if ((session)->dhandle != NULL && (session)->dhandle->stat_array != NULL) \
            WT_STATP_DSRC_DECRV(session, (session)->dhandle->stats, fld, value);  \
    } while (0)
#define WT_STAT_DSRC_DECR(session, fld) WT_STAT_DSRC_DECRV(session, fld, 1)

#define WT_STATP_DSRC_SET(session, stats, fld, value)                           \
    do {                                                                        \
        if (WT_STAT_ENABLED(session)) {                                         \
            __wt_stats_clear_dsrc(stats, WT_STATS_FIELD_TO_OFFSET(stats, fld)); \
            WT_STAT_SET_BASE(session, (stats)[0], fld, value);                  \
        }                                                                       \
    } while (0)
#define WT_STAT_DSRC_SET(session, fld, value)                                     \
    do {                                                                          \
        if ((session)->dhandle != NULL && (session)->dhandle->stat_array != NULL) \
            WT_STATP_DSRC_SET(session, (session)->dhandle->stats, fld, value);    \
    } while (0)

/*
 * Update connection and data handle statistics if statistics gathering is enabled. Updates both
 * statistics concurrently and is useful to avoid the duplicated calls that happen in a lot of
 * places.
 */
#define WT_STAT_CONN_DSRC_DECRV(session, fld, value) \
    do {                                             \
        WT_STAT_CONN_DECRV(session, fld, value);     \
        WT_STAT_DSRC_DECRV(session, fld, value);     \
    } while (0)
#define WT_STAT_CONN_DSRC_DECR(session, fld) WT_STAT_CONN_DSRC_DECRV(session, fld, 1)

#define WT_STAT_CONN_DSRC_INCRV(session, fld, value) \
    do {                                             \
        WT_STAT_CONN_INCRV(session, fld, value);     \
        WT_STAT_DSRC_INCRV(session, fld, value);     \
    } while (0)
#define WT_STAT_CONN_DSRC_INCR(session, fld) WT_STAT_CONN_DSRC_INCRV(session, fld, 1)
/*
 * Update per session statistics.
 */
#define WT_STAT_SESSION_INCRV(session, fld, value) \
    WT_STAT_INCRV_BASE(session, &(session)->stats, fld, value)
#define WT_STAT_SESSION_SET(session, fld, value) \
    WT_STAT_SET_BASE(session, &(session)->stats, fld, value)

/*
 * Construct histogram increment functions to put the passed value into the right bucket. Bucket
 * ranges, represented by various statistics, depend upon whether the passed value is in
 * milliseconds or microseconds.
 */
#define WT_STAT_MSECS_HIST_INCR_FUNC(name, stat)                \
    static WT_INLINE void __wt_stat_msecs_hist_incr_##name(     \
      WT_SESSION_IMPL *session, uint64_t msecs)                 \
    {                                                           \
        WT_STAT_CONN_INCRV(session, stat##_total_msecs, msecs); \
        if (msecs < 10)                                         \
            WT_STAT_CONN_INCR(session, stat##_lt10);            \
        else if (msecs < 50)                                    \
            WT_STAT_CONN_INCR(session, stat##_lt50);            \
        else if (msecs < 100)                                   \
            WT_STAT_CONN_INCR(session, stat##_lt100);           \
        else if (msecs < 250)                                   \
            WT_STAT_CONN_INCR(session, stat##_lt250);           \
        else if (msecs < 500)                                   \
            WT_STAT_CONN_INCR(session, stat##_lt500);           \
        else if (msecs < WT_THOUSAND)                           \
            WT_STAT_CONN_INCR(session, stat##_lt1000);          \
        else                                                    \
            WT_STAT_CONN_INCR(session, stat##_gt1000);          \
    }

#define WT_STAT_USECS_HIST_INCR_FUNC(name, stat)                \
    static WT_INLINE void __wt_stat_usecs_hist_incr_##name(     \
      WT_SESSION_IMPL *session, uint64_t usecs)                 \
    {                                                           \
        WT_STAT_CONN_INCRV(session, stat##_total_usecs, usecs); \
        if (usecs < 100)                                        \
            WT_STAT_CONN_INCR(session, stat##_lt100);           \
        else if (usecs < 250)                                   \
            WT_STAT_CONN_INCR(session, stat##_lt250);           \
        else if (usecs < 500)                                   \
            WT_STAT_CONN_INCR(session, stat##_lt500);           \
        else if (usecs < WT_THOUSAND)                           \
            WT_STAT_CONN_INCR(session, stat##_lt1000);          \
        else if (usecs < 10 * WT_THOUSAND)                      \
            WT_STAT_CONN_INCR(session, stat##_lt10000);         \
        else                                                    \
            WT_STAT_CONN_INCR(session, stat##_gt10000);         \
    }

#define WT_STAT_COMPR_RATIO_READ_HIST_INCR_FUNC(ratio)                \
    static WT_INLINE void __wt_stat_compr_ratio_read_hist_incr(       \
      WT_SESSION_IMPL *session, uint64_t ratio)                       \
    {                                                                 \
        if (ratio < 2)                                                \
            WT_STAT_DSRC_INCR(session, compress_read_ratio_hist_2);   \
        else if (ratio < 4)                                           \
            WT_STAT_DSRC_INCR(session, compress_read_ratio_hist_4);   \
        else if (ratio < 8)                                           \
            WT_STAT_DSRC_INCR(session, compress_read_ratio_hist_8);   \
        else if (ratio < 16)                                          \
            WT_STAT_DSRC_INCR(session, compress_read_ratio_hist_16);  \
        else if (ratio < 32)                                          \
            WT_STAT_DSRC_INCR(session, compress_read_ratio_hist_32);  \
        else if (ratio < 64)                                          \
            WT_STAT_DSRC_INCR(session, compress_read_ratio_hist_64);  \
        else                                                          \
            WT_STAT_DSRC_INCR(session, compress_read_ratio_hist_max); \
    }

#define WT_STAT_COMPR_RATIO_WRITE_HIST_INCR_FUNC(ratio)                \
    static WT_INLINE void __wt_stat_compr_ratio_write_hist_incr(       \
      WT_SESSION_IMPL *session, uint64_t ratio)                        \
    {                                                                  \
        if (ratio < 2)                                                 \
            WT_STAT_DSRC_INCR(session, compress_write_ratio_hist_2);   \
        else if (ratio < 4)                                            \
            WT_STAT_DSRC_INCR(session, compress_write_ratio_hist_4);   \
        else if (ratio < 8)                                            \
            WT_STAT_DSRC_INCR(session, compress_write_ratio_hist_8);   \
        else if (ratio < 16)                                           \
            WT_STAT_DSRC_INCR(session, compress_write_ratio_hist_16);  \
        else if (ratio < 32)                                           \
            WT_STAT_DSRC_INCR(session, compress_write_ratio_hist_32);  \
        else if (ratio < 64)                                           \
            WT_STAT_DSRC_INCR(session, compress_write_ratio_hist_64);  \
        else                                                           \
            WT_STAT_DSRC_INCR(session, compress_write_ratio_hist_max); \
    }

/*
 * DO NOT EDIT: automatically built by dist/stat.py.
 */
/* Statistics section: BEGIN */

/*
 * Statistics entries for connections.
 */
#define WT_CONNECTION_STATS_BASE 1000
struct __wt_connection_stats {
    int64_t autocommit_readonly_retry;
    int64_t autocommit_update_retry;
    int64_t background_compact_fail;
    int64_t background_compact_fail_cache_pressure;
    int64_t background_compact_interrupted;
    int64_t background_compact_ema;
    int64_t background_compact_bytes_recovered;
    int64_t background_compact_running;
    int64_t background_compact_exclude;
    int64_t background_compact_skipped;
    int64_t background_compact_sleep_cache_pressure;
    int64_t background_compact_success;
    int64_t background_compact_timeout;
    int64_t background_compact_files_tracked;
    int64_t backup_cursor_open;
    int64_t backup_dup_open;
    int64_t backup_granularity;
    int64_t backup_bits_clr;
    int64_t backup_incremental;
    int64_t backup_start;
    int64_t backup_blocks;
    int64_t backup_blocks_compressed;
    int64_t backup_blocks_uncompressed;
    int64_t block_cache_blocks_update;
    int64_t block_cache_bytes_update;
    int64_t block_cache_blocks_evicted;
    int64_t block_cache_bypass_filesize;
    int64_t block_cache_lookups;
    int64_t block_cache_not_evicted_overhead;
    int64_t block_cache_bypass_writealloc;
    int64_t block_cache_bypass_overhead_put;
    int64_t block_cache_bypass_get;
    int64_t block_cache_bypass_put;
    int64_t block_cache_eviction_passes;
    int64_t block_cache_hits;
    int64_t block_cache_misses;
    int64_t block_cache_bypass_chkpt;
    int64_t block_cache_blocks_removed;
    int64_t block_cache_blocks_removed_blocked;
    int64_t block_cache_blocks;
    int64_t block_cache_blocks_insert_read;
    int64_t block_cache_blocks_insert_write;
    int64_t block_cache_bytes;
    int64_t block_cache_bytes_insert_read;
    int64_t block_cache_bytes_insert_write;
    int64_t disagg_block_hs_byte_read;
    int64_t disagg_block_hs_byte_write;
    int64_t disagg_block_get;
    int64_t disagg_block_hs_get;
    int64_t disagg_block_page_discard;
    int64_t disagg_block_put;
    int64_t disagg_block_hs_put;
    int64_t block_preload;
    int64_t block_read;
    int64_t block_write;
    int64_t block_byte_read;
    int64_t block_byte_read_intl;
    int64_t block_byte_read_intl_disk;
    int64_t block_byte_read_leaf;
    int64_t block_byte_read_leaf_disk;
    int64_t block_byte_read_mmap;
    int64_t block_byte_read_syscall;
    int64_t block_byte_write_saved_delta_intl;
    int64_t block_byte_write_saved_delta_leaf;
    int64_t block_byte_write;
    int64_t block_byte_write_compact;
    int64_t block_byte_write_checkpoint;
    int64_t block_byte_write_intl_disk;
    int64_t block_byte_write_intl;
    int64_t block_byte_write_leaf_disk;
    int64_t block_byte_write_leaf;
    int64_t block_byte_write_mmap;
    int64_t block_byte_write_syscall;
    int64_t block_map_read;
    int64_t block_byte_map_read;
    int64_t block_byte_write_intl_delta_lt20;
    int64_t block_byte_write_intl_delta_lt40;
    int64_t block_byte_write_intl_delta_lt60;
    int64_t block_byte_write_intl_delta_lt80;
    int64_t block_byte_write_intl_delta_lt100;
    int64_t block_byte_write_intl_delta_gt100;
    int64_t block_byte_write_leaf_delta_lt20;
    int64_t block_byte_write_leaf_delta_lt40;
    int64_t block_byte_write_leaf_delta_lt60;
    int64_t block_byte_write_leaf_delta_lt80;
    int64_t block_byte_write_leaf_delta_lt100;
    int64_t block_byte_write_leaf_delta_gt100;
    int64_t block_remap_file_resize;
    int64_t block_remap_file_write;
    int64_t block_first_srch_walk_time;
    int64_t eviction_interupted_by_app;
    int64_t eviction_app_time;
    int64_t cache_eviction_app_threads_fill_ratio_lt_25;
    int64_t cache_eviction_app_threads_fill_ratio_25_50;
    int64_t cache_eviction_app_threads_fill_ratio_50_75;
    int64_t cache_eviction_app_threads_fill_ratio_gt_75;
    int64_t cache_read_app_count;
    int64_t cache_read_app_time;
    int64_t cache_write_app_count;
    int64_t cache_write_app_time;
    int64_t cache_bytes_delta_updates;
    int64_t cache_bytes_updates;
    int64_t cache_bytes_image;
    int64_t cache_bytes_hs;
    int64_t cache_bytes_inuse;
    int64_t cache_bytes_dirty_total;
    int64_t cache_bytes_other;
    int64_t cache_bytes_read;
    int64_t cache_bytes_write;
    int64_t cache_eviction_blocked_checkpoint;
    int64_t cache_eviction_blocked_checkpoint_hs;
    int64_t cache_bytes_hs_dirty;
    int64_t cache_eviction_blocked_disagg_dirty_internal_page;
    int64_t eviction_server_evict_attempt;
    int64_t eviction_worker_evict_attempt;
    int64_t eviction_server_evict_fail;
    int64_t eviction_worker_evict_fail;
    int64_t eviction_get_ref_empty;
    int64_t eviction_get_ref_empty2;
    int64_t eviction_aggressive_set;
    int64_t eviction_empty_score;
    int64_t cache_eviction_blocked_no_ts_checkpoint_race_1;
    int64_t cache_eviction_blocked_no_ts_checkpoint_race_2;
    int64_t cache_eviction_blocked_no_ts_checkpoint_race_3;
    int64_t cache_eviction_blocked_no_ts_checkpoint_race_4;
    int64_t cache_eviction_blocked_remove_hs_race_with_checkpoint;
    int64_t cache_eviction_blocked_no_progress;
    int64_t eviction_walk_passes;
    int64_t eviction_queue_empty;
    int64_t eviction_queue_not_empty;
    int64_t eviction_server_skip_dirty_pages_during_checkpoint;
    int64_t eviction_server_skip_intl_page_with_active_child;
    int64_t eviction_server_skip_metatdata_with_history;
    int64_t eviction_server_skip_pages_checkpoint_timestamp;
    int64_t eviction_server_skip_pages_last_running;
    int64_t eviction_server_skip_pages_retry;
    int64_t eviction_server_skip_unwanted_pages;
    int64_t eviction_server_skip_unwanted_tree;
    int64_t eviction_server_skip_trees_too_many_active_walks;
    int64_t eviction_server_skip_checkpointing_trees;
    int64_t eviction_server_skip_trees_stick_in_cache;
    int64_t eviction_server_skip_trees_eviction_disabled;
    int64_t eviction_server_skip_trees_not_useful_before;
    int64_t eviction_server_slept;
    int64_t eviction_slow;
    int64_t eviction_walk_leaf_notfound;
    int64_t eviction_state;
    int64_t eviction_walk_sleeps;
    int64_t cache_eviction_pages_queued_updates;
    int64_t cache_eviction_pages_queued_clean;
    int64_t cache_eviction_pages_queued_dirty;
    int64_t cache_eviction_pages_seen_updates;
    int64_t cache_eviction_pages_seen_clean;
    int64_t cache_eviction_pages_seen_dirty;
    int64_t npos_evict_walk_max;
    int64_t eviction_restored_pos;
    int64_t eviction_restored_pos_differ;
    int64_t cache_eviction_target_page_lt10;
    int64_t cache_eviction_target_page_lt32;
    int64_t cache_eviction_target_page_ge128;
    int64_t cache_eviction_target_page_lt64;
    int64_t cache_eviction_target_page_lt128;
    int64_t cache_eviction_target_page_reduced;
    int64_t eviction_target_strategy_clean;
    int64_t eviction_target_strategy_dirty;
    int64_t eviction_target_strategy_updates;
    int64_t eviction_walks_abandoned;
    int64_t eviction_walks_stopped;
    int64_t eviction_walks_gave_up_no_targets;
    int64_t eviction_walks_gave_up_ratio;
    int64_t eviction_walk_random_returns_null_position;
    int64_t eviction_walks_ended;
    int64_t eviction_walk_restart;
    int64_t eviction_walk_from_root;
    int64_t eviction_walk_saved_pos;
    int64_t eviction_active_workers;
    int64_t eviction_stable_state_workers;
    int64_t eviction_walks_active;
    int64_t eviction_walks_started;
    int64_t eviction_force_no_retry;
    int64_t eviction_force_hs_fail;
    int64_t eviction_force_hs;
    int64_t eviction_force_hs_success;
    int64_t eviction_force_clean;
    int64_t eviction_force_dirty;
    int64_t eviction_force_long_update_list;
    int64_t eviction_force_delete;
    int64_t eviction_force;
    int64_t eviction_force_fail;
    int64_t cache_eviction_blocked_hazard;
    int64_t cache_hazard_checks;
    int64_t cache_hazard_walks;
    int64_t cache_hazard_max;
    int64_t cache_eviction_hs_cursor_not_cached;
    int64_t cache_hs_insert;
    int64_t cache_hs_insert_restart;
    int64_t cache_hs_ondisk_max;
    int64_t cache_hs_ondisk;
    int64_t cache_hs_read;
    int64_t cache_hs_read_miss;
    int64_t cache_hs_read_squash;
    int64_t cache_hs_order_lose_durable_timestamp;
    int64_t cache_hs_key_truncate_rts_unstable;
    int64_t cache_hs_key_truncate_rts;
    int64_t cache_hs_btree_truncate;
    int64_t cache_hs_key_truncate;
    int64_t cache_hs_order_remove;
    int64_t cache_hs_key_truncate_onpage_removal;
    int64_t cache_hs_btree_truncate_dryrun;
    int64_t cache_hs_key_truncate_rts_unstable_dryrun;
    int64_t cache_hs_key_truncate_rts_dryrun;
    int64_t cache_hs_order_reinsert;
    int64_t cache_hs_write_squash;
    int64_t cache_inmem_splittable;
    int64_t cache_inmem_split;
    int64_t cache_eviction_blocked_internal_page_split;
    int64_t cache_eviction_internal;
    int64_t eviction_internal_pages_queued;
    int64_t eviction_internal_pages_seen;
    int64_t eviction_internal_pages_already_queued;
    int64_t cache_eviction_split_internal;
    int64_t cache_eviction_split_leaf;
    int64_t cache_eviction_random_sample_inmem_root;
    int64_t cache_bytes_max;
    int64_t eviction_maximum_gen_gap;
    int64_t eviction_maximum_milliseconds;
    int64_t eviction_maximum_page_size;
    int64_t eviction_app_dirty_attempt;
    int64_t eviction_app_dirty_fail;
    int64_t cache_eviction_dirty;
    int64_t cache_eviction_blocked_multi_block_reconciliation_during_checkpoint;
    int64_t npos_read_walk_max;
    int64_t cache_read_internal_delta;
    int64_t cache_read_flatten_leaf_delta;
    int64_t cache_read_flatten_leaf_delta_fail;
    int64_t cache_read_leaf_delta;
    int64_t cache_eviction_trigger_dirty_reached;
    int64_t cache_eviction_trigger_reached;
    int64_t cache_eviction_trigger_updates_reached;
    int64_t eviction_timed_out_ops;
    int64_t cache_eviction_blocked_overflow_keys;
    int64_t cache_read_overflow;
    int64_t eviction_app_attempt;
    int64_t eviction_app_fail;
    int64_t cache_eviction_blocked_materialization;
    int64_t cache_eviction_blocked_disagg_next_checkpoint;
    int64_t cache_eviction_deepen;
    int64_t cache_write_hs;
    int64_t eviction_consider_prefetch;
    int64_t cache_pages_inuse;
    int64_t cache_eviction_dirty_obsolete_tw;
    int64_t cache_eviction_ahead_of_last_materialized_lsn;
    int64_t eviction_pages_in_parallel_with_checkpoint;
    int64_t eviction_pages_ordinary_queued;
    int64_t eviction_pages_queued_post_lru;
    int64_t eviction_pages_queued_urgent;
    int64_t eviction_pages_queued_oldest;
    int64_t eviction_pages_queued_urgent_hs_dirty;
    int64_t cache_read;
    int64_t cache_read_deleted;
    int64_t cache_read_deleted_prepared;
    int64_t cache_read_checkpoint;
    int64_t eviction_clear_ordinary;
    int64_t cache_pages_requested;
    int64_t cache_pages_prefetch;
    int64_t cache_pages_requested_internal;
    int64_t cache_pages_requested_leaf;
    int64_t cache_eviction_pages_seen;
    int64_t eviction_pages_already_queued;
    int64_t eviction_fail;
    int64_t eviction_fail_active_children_on_an_internal_page;
    int64_t eviction_fail_in_reconciliation;
    int64_t eviction_fail_checkpoint_no_ts;
    int64_t eviction_walk;
    int64_t cache_write;
    int64_t cache_write_restore;
    int64_t cache_overhead;
    int64_t cache_eviction_blocked_precise_checkpoint;
    int64_t cache_evict_split_failed_lock;
    int64_t cache_eviction_blocked_recently_modified;
    int64_t cache_scrub_restore;
    int64_t cache_reverse_splits;
    int64_t cache_reverse_splits_skipped_vlcs;
    int64_t cache_eviction_hs_shared_cursor_not_cached;
    int64_t cache_read_delta_updates;
    int64_t cache_read_restored_tombstone_bytes;
    int64_t cache_hs_insert_full_update;
    int64_t cache_hs_insert_reverse_modify;
    int64_t eviction_reentry_hs_eviction_milliseconds;
    int64_t cache_bytes_internal;
    int64_t cache_bytes_leaf;
    int64_t cache_bytes_dirty;
    int64_t cache_bytes_dirty_internal;
    int64_t cache_bytes_dirty_leaf;
    int64_t cache_pages_dirty;
    int64_t cache_eviction_blocked_uncommitted_truncate;
    int64_t cache_eviction_clean;
    int64_t cache_bytes_hs_updates;
    int64_t cache_updates_txn_uncommitted_bytes;
    int64_t cache_updates_txn_uncommitted_count;
    int64_t fsync_all_fh_total;
    int64_t fsync_all_fh;
    int64_t fsync_all_time;
    int64_t capacity_bytes_read;
    int64_t capacity_bytes_ckpt;
    int64_t capacity_bytes_chunkcache;
    int64_t capacity_bytes_evict;
    int64_t capacity_bytes_log;
    int64_t capacity_bytes_written;
    int64_t capacity_threshold;
    int64_t capacity_time_total;
    int64_t capacity_time_ckpt;
    int64_t capacity_time_evict;
    int64_t capacity_time_log;
    int64_t capacity_time_read;
    int64_t capacity_time_chunkcache;
    int64_t checkpoint_cleanup_success;
    int64_t checkpoint_snapshot_acquired;
    int64_t checkpoint_skipped;
    int64_t checkpoint_fsync_post;
    int64_t checkpoint_fsync_post_duration;
    int64_t checkpoint_generation;
    int64_t checkpoint_time_max;
    int64_t checkpoint_time_min;
    int64_t checkpoint_handle_drop_duration;
    int64_t checkpoint_handle_duration;
    int64_t checkpoint_handle_apply_duration;
    int64_t checkpoint_handle_skip_duration;
    int64_t checkpoint_handle_meta_check_duration;
    int64_t checkpoint_handle_lock_duration;
    int64_t checkpoint_handle_applied;
    int64_t checkpoint_handle_dropped;
    int64_t checkpoint_handle_meta_checked;
    int64_t checkpoint_handle_locked;
    int64_t checkpoint_handle_skipped;
    int64_t checkpoint_handle_walked;
    int64_t checkpoint_time_recent;
    int64_t checkpoint_pages_reconciled_bytes;
    int64_t checkpoints_api;
    int64_t checkpoints_compact;
    int64_t checkpoint_sync;
    int64_t checkpoint_presync;
    int64_t checkpoint_hs_pages_reconciled;
    int64_t checkpoint_pages_visited_internal;
    int64_t checkpoint_pages_visited_leaf;
    int64_t checkpoint_pages_reconciled;
    int64_t checkpoint_cleanup_pages_evict;
    int64_t checkpoint_cleanup_pages_obsolete_tw;
    int64_t checkpoint_cleanup_pages_read_reclaim_space;
    int64_t checkpoint_cleanup_pages_read_obsolete_tw;
    int64_t checkpoint_cleanup_pages_removed;
    int64_t checkpoint_cleanup_pages_walk_skipped;
    int64_t checkpoint_cleanup_pages_visited;
    int64_t checkpoint_prep_running;
    int64_t checkpoint_prep_max;
    int64_t checkpoint_prep_min;
    int64_t checkpoint_prep_recent;
    int64_t checkpoint_prep_total;
    int64_t checkpoint_state;
    int64_t checkpoint_scrub_target;
    int64_t checkpoint_scrub_max;
    int64_t checkpoint_scrub_min;
    int64_t checkpoint_scrub_recent;
    int64_t checkpoint_scrub_total;
    int64_t checkpoint_stop_stress_active;
    int64_t checkpoint_tree_duration;
    int64_t checkpoints_total_failed;
    int64_t checkpoints_total_succeed;
    int64_t checkpoint_time_total;
    int64_t checkpoint_wait_reduce_dirty;
    int64_t chunkcache_spans_chunks_read;
    int64_t chunkcache_chunks_evicted;
    int64_t chunkcache_exceeded_bitmap_capacity;
    int64_t chunkcache_exceeded_capacity;
    int64_t chunkcache_lookups;
    int64_t chunkcache_chunks_loaded_from_flushed_tables;
    int64_t chunkcache_metadata_inserted;
    int64_t chunkcache_metadata_removed;
    int64_t chunkcache_metadata_work_units_dropped;
    int64_t chunkcache_metadata_work_units_created;
    int64_t chunkcache_metadata_work_units_dequeued;
    int64_t chunkcache_misses;
    int64_t chunkcache_io_failed;
    int64_t chunkcache_retries;
    int64_t chunkcache_retries_checksum_mismatch;
    int64_t chunkcache_toomany_retries;
    int64_t chunkcache_bytes_read_persistent;
    int64_t chunkcache_bytes_inuse;
    int64_t chunkcache_bytes_inuse_pinned;
    int64_t chunkcache_chunks_inuse;
    int64_t chunkcache_created_from_metadata;
    int64_t chunkcache_chunks_pinned;
    int64_t cond_auto_wait_reset;
    int64_t cond_auto_wait;
    int64_t cond_auto_wait_skipped;
    int64_t time_travel;
    int64_t file_open;
    int64_t buckets_dh;
    int64_t buckets;
    int64_t memory_allocation;
    int64_t memory_free;
    int64_t memory_grow;
    int64_t no_session_sweep_5min;
    int64_t no_session_sweep_60min;
    int64_t cond_wait;
    int64_t rwlock_read;
    int64_t rwlock_write;
    int64_t fsync_io;
    int64_t read_io;
    int64_t write_io;
    int64_t cursor_tree_walk_del_page_skip;
    int64_t cursor_next_skip_total;
    int64_t cursor_prev_skip_total;
    int64_t cursor_skip_hs_cur_position;
    int64_t cursor_tree_walk_inmem_del_page_skip;
    int64_t cursor_tree_walk_ondisk_del_page_skip;
    int64_t cursor_search_near_prefix_fast_paths;
    int64_t cursor_reposition_failed;
    int64_t cursor_reposition;
    int64_t cursor_bulk_count;
    int64_t cursor_cached_count;
    int64_t cursor_bound_error;
    int64_t cursor_bounds_reset;
    int64_t cursor_bounds_comparisons;
    int64_t cursor_bounds_next_unpositioned;
    int64_t cursor_bounds_next_early_exit;
    int64_t cursor_bounds_prev_unpositioned;
    int64_t cursor_bounds_prev_early_exit;
    int64_t cursor_bounds_search_early_exit;
    int64_t cursor_bounds_search_near_repositioned_cursor;
    int64_t cursor_insert_bulk;
    int64_t cursor_cache_error;
    int64_t cursor_cache;
    int64_t cursor_close_error;
    int64_t cursor_compare_error;
    int64_t cursor_create;
    int64_t cursor_equals_error;
    int64_t cursor_get_key_error;
    int64_t cursor_get_value_error;
    int64_t cursor_insert;
    int64_t cursor_insert_error;
    int64_t cursor_insert_check_error;
    int64_t cursor_insert_bytes;
    int64_t cursor_largest_key_error;
    int64_t cursor_modify;
    int64_t cursor_modify_error;
    int64_t cursor_modify_bytes;
    int64_t cursor_modify_bytes_touch;
    int64_t cursor_next;
    int64_t cursor_next_error;
    int64_t cursor_next_hs_tombstone;
    int64_t cursor_next_skip_lt_100;
    int64_t cursor_next_skip_ge_100;
    int64_t cursor_next_random_error;
    int64_t cursor_restart;
    int64_t cursor_prev;
    int64_t cursor_prev_error;
    int64_t cursor_prev_hs_tombstone;
    int64_t cursor_prev_skip_ge_100;
    int64_t cursor_prev_skip_lt_100;
    int64_t cursor_reconfigure_error;
    int64_t cursor_remove;
    int64_t cursor_remove_error;
    int64_t cursor_remove_bytes;
    int64_t cursor_reopen_error;
    int64_t cursor_reserve;
    int64_t cursor_reserve_error;
    int64_t cursor_reset;
    int64_t cursor_reset_error;
    int64_t cursor_search;
    int64_t cursor_search_error;
    int64_t cursor_search_hs;
    int64_t cursor_search_near;
    int64_t cursor_search_near_error;
    int64_t cursor_sweep_buckets;
    int64_t cursor_sweep_closed;
    int64_t cursor_sweep_examined;
    int64_t cursor_sweep;
    int64_t cursor_truncate;
    int64_t cursor_truncate_keys_deleted;
    int64_t cursor_update;
    int64_t cursor_update_error;
    int64_t cursor_update_bytes;
    int64_t cursor_update_bytes_changed;
    int64_t cursor_reopen;
    int64_t cursor_open_count;
    int64_t dh_conn_handle_table_count;
    int64_t dh_conn_handle_tiered_count;
    int64_t dh_conn_handle_tiered_tree_count;
    int64_t dh_conn_handle_btree_count;
    int64_t dh_conn_handle_checkpoint_count;
    int64_t dh_conn_handle_size;
    int64_t dh_conn_handle_count;
    int64_t dh_sweep_ref;
    int64_t dh_sweep_dead_close;
    int64_t dh_sweep_remove;
    int64_t dh_sweep_expired_close;
    int64_t dh_sweep_tod;
    int64_t dh_sweeps;
    int64_t dh_sweep_skip_ckpt;
    int64_t dh_session_handles;
    int64_t dh_session_sweeps;
    int64_t disagg_role_leader;
    int64_t layered_curs_insert;
    int64_t layered_curs_next;
    int64_t layered_curs_next_ingest;
    int64_t layered_curs_next_stable;
    int64_t layered_curs_prev;
    int64_t layered_curs_prev_ingest;
    int64_t layered_curs_prev_stable;
    int64_t layered_curs_remove;
    int64_t layered_curs_search_near;
    int64_t layered_curs_search_near_ingest;
    int64_t layered_curs_search_near_stable;
    int64_t layered_curs_search;
    int64_t layered_curs_search_ingest;
    int64_t layered_curs_search_stable;
    int64_t layered_curs_update;
    int64_t layered_curs_upgrade_ingest;
    int64_t layered_curs_upgrade_stable;
    int64_t layered_table_manager_checkpoints;
    int64_t layered_table_manager_checkpoints_refreshed;
    int64_t layered_table_manager_logops_applied;
    int64_t layered_table_manager_logops_skipped;
    int64_t layered_table_manager_skip_lsn;
    int64_t layered_table_manager_tables;
    int64_t layered_table_manager_running;
    int64_t layered_table_manager_active;
    int64_t live_restore_bytes_copied;
    int64_t live_restore_work_remaining;
    int64_t live_restore_source_read_count;
    int64_t live_restore_hist_source_read_latency_lt2;
    int64_t live_restore_hist_source_read_latency_lt5;
    int64_t live_restore_hist_source_read_latency_lt10;
    int64_t live_restore_hist_source_read_latency_lt50;
    int64_t live_restore_hist_source_read_latency_lt100;
    int64_t live_restore_hist_source_read_latency_lt250;
    int64_t live_restore_hist_source_read_latency_lt500;
    int64_t live_restore_hist_source_read_latency_lt1000;
    int64_t live_restore_hist_source_read_latency_gt1000;
    int64_t live_restore_hist_source_read_latency_total_msecs;
    int64_t live_restore_state;
    int64_t lock_btree_page_count;
    int64_t lock_btree_page_wait_application;
    int64_t lock_btree_page_wait_internal;
    int64_t lock_checkpoint_count;
    int64_t lock_checkpoint_wait_application;
    int64_t lock_checkpoint_wait_internal;
    int64_t lock_dhandle_wait_application;
    int64_t lock_dhandle_wait_internal;
    int64_t lock_dhandle_read_count;
    int64_t lock_dhandle_write_count;
    int64_t lock_metadata_count;
    int64_t lock_metadata_wait_application;
    int64_t lock_metadata_wait_internal;
    int64_t lock_schema_count;
    int64_t lock_schema_wait_application;
    int64_t lock_schema_wait_internal;
    int64_t lock_table_wait_application;
    int64_t lock_table_wait_internal;
    int64_t lock_table_read_count;
    int64_t lock_table_write_count;
    int64_t lock_txn_global_wait_application;
    int64_t lock_txn_global_wait_internal;
    int64_t lock_txn_global_read_count;
    int64_t lock_txn_global_write_count;
    int64_t log_slot_switch_busy;
    int64_t log_force_remove_sleep;
    int64_t log_bytes_payload;
    int64_t log_bytes_written;
    int64_t log_zero_fills;
    int64_t log_flush;
    int64_t log_force_write;
    int64_t log_force_write_skip;
    int64_t log_compress_writes;
    int64_t log_compress_write_fails;
    int64_t log_compress_small;
    int64_t log_release_write_lsn;
    int64_t log_scans;
    int64_t log_scan_rereads;
    int64_t log_write_lsn;
    int64_t log_write_lsn_skip;
    int64_t log_sync;
    int64_t log_sync_duration;
    int64_t log_sync_dir;
    int64_t log_sync_dir_duration;
    int64_t log_writes;
    int64_t log_slot_consolidated;
    int64_t log_max_filesize;
    int64_t log_prealloc_max;
    int64_t log_prealloc_missed;
    int64_t log_prealloc_files;
    int64_t log_prealloc_used;
    int64_t log_scan_records;
    int64_t log_slot_close_race;
    int64_t log_slot_close_unbuf;
    int64_t log_slot_closes;
    int64_t log_slot_races;
    int64_t log_slot_yield_race;
    int64_t log_slot_immediate;
    int64_t log_slot_yield_close;
    int64_t log_slot_yield_sleep;
    int64_t log_slot_yield;
    int64_t log_slot_active_closed;
    int64_t log_slot_yield_duration;
    int64_t log_slot_no_free_slots;
    int64_t log_slot_unbuffered;
    int64_t log_compress_mem;
    int64_t log_buffer_size;
    int64_t log_compress_len;
    int64_t log_slot_coalesced;
    int64_t log_close_yields;
    int64_t perf_hist_bmread_latency_lt2;
    int64_t perf_hist_bmread_latency_lt5;
    int64_t perf_hist_bmread_latency_lt10;
    int64_t perf_hist_bmread_latency_lt50;
    int64_t perf_hist_bmread_latency_lt100;
    int64_t perf_hist_bmread_latency_lt250;
    int64_t perf_hist_bmread_latency_lt500;
    int64_t perf_hist_bmread_latency_lt1000;
    int64_t perf_hist_bmread_latency_gt1000;
    int64_t perf_hist_bmread_latency_total_msecs;
    int64_t perf_hist_bmwrite_latency_lt2;
    int64_t perf_hist_bmwrite_latency_lt5;
    int64_t perf_hist_bmwrite_latency_lt10;
    int64_t perf_hist_bmwrite_latency_lt50;
    int64_t perf_hist_bmwrite_latency_lt100;
    int64_t perf_hist_bmwrite_latency_lt250;
    int64_t perf_hist_bmwrite_latency_lt500;
    int64_t perf_hist_bmwrite_latency_lt1000;
    int64_t perf_hist_bmwrite_latency_gt1000;
    int64_t perf_hist_bmwrite_latency_total_msecs;
    int64_t perf_hist_disaggbmread_latency_lt100;
    int64_t perf_hist_disaggbmread_latency_lt250;
    int64_t perf_hist_disaggbmread_latency_lt500;
    int64_t perf_hist_disaggbmread_latency_lt1000;
    int64_t perf_hist_disaggbmread_latency_lt2500;
    int64_t perf_hist_disaggbmread_latency_lt5000;
    int64_t perf_hist_disaggbmread_latency_lt10000;
    int64_t perf_hist_disaggbmread_latency_gt10000;
    int64_t perf_hist_disaggbmread_latency_total_usecs;
    int64_t perf_hist_disaggbmwrite_latency_lt100;
    int64_t perf_hist_disaggbmwrite_latency_lt250;
    int64_t perf_hist_disaggbmwrite_latency_lt500;
    int64_t perf_hist_disaggbmwrite_latency_lt1000;
    int64_t perf_hist_disaggbmwrite_latency_lt2500;
    int64_t perf_hist_disaggbmwrite_latency_lt5000;
    int64_t perf_hist_disaggbmwrite_latency_lt10000;
    int64_t perf_hist_disaggbmwrite_latency_gt10000;
    int64_t perf_hist_disaggbmwrite_latency_total_usecs;
    int64_t perf_hist_fsread_latency_lt2;
    int64_t perf_hist_fsread_latency_lt5;
    int64_t perf_hist_fsread_latency_lt10;
    int64_t perf_hist_fsread_latency_lt50;
    int64_t perf_hist_fsread_latency_lt100;
    int64_t perf_hist_fsread_latency_lt250;
    int64_t perf_hist_fsread_latency_lt500;
    int64_t perf_hist_fsread_latency_lt1000;
    int64_t perf_hist_fsread_latency_gt1000;
    int64_t perf_hist_fsread_latency_total_msecs;
    int64_t perf_hist_fswrite_latency_lt2;
    int64_t perf_hist_fswrite_latency_lt5;
    int64_t perf_hist_fswrite_latency_lt10;
    int64_t perf_hist_fswrite_latency_lt50;
    int64_t perf_hist_fswrite_latency_lt100;
    int64_t perf_hist_fswrite_latency_lt250;
    int64_t perf_hist_fswrite_latency_lt500;
    int64_t perf_hist_fswrite_latency_lt1000;
    int64_t perf_hist_fswrite_latency_gt1000;
    int64_t perf_hist_fswrite_latency_total_msecs;
    int64_t perf_hist_internal_reconstruct_latency_lt100;
    int64_t perf_hist_internal_reconstruct_latency_lt250;
    int64_t perf_hist_internal_reconstruct_latency_lt500;
    int64_t perf_hist_internal_reconstruct_latency_lt1000;
    int64_t perf_hist_internal_reconstruct_latency_lt2500;
    int64_t perf_hist_internal_reconstruct_latency_lt5000;
    int64_t perf_hist_internal_reconstruct_latency_lt10000;
    int64_t perf_hist_internal_reconstruct_latency_gt10000;
    int64_t perf_hist_internal_reconstruct_latency_total_usecs;
    int64_t perf_hist_leaf_reconstruct_latency_lt100;
    int64_t perf_hist_leaf_reconstruct_latency_lt250;
    int64_t perf_hist_leaf_reconstruct_latency_lt500;
    int64_t perf_hist_leaf_reconstruct_latency_lt1000;
    int64_t perf_hist_leaf_reconstruct_latency_lt2500;
    int64_t perf_hist_leaf_reconstruct_latency_lt5000;
    int64_t perf_hist_leaf_reconstruct_latency_lt10000;
    int64_t perf_hist_leaf_reconstruct_latency_gt10000;
    int64_t perf_hist_leaf_reconstruct_latency_total_usecs;
    int64_t perf_hist_opread_latency_lt100;
    int64_t perf_hist_opread_latency_lt250;
    int64_t perf_hist_opread_latency_lt500;
    int64_t perf_hist_opread_latency_lt1000;
    int64_t perf_hist_opread_latency_lt2500;
    int64_t perf_hist_opread_latency_lt5000;
    int64_t perf_hist_opread_latency_lt10000;
    int64_t perf_hist_opread_latency_gt10000;
    int64_t perf_hist_opread_latency_total_usecs;
    int64_t perf_hist_opwrite_latency_lt100;
    int64_t perf_hist_opwrite_latency_lt250;
    int64_t perf_hist_opwrite_latency_lt500;
    int64_t perf_hist_opwrite_latency_lt1000;
    int64_t perf_hist_opwrite_latency_lt2500;
    int64_t perf_hist_opwrite_latency_lt5000;
    int64_t perf_hist_opwrite_latency_lt10000;
    int64_t perf_hist_opwrite_latency_gt10000;
    int64_t perf_hist_opwrite_latency_total_usecs;
    int64_t prefetch_skipped_internal_page;
    int64_t prefetch_skipped_no_flag_set;
    int64_t prefetch_failed_start;
    int64_t prefetch_skipped_same_ref;
    int64_t prefetch_disk_one;
    int64_t prefetch_skipped_no_valid_dhandle;
    int64_t prefetch_skipped;
    int64_t prefetch_skipped_disk_read_count;
    int64_t prefetch_skipped_internal_session;
    int64_t prefetch_skipped_special_handle;
    int64_t prefetch_pages_fail;
    int64_t prefetch_pages_queued;
    int64_t prefetch_pages_read;
    int64_t prefetch_skipped_error_ok;
    int64_t prefetch_attempts;
    int64_t rec_vlcs_emptied_pages;
    int64_t rec_time_window_bytes_ts;
    int64_t rec_time_window_bytes_txn;
    int64_t rec_average_internal_page_delta_chain_length;
    int64_t rec_average_leaf_page_delta_chain_length;
    int64_t rec_page_mods_le5;
    int64_t rec_page_mods_le10;
    int64_t rec_page_mods_le20;
    int64_t rec_page_mods_le50;
    int64_t rec_page_mods_le100;
    int64_t rec_page_mods_le200;
    int64_t rec_page_mods_le500;
    int64_t rec_page_mods_gt500;
    int64_t rec_hs_wrapup_next_prev_calls;
    int64_t rec_skip_empty_deltas;
    int64_t rec_page_delete_fast;
    int64_t rec_page_full_image_internal;
    int64_t rec_page_full_image_leaf;
    int64_t rec_page_delta_internal;
    int64_t rec_multiblock_internal;
    int64_t rec_page_delta_leaf;
    int64_t rec_multiblock_leaf;
    int64_t rec_overflow_key_leaf;
    int64_t rec_max_internal_page_deltas;
    int64_t rec_max_leaf_page_deltas;
    int64_t rec_maximum_milliseconds;
    int64_t rec_maximum_image_build_milliseconds;
    int64_t rec_maximum_hs_wrapup_milliseconds;
    int64_t rec_ingest_garbage_collection_keys;
    int64_t rec_overflow_value;
    int64_t rec_pages;
    int64_t rec_pages_eviction;
    int64_t rec_pages_size_1MB_to_10MB;
    int64_t rec_pages_size_10MB_to_100MB;
    int64_t rec_pages_size_100MB_to_1GB;
    int64_t rec_pages_size_1GB_plus;
    int64_t rec_pages_with_prepare;
    int64_t rec_pages_with_ts;
    int64_t rec_pages_with_txn;
    int64_t rec_page_delete;
    int64_t rec_time_aggr_newest_start_durable_ts;
    int64_t rec_time_aggr_newest_stop_durable_ts;
    int64_t rec_time_aggr_newest_stop_ts;
    int64_t rec_time_aggr_newest_stop_txn;
    int64_t rec_time_aggr_newest_txn;
    int64_t rec_time_aggr_oldest_start_ts;
    int64_t rec_time_aggr_prepared;
    int64_t rec_time_window_pages_prepared;
    int64_t rec_time_window_pages_durable_start_ts;
    int64_t rec_time_window_pages_start_ts;
    int64_t rec_time_window_pages_start_txn;
    int64_t rec_time_window_pages_durable_stop_ts;
    int64_t rec_time_window_pages_stop_ts;
    int64_t rec_time_window_pages_stop_txn;
    int64_t rec_pages_with_internal_deltas;
    int64_t rec_pages_with_leaf_deltas;
    int64_t rec_time_window_prepared;
    int64_t rec_time_window_durable_start_ts;
    int64_t rec_time_window_start_ts;
    int64_t rec_time_window_start_txn;
    int64_t rec_time_window_durable_stop_ts;
    int64_t rec_time_window_stop_ts;
    int64_t rec_time_window_stop_txn;
    int64_t rec_split_stashed_bytes;
    int64_t rec_split_stashed_objects;
    int64_t local_objects_inuse;
    int64_t flush_tier_fail;
    int64_t flush_tier;
    int64_t flush_tier_skipped;
    int64_t flush_tier_switched;
    int64_t local_objects_removed;
    int64_t session_open;
    int64_t session_query_ts;
    int64_t session_table_alter_fail;
    int64_t session_table_alter_success;
    int64_t session_table_alter_trigger_checkpoint;
    int64_t session_table_alter_skip;
    int64_t session_table_compact_conflicting_checkpoint;
    int64_t session_table_compact_dhandle_success;
    int64_t session_table_compact_fail;
    int64_t session_table_compact_fail_cache_pressure;
    int64_t session_table_compact_passes;
    int64_t session_table_compact_eviction;
    int64_t session_table_compact_running;
    int64_t session_table_compact_skipped;
    int64_t session_table_compact_success;
    int64_t session_table_compact_timeout;
    int64_t session_table_create_fail;
    int64_t session_table_create_success;
    int64_t session_table_create_import_fail;
    int64_t session_table_create_import_repair;
    int64_t session_table_create_import_success;
    int64_t session_table_drop_fail;
    int64_t session_table_drop_success;
    int64_t session_table_salvage_fail;
    int64_t session_table_salvage_success;
    int64_t session_table_truncate_fail;
    int64_t session_table_truncate_success;
    int64_t session_table_verify_fail;
    int64_t session_table_verify_success;
    int64_t tiered_work_units_dequeued;
    int64_t tiered_work_units_removed;
    int64_t tiered_work_units_created;
    int64_t tiered_retention;
    int64_t thread_fsync_active;
    int64_t thread_read_active;
    int64_t thread_write_active;
    int64_t application_cache_ops;
    int64_t application_cache_interruptible_ops;
    int64_t application_cache_uninterruptible_ops;
    int64_t application_evict_snapshot_refreshed;
    int64_t application_cache_time;
    int64_t application_cache_interruptible_time;
    int64_t application_cache_uninterruptible_time;
    int64_t txn_release_blocked;
    int64_t dhandle_lock_blocked;
    int64_t page_index_slot_ref_blocked;
    int64_t prepared_transition_blocked_page;
    int64_t page_busy_blocked;
    int64_t page_forcible_evict_blocked;
    int64_t page_locked_blocked;
    int64_t page_read_blocked;
    int64_t page_sleep;
    int64_t page_del_rollback_blocked;
    int64_t child_modify_blocked_page;
    int64_t page_split_restart;
    int64_t page_read_skip_deleted;
    int64_t txn_prepared_updates;
    int64_t txn_prepared_updates_committed;
    int64_t txn_prepared_updates_key_repeated;
    int64_t txn_prepared_updates_rolledback;
    int64_t txn_read_race_prepare_commit;
    int64_t txn_read_overflow_remove;
    int64_t txn_rollback_oldest_pinned;
    int64_t txn_rollback_oldest_id;
    int64_t txn_prepare;
    int64_t txn_prepare_commit;
    int64_t txn_prepare_active;
    int64_t txn_prepare_rollback;
    int64_t txn_query_ts;
    int64_t txn_read_race_prepare_update;
    int64_t txn_rts;
    int64_t txn_rts_sweep_hs_keys_dryrun;
    int64_t txn_rts_hs_stop_older_than_newer_start;
    int64_t txn_rts_inconsistent_ckpt;
    int64_t txn_rts_keys_removed;
    int64_t txn_rts_keys_restored;
    int64_t txn_rts_keys_removed_dryrun;
    int64_t txn_rts_keys_restored_dryrun;
    int64_t txn_rts_pages_visited;
    int64_t txn_rts_hs_restore_tombstones;
    int64_t txn_rts_hs_restore_updates;
    int64_t txn_rts_delete_rle_skipped;
    int64_t txn_rts_stable_rle_skipped;
    int64_t txn_rts_sweep_hs_keys;
    int64_t txn_rts_hs_restore_tombstones_dryrun;
    int64_t txn_rts_tree_walk_skip_pages;
    int64_t txn_rts_upd_aborted;
    int64_t txn_rts_hs_restore_updates_dryrun;
    int64_t txn_rts_hs_removed;
    int64_t txn_rts_upd_aborted_dryrun;
    int64_t txn_rts_hs_removed_dryrun;
    int64_t txn_sessions_walked;
    int64_t txn_set_ts;
    int64_t txn_set_ts_durable;
    int64_t txn_set_ts_durable_upd;
    int64_t txn_set_ts_force;
    int64_t txn_set_ts_out_of_order;
    int64_t txn_set_ts_oldest;
    int64_t txn_set_ts_oldest_upd;
    int64_t txn_set_ts_stable;
    int64_t txn_set_ts_stable_upd;
    int64_t txn_begin;
    int64_t txn_hs_ckpt_duration;
    int64_t txn_pinned_range;
    int64_t txn_pinned_checkpoint_range;
    int64_t txn_pinned_timestamp;
    int64_t txn_pinned_timestamp_checkpoint;
    int64_t txn_pinned_timestamp_reader;
    int64_t txn_pinned_timestamp_oldest;
    int64_t txn_timestamp_oldest_active_read;
    int64_t txn_rollback_to_stable_running;
    int64_t txn_walk_sessions;
    int64_t txn_commit;
    int64_t txn_rollback;
    int64_t txn_update_conflict;
};

/*
 * Statistics entries for data sources.
 */
#define WT_DSRC_STATS_BASE 2000
struct __wt_dsrc_stats {
    int64_t autocommit_readonly_retry;
    int64_t autocommit_update_retry;
    int64_t backup_blocks_compressed;
    int64_t backup_blocks_uncompressed;
    int64_t disagg_block_hs_byte_read;
    int64_t disagg_block_hs_byte_write;
    int64_t disagg_block_get;
    int64_t disagg_block_hs_get;
    int64_t disagg_block_page_discard;
    int64_t disagg_block_put;
    int64_t disagg_block_hs_put;
    int64_t block_extension;
    int64_t block_alloc;
    int64_t block_free;
    int64_t block_checkpoint_size;
    int64_t allocation_size;
    int64_t block_reuse_bytes;
    int64_t block_magic;
    int64_t block_major;
    int64_t block_size;
    int64_t block_minor;
    int64_t btree_checkpoint_generation;
    int64_t btree_clean_checkpoint_timer;
    int64_t btree_compact_pages_reviewed;
    int64_t btree_compact_pages_rewritten;
    int64_t btree_compact_pages_skipped;
    int64_t btree_compact_bytes_rewritten_expected;
    int64_t btree_compact_pages_rewritten_expected;
    int64_t btree_checkpoint_pages_reconciled;
    int64_t btree_compact_skipped;
    int64_t btree_column_fix;
    int64_t btree_column_tws;
    int64_t btree_column_internal;
    int64_t btree_column_rle;
    int64_t btree_column_deleted;
    int64_t btree_column_variable;
    int64_t btree_fixed_len;
    int64_t btree_maxintlpage;
    int64_t btree_maxleafkey;
    int64_t btree_maxleafpage;
    int64_t btree_maxleafvalue;
    int64_t btree_maximum_depth;
    int64_t btree_entries;
    int64_t btree_overflow;
    int64_t btree_row_empty_values;
    int64_t btree_row_internal;
    int64_t btree_row_leaf;
    int64_t cache_eviction_app_threads_fill_ratio_lt_25;
    int64_t cache_eviction_app_threads_fill_ratio_25_50;
    int64_t cache_eviction_app_threads_fill_ratio_50_75;
    int64_t cache_eviction_app_threads_fill_ratio_gt_75;
    int64_t cache_bytes_inuse;
    int64_t cache_bytes_dirty_total;
    int64_t cache_bytes_read;
    int64_t cache_bytes_write;
    int64_t cache_eviction_blocked_checkpoint;
    int64_t cache_eviction_blocked_checkpoint_hs;
    int64_t eviction_fail;
    int64_t cache_eviction_blocked_disagg_dirty_internal_page;
    int64_t cache_eviction_blocked_no_ts_checkpoint_race_1;
    int64_t cache_eviction_blocked_no_ts_checkpoint_race_2;
    int64_t cache_eviction_blocked_no_ts_checkpoint_race_3;
    int64_t cache_eviction_blocked_no_ts_checkpoint_race_4;
    int64_t cache_eviction_blocked_remove_hs_race_with_checkpoint;
    int64_t cache_eviction_blocked_no_progress;
    int64_t cache_eviction_pages_queued_updates;
    int64_t cache_eviction_pages_queued_clean;
    int64_t cache_eviction_pages_queued_dirty;
    int64_t cache_eviction_pages_seen_updates;
    int64_t cache_eviction_pages_seen_clean;
    int64_t cache_eviction_pages_seen_dirty;
    int64_t eviction_walk_passes;
    int64_t cache_eviction_target_page_lt10;
    int64_t cache_eviction_target_page_lt32;
    int64_t cache_eviction_target_page_ge128;
    int64_t cache_eviction_target_page_lt64;
    int64_t cache_eviction_target_page_lt128;
    int64_t cache_eviction_target_page_reduced;
    int64_t cache_eviction_blocked_hazard;
    int64_t cache_eviction_hs_cursor_not_cached;
    int64_t cache_hs_insert;
    int64_t cache_hs_insert_restart;
    int64_t cache_hs_read;
    int64_t cache_hs_read_miss;
    int64_t cache_hs_read_squash;
    int64_t cache_hs_order_lose_durable_timestamp;
    int64_t cache_hs_key_truncate_rts_unstable;
    int64_t cache_hs_key_truncate_rts;
    int64_t cache_hs_btree_truncate;
    int64_t cache_hs_key_truncate;
    int64_t cache_hs_order_remove;
    int64_t cache_hs_key_truncate_onpage_removal;
    int64_t cache_hs_btree_truncate_dryrun;
    int64_t cache_hs_key_truncate_rts_unstable_dryrun;
    int64_t cache_hs_key_truncate_rts_dryrun;
    int64_t cache_hs_order_reinsert;
    int64_t cache_hs_write_squash;
    int64_t cache_inmem_splittable;
    int64_t cache_inmem_split;
    int64_t cache_eviction_blocked_internal_page_split;
    int64_t cache_eviction_internal;
    int64_t cache_eviction_split_internal;
    int64_t cache_eviction_split_leaf;
    int64_t cache_eviction_random_sample_inmem_root;
    int64_t cache_eviction_dirty;
    int64_t cache_eviction_blocked_multi_block_reconciliation_during_checkpoint;
    int64_t cache_read_internal_delta;
    int64_t cache_read_flatten_leaf_delta;
    int64_t cache_read_flatten_leaf_delta_fail;
    int64_t cache_read_leaf_delta;
    int64_t cache_eviction_trigger_dirty_reached;
    int64_t cache_eviction_trigger_reached;
    int64_t cache_eviction_trigger_updates_reached;
    int64_t cache_eviction_blocked_overflow_keys;
    int64_t cache_read_overflow;
    int64_t cache_eviction_blocked_materialization;
    int64_t cache_eviction_blocked_disagg_next_checkpoint;
    int64_t cache_eviction_deepen;
    int64_t cache_write_hs;
    int64_t cache_eviction_dirty_obsolete_tw;
    int64_t cache_eviction_ahead_of_last_materialized_lsn;
    int64_t cache_read;
    int64_t cache_read_deleted;
    int64_t cache_read_deleted_prepared;
    int64_t cache_read_checkpoint;
    int64_t cache_pages_requested;
    int64_t cache_pages_prefetch;
    int64_t cache_pages_requested_internal;
    int64_t cache_pages_requested_leaf;
    int64_t cache_eviction_pages_seen;
    int64_t cache_write;
    int64_t cache_write_restore;
    int64_t cache_eviction_blocked_precise_checkpoint;
    int64_t cache_evict_split_failed_lock;
    int64_t cache_eviction_blocked_recently_modified;
    int64_t cache_scrub_restore;
    int64_t cache_reverse_splits;
    int64_t cache_reverse_splits_skipped_vlcs;
    int64_t cache_eviction_hs_shared_cursor_not_cached;
    int64_t cache_read_delta_updates;
    int64_t cache_read_restored_tombstone_bytes;
    int64_t cache_hs_insert_full_update;
    int64_t cache_hs_insert_reverse_modify;
    int64_t cache_bytes_dirty;
    int64_t cache_bytes_dirty_internal;
    int64_t cache_bytes_dirty_leaf;
    int64_t cache_eviction_blocked_uncommitted_truncate;
    int64_t cache_eviction_clean;
    int64_t cache_state_gen_avg_gap;
    int64_t cache_state_avg_written_size;
    int64_t cache_state_avg_visited_age;
    int64_t cache_state_avg_unvisited_age;
    int64_t cache_state_pages_clean;
    int64_t cache_state_gen_current;
    int64_t cache_state_pages_dirty;
    int64_t cache_state_root_entries;
    int64_t cache_state_pages_internal;
    int64_t cache_state_pages_leaf;
    int64_t cache_state_gen_max_gap;
    int64_t cache_state_max_pagesize;
    int64_t cache_state_min_written_size;
    int64_t cache_state_unvisited_count;
    int64_t cache_state_smaller_alloc_size;
    int64_t cache_state_memory;
    int64_t cache_state_queued;
    int64_t cache_state_not_queueable;
    int64_t cache_state_refs_skipped;
    int64_t cache_state_root_size;
    int64_t cache_state_pages;
    int64_t checkpoint_snapshot_acquired;
    int64_t checkpoint_cleanup_pages_evict;
    int64_t checkpoint_cleanup_pages_obsolete_tw;
    int64_t checkpoint_cleanup_pages_read_reclaim_space;
    int64_t checkpoint_cleanup_pages_read_obsolete_tw;
    int64_t checkpoint_cleanup_pages_removed;
    int64_t checkpoint_cleanup_pages_walk_skipped;
    int64_t checkpoint_cleanup_pages_visited;
    int64_t compress_precomp_intl_max_page_size;
    int64_t compress_precomp_leaf_max_page_size;
    int64_t compress_write_fail;
    int64_t compress_write_too_small;
    int64_t compress_read;
    int64_t compress_read_ratio_hist_max;
    int64_t compress_read_ratio_hist_2;
    int64_t compress_read_ratio_hist_4;
    int64_t compress_read_ratio_hist_8;
    int64_t compress_read_ratio_hist_16;
    int64_t compress_read_ratio_hist_32;
    int64_t compress_read_ratio_hist_64;
    int64_t compress_write;
    int64_t compress_write_ratio_hist_max;
    int64_t compress_write_ratio_hist_2;
    int64_t compress_write_ratio_hist_4;
    int64_t compress_write_ratio_hist_8;
    int64_t compress_write_ratio_hist_16;
    int64_t compress_write_ratio_hist_32;
    int64_t compress_write_ratio_hist_64;
    int64_t cursor_tree_walk_del_page_skip;
    int64_t cursor_next_skip_total;
    int64_t cursor_prev_skip_total;
    int64_t cursor_skip_hs_cur_position;
    int64_t cursor_tree_walk_inmem_del_page_skip;
    int64_t cursor_tree_walk_ondisk_del_page_skip;
    int64_t cursor_search_near_prefix_fast_paths;
    int64_t cursor_reposition_failed;
    int64_t cursor_reposition;
    int64_t cursor_insert_bulk;
    int64_t cursor_reopen;
    int64_t cursor_cache;
    int64_t cursor_create;
    int64_t cursor_bound_error;
    int64_t cursor_bounds_reset;
    int64_t cursor_bounds_comparisons;
    int64_t cursor_bounds_next_unpositioned;
    int64_t cursor_bounds_next_early_exit;
    int64_t cursor_bounds_prev_unpositioned;
    int64_t cursor_bounds_prev_early_exit;
    int64_t cursor_bounds_search_early_exit;
    int64_t cursor_bounds_search_near_repositioned_cursor;
    int64_t cursor_cache_error;
    int64_t cursor_close_error;
    int64_t cursor_compare_error;
    int64_t cursor_equals_error;
    int64_t cursor_get_key_error;
    int64_t cursor_get_value_error;
    int64_t cursor_insert_error;
    int64_t cursor_insert_check_error;
    int64_t cursor_largest_key_error;
    int64_t cursor_modify_error;
    int64_t cursor_next_error;
    int64_t cursor_next_hs_tombstone;
    int64_t cursor_next_skip_lt_100;
    int64_t cursor_next_skip_ge_100;
    int64_t cursor_next_random_error;
    int64_t cursor_prev_error;
    int64_t cursor_prev_hs_tombstone;
    int64_t cursor_prev_skip_ge_100;
    int64_t cursor_prev_skip_lt_100;
    int64_t cursor_reconfigure_error;
    int64_t cursor_remove_error;
    int64_t cursor_reopen_error;
    int64_t cursor_reserve_error;
    int64_t cursor_reset_error;
    int64_t cursor_search_error;
    int64_t cursor_search_near_error;
    int64_t cursor_update_error;
    int64_t cursor_insert;
    int64_t cursor_insert_bytes;
    int64_t cursor_modify;
    int64_t cursor_modify_bytes;
    int64_t cursor_modify_bytes_touch;
    int64_t cursor_next;
    int64_t cursor_open_count;
    int64_t cursor_restart;
    int64_t cursor_prev;
    int64_t cursor_remove;
    int64_t cursor_remove_bytes;
    int64_t cursor_reserve;
    int64_t cursor_reset;
    int64_t cursor_search;
    int64_t cursor_search_hs;
    int64_t cursor_search_near;
    int64_t cursor_truncate;
    int64_t cursor_update;
    int64_t cursor_update_bytes;
    int64_t cursor_update_bytes_changed;
    int64_t layered_curs_insert;
    int64_t layered_curs_next;
    int64_t layered_curs_next_ingest;
    int64_t layered_curs_next_stable;
    int64_t layered_curs_prev;
    int64_t layered_curs_prev_ingest;
    int64_t layered_curs_prev_stable;
    int64_t layered_curs_remove;
    int64_t layered_curs_search_near;
    int64_t layered_curs_search_near_ingest;
    int64_t layered_curs_search_near_stable;
    int64_t layered_curs_search;
    int64_t layered_curs_search_ingest;
    int64_t layered_curs_search_stable;
    int64_t layered_curs_update;
    int64_t layered_curs_upgrade_ingest;
    int64_t layered_curs_upgrade_stable;
    int64_t layered_table_manager_checkpoints;
    int64_t layered_table_manager_checkpoints_refreshed;
    int64_t layered_table_manager_logops_applied;
    int64_t layered_table_manager_logops_skipped;
    int64_t layered_table_manager_skip_lsn;
    int64_t rec_vlcs_emptied_pages;
    int64_t rec_time_window_bytes_ts;
    int64_t rec_time_window_bytes_txn;
    int64_t rec_average_internal_page_delta_chain_length;
    int64_t rec_average_leaf_page_delta_chain_length;
    int64_t rec_page_mods_le5;
    int64_t rec_page_mods_le10;
    int64_t rec_page_mods_le20;
    int64_t rec_page_mods_le50;
    int64_t rec_page_mods_le100;
    int64_t rec_page_mods_le200;
    int64_t rec_page_mods_le500;
    int64_t rec_page_mods_gt500;
    int64_t rec_hs_wrapup_next_prev_calls;
    int64_t rec_dictionary;
    int64_t rec_skip_empty_deltas;
    int64_t rec_page_delete_fast;
    int64_t rec_page_full_image_internal;
    int64_t rec_page_full_image_leaf;
    int64_t rec_page_delta_internal;
    int64_t rec_suffix_compression;
    int64_t rec_multiblock_internal;
    int64_t rec_page_delta_leaf;
    int64_t rec_prefix_compression;
    int64_t rec_multiblock_leaf;
    int64_t rec_overflow_key_leaf;
    int64_t rec_max_internal_page_deltas;
    int64_t rec_max_leaf_page_deltas;
    int64_t rec_multiblock_max;
    int64_t rec_ingest_garbage_collection_keys;
    int64_t rec_overflow_value;
    int64_t rec_pages;
    int64_t rec_pages_eviction;
    int64_t rec_pages_size_1MB_to_10MB;
    int64_t rec_pages_size_10MB_to_100MB;
    int64_t rec_pages_size_100MB_to_1GB;
    int64_t rec_pages_size_1GB_plus;
    int64_t rec_page_delete;
    int64_t rec_time_aggr_newest_start_durable_ts;
    int64_t rec_time_aggr_newest_stop_durable_ts;
    int64_t rec_time_aggr_newest_stop_ts;
    int64_t rec_time_aggr_newest_stop_txn;
    int64_t rec_time_aggr_newest_txn;
    int64_t rec_time_aggr_oldest_start_ts;
    int64_t rec_time_aggr_prepared;
    int64_t rec_time_window_pages_prepared;
    int64_t rec_time_window_pages_durable_start_ts;
    int64_t rec_time_window_pages_start_ts;
    int64_t rec_time_window_pages_start_txn;
    int64_t rec_time_window_pages_durable_stop_ts;
    int64_t rec_time_window_pages_stop_ts;
    int64_t rec_time_window_pages_stop_txn;
    int64_t rec_pages_with_internal_deltas;
    int64_t rec_pages_with_leaf_deltas;
    int64_t rec_time_window_prepared;
    int64_t rec_time_window_durable_start_ts;
    int64_t rec_time_window_start_ts;
    int64_t rec_time_window_start_txn;
    int64_t rec_time_window_durable_stop_ts;
    int64_t rec_time_window_stop_ts;
    int64_t rec_time_window_stop_txn;
    int64_t session_compact;
    int64_t txn_read_race_prepare_commit;
    int64_t txn_read_overflow_remove;
    int64_t txn_read_race_prepare_update;
    int64_t txn_rts_sweep_hs_keys_dryrun;
    int64_t txn_rts_hs_stop_older_than_newer_start;
    int64_t txn_rts_inconsistent_ckpt;
    int64_t txn_rts_keys_removed;
    int64_t txn_rts_keys_restored;
    int64_t txn_rts_keys_removed_dryrun;
    int64_t txn_rts_keys_restored_dryrun;
    int64_t txn_rts_hs_restore_tombstones;
    int64_t txn_rts_hs_restore_updates;
    int64_t txn_rts_delete_rle_skipped;
    int64_t txn_rts_stable_rle_skipped;
    int64_t txn_rts_sweep_hs_keys;
    int64_t txn_rts_hs_restore_tombstones_dryrun;
    int64_t txn_rts_hs_restore_updates_dryrun;
    int64_t txn_rts_hs_removed;
    int64_t txn_rts_hs_removed_dryrun;
    int64_t txn_update_conflict;
};

/*
 * Statistics entries for session.
 */
#define WT_SESSION_STATS_BASE 4000
struct __wt_session_stats {
    int64_t bytes_read;
    int64_t bytes_write;
    int64_t lock_dhandle_wait;
    int64_t txn_bytes_dirty;
    int64_t txn_updates;
    int64_t read_time;
    int64_t write_time;
    int64_t lock_schema_wait;
    int64_t cache_time;
    int64_t cache_time_interruptible;
    int64_t cache_time_mandatory;
};

/* Statistics section: END */
