/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

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
 */
#define WT_COUNTER_SLOTS 23

/*
 * WT_STATS_SLOT_ID is the thread's slot ID for the array of structures.
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
#define WT_STATS_SLOT_ID(session) (((session)->id) % WT_COUNTER_SLOTS)

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
static inline int64_t
__wt_stats_aggregate(void *stats_arg, int slot)
{
    int64_t **stats, aggr_v;
    int i;

    stats = (int64_t **)stats_arg;
    for (aggr_v = 0, i = 0; i < WT_COUNTER_SLOTS; i++)
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

/*
 * Clear the values in all structures in the array.
 */
static inline void
__wt_stats_clear(void *stats_arg, int slot)
{
    int64_t **stats;
    int i;

    stats = (int64_t **)stats_arg;
    for (i = 0; i < WT_COUNTER_SLOTS; i++)
        stats[i][slot] = 0;
}

/*
 * Read/write statistics if statistics gathering is enabled. Reading and writing the field requires
 * different actions: reading sums the values across the array of structures, writing updates a
 * single structure's value.
 */
#define WT_STAT_ENABLED(session) (S2C(session)->stat_flags != 0)

#define WT_STAT_READ(stats, fld) __wt_stats_aggregate(stats, WT_STATS_FIELD_TO_OFFSET(stats, fld))
#define WT_STAT_WRITE(session, stats, fld, v) \
    do {                                      \
        if (WT_STAT_ENABLED(session))         \
            (stats)->fld = (int64_t)(v);      \
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

#define WT_STAT_DECRV(session, stats, fld, value)                                 \
    do {                                                                          \
        WT_STAT_DECRV_BASE(session, (stats)[(session)->stat_bucket], fld, value); \
    } while (0)
#define WT_STAT_DECRV_ATOMIC(session, stats, fld, value)                                 \
    do {                                                                                 \
        WT_STAT_DECRV_ATOMIC_BASE(session, (stats)[(session)->stat_bucket], fld, value); \
    } while (0)
#define WT_STAT_DECR(session, stats, fld) WT_STAT_DECRV(session, stats, fld, 1)

#define WT_STAT_INCRV(session, stats, fld, value)                                 \
    do {                                                                          \
        WT_STAT_INCRV_BASE(session, (stats)[(session)->stat_bucket], fld, value); \
    } while (0)
#define WT_STAT_INCRV_ATOMIC(session, stats, fld, value)                                 \
    do {                                                                                 \
        WT_STAT_INCRV_ATOMIC_BASE(session, (stats)[(session)->stat_bucket], fld, value); \
    } while (0)
#define WT_STAT_INCR(session, stats, fld) WT_STAT_INCRV(session, stats, fld, 1)
#define WT_STAT_SET(session, stats, fld, value)                            \
    do {                                                                   \
        if (WT_STAT_ENABLED(session)) {                                    \
            __wt_stats_clear(stats, WT_STATS_FIELD_TO_OFFSET(stats, fld)); \
            (stats)[0]->fld = (int64_t)(value);                            \
        }                                                                  \
    } while (0)

/*
 * Update connection handle statistics if statistics gathering is enabled.
 */
#define WT_STAT_CONN_DECRV(session, fld, value) \
    WT_STAT_DECRV_BASE(session, S2C(session)->stats[(session)->stat_bucket], fld, value)
#define WT_STAT_CONN_DECR_ATOMIC(session, fld) \
    WT_STAT_DECRV_ATOMIC_BASE(session, S2C(session)->stats[(session)->stat_bucket], fld, 1)
#define WT_STAT_CONN_DECR(session, fld) WT_STAT_CONN_DECRV(session, fld, 1)

#define WT_STAT_CONN_INCRV(session, fld, value) \
    WT_STAT_INCRV_BASE(session, S2C(session)->stats[(session)->stat_bucket], fld, value)
#define WT_STAT_CONN_INCR_ATOMIC(session, fld) \
    WT_STAT_INCRV_ATOMIC_BASE(session, S2C(session)->stats[(session)->stat_bucket], fld, 1)
#define WT_STAT_CONN_INCR(session, fld) WT_STAT_CONN_INCRV(session, fld, 1)

#define WT_STAT_CONN_SET(session, fld, value) WT_STAT_SET(session, S2C(session)->stats, fld, value)

/*
 * Update data-source handle statistics if statistics gathering is enabled and the data-source
 * handle is set.
 *
 * XXX We shouldn't have to check if the data-source handle is NULL, but it's necessary until
 * everything is converted to using data-source handles.
 */
#define WT_STAT_DATA_DECRV(session, fld, value)                                   \
    do {                                                                          \
        if ((session)->dhandle != NULL && (session)->dhandle->stat_array != NULL) \
            WT_STAT_DECRV(session, (session)->dhandle->stats, fld, value);        \
    } while (0)
#define WT_STAT_DATA_DECR(session, fld) WT_STAT_DATA_DECRV(session, fld, 1)
#define WT_STAT_DATA_INCRV(session, fld, value)                                   \
    do {                                                                          \
        if ((session)->dhandle != NULL && (session)->dhandle->stat_array != NULL) \
            WT_STAT_INCRV(session, (session)->dhandle->stats, fld, value);        \
    } while (0)
#define WT_STAT_DATA_INCR(session, fld) WT_STAT_DATA_INCRV(session, fld, 1)
#define WT_STAT_DATA_SET(session, fld, value)                                     \
    do {                                                                          \
        if ((session)->dhandle != NULL && (session)->dhandle->stat_array != NULL) \
            WT_STAT_SET(session, (session)->dhandle->stats, fld, value);          \
    } while (0)

/*
 * Update connection and data handle statistics if statistics gathering is enabled. Updates both
 * statistics concurrently and is useful to avoid the duplicated calls that happen in a lot of
 * places.
 */
#define WT_STAT_CONN_DATA_DECRV(session, fld, value) \
    do {                                             \
        WT_STAT_CONN_DECRV(session, fld, value);     \
        WT_STAT_DATA_DECRV(session, fld, value);     \
    } while (0)
#define WT_STAT_CONN_DATA_DECR(session, fld) WT_STAT_CONN_DATA_DECRV(session, fld, 1)

#define WT_STAT_CONN_DATA_INCRV(session, fld, value) \
    do {                                             \
        WT_STAT_CONN_INCRV(session, fld, value);     \
        WT_STAT_DATA_INCRV(session, fld, value);     \
    } while (0)
#define WT_STAT_CONN_DATA_INCR(session, fld) WT_STAT_CONN_DATA_INCRV(session, fld, 1)
/*
 * Update per session statistics.
 */
#define WT_STAT_SESSION_INCRV(session, fld, value) \
    WT_STAT_INCRV_BASE(session, &(session)->stats, fld, value)

/*
 * Construct histogram increment functions to put the passed value into the right bucket. Bucket
 * ranges, represented by various statistics, depend upon whether the passed value is in
 * milliseconds or microseconds. Also values less than a given minimum are ignored and not put in
 * any bucket. This floor value keeps us from having an excessively large smallest values.
 */
#define WT_STAT_MSECS_HIST_INCR_FUNC(name, stat, min_val)                                         \
    static inline void __wt_stat_msecs_hist_incr_##name(WT_SESSION_IMPL *session, uint64_t msecs) \
    {                                                                                             \
        if (msecs < (min_val))                                                                    \
            return;                                                                               \
        if (msecs < 50)                                                                           \
            WT_STAT_CONN_INCR(session, stat##_lt50);                                              \
        else if (msecs < 100)                                                                     \
            WT_STAT_CONN_INCR(session, stat##_lt100);                                             \
        else if (msecs < 250)                                                                     \
            WT_STAT_CONN_INCR(session, stat##_lt250);                                             \
        else if (msecs < 500)                                                                     \
            WT_STAT_CONN_INCR(session, stat##_lt500);                                             \
        else if (msecs < 1000)                                                                    \
            WT_STAT_CONN_INCR(session, stat##_lt1000);                                            \
        else                                                                                      \
            WT_STAT_CONN_INCR(session, stat##_gt1000);                                            \
    }

#define WT_STAT_USECS_HIST_INCR_FUNC(name, stat, min_val)                                         \
    static inline void __wt_stat_usecs_hist_incr_##name(WT_SESSION_IMPL *session, uint64_t usecs) \
    {                                                                                             \
        if (usecs < (min_val))                                                                    \
            return;                                                                               \
        if (usecs < 250)                                                                          \
            WT_STAT_CONN_INCR(session, stat##_lt250);                                             \
        else if (usecs < 500)                                                                     \
            WT_STAT_CONN_INCR(session, stat##_lt500);                                             \
        else if (usecs < 1000)                                                                    \
            WT_STAT_CONN_INCR(session, stat##_lt1000);                                            \
        else if (usecs < 10000)                                                                   \
            WT_STAT_CONN_INCR(session, stat##_lt10000);                                           \
        else                                                                                      \
            WT_STAT_CONN_INCR(session, stat##_gt10000);                                           \
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
    int64_t lsm_work_queue_app;
    int64_t lsm_work_queue_manager;
    int64_t lsm_rows_merged;
    int64_t lsm_checkpoint_throttle;
    int64_t lsm_merge_throttle;
    int64_t lsm_work_queue_switch;
    int64_t lsm_work_units_discarded;
    int64_t lsm_work_units_done;
    int64_t lsm_work_units_created;
    int64_t lsm_work_queue_max;
    int64_t block_preload;
    int64_t block_read;
    int64_t block_write;
    int64_t block_byte_read;
    int64_t block_byte_read_mmap;
    int64_t block_byte_read_syscall;
    int64_t block_byte_write;
    int64_t block_byte_write_checkpoint;
    int64_t block_byte_write_mmap;
    int64_t block_byte_write_syscall;
    int64_t block_map_read;
    int64_t block_byte_map_read;
    int64_t block_remap_file_resize;
    int64_t block_remap_file_write;
    int64_t cache_read_app_count;
    int64_t cache_read_app_time;
    int64_t cache_write_app_count;
    int64_t cache_write_app_time;
    int64_t cache_bytes_updates;
    int64_t cache_bytes_image;
    int64_t cache_bytes_hs;
    int64_t cache_bytes_inuse;
    int64_t cache_bytes_dirty_total;
    int64_t cache_bytes_other;
    int64_t cache_bytes_read;
    int64_t cache_bytes_write;
    int64_t cache_lookaside_score;
    int64_t cache_eviction_checkpoint;
    int64_t cache_eviction_blocked_checkpoint_hs;
    int64_t cache_eviction_get_ref;
    int64_t cache_eviction_get_ref_empty;
    int64_t cache_eviction_get_ref_empty2;
    int64_t cache_eviction_aggressive_set;
    int64_t cache_eviction_empty_score;
    int64_t cache_eviction_blocked_ooo_checkpoint_race_1;
    int64_t cache_eviction_blocked_ooo_checkpoint_race_2;
    int64_t cache_eviction_blocked_ooo_checkpoint_race_3;
    int64_t cache_eviction_blocked_ooo_checkpoint_race_4;
    int64_t cache_eviction_walk_passes;
    int64_t cache_eviction_queue_empty;
    int64_t cache_eviction_queue_not_empty;
    int64_t cache_eviction_server_evicting;
    int64_t cache_eviction_server_slept;
    int64_t cache_eviction_slow;
    int64_t cache_eviction_walk_leaf_notfound;
    int64_t cache_eviction_state;
    int64_t cache_eviction_walk_sleeps;
    int64_t cache_eviction_target_page_lt10;
    int64_t cache_eviction_target_page_lt32;
    int64_t cache_eviction_target_page_ge128;
    int64_t cache_eviction_target_page_lt64;
    int64_t cache_eviction_target_page_lt128;
    int64_t cache_eviction_target_page_reduced;
    int64_t cache_eviction_target_strategy_both_clean_and_dirty;
    int64_t cache_eviction_target_strategy_clean;
    int64_t cache_eviction_target_strategy_dirty;
    int64_t cache_eviction_walks_abandoned;
    int64_t cache_eviction_walks_stopped;
    int64_t cache_eviction_walks_gave_up_no_targets;
    int64_t cache_eviction_walks_gave_up_ratio;
    int64_t cache_eviction_walks_ended;
    int64_t cache_eviction_walk_restart;
    int64_t cache_eviction_walk_from_root;
    int64_t cache_eviction_walk_saved_pos;
    int64_t cache_eviction_active_workers;
    int64_t cache_eviction_worker_created;
    int64_t cache_eviction_worker_evicting;
    int64_t cache_eviction_worker_removed;
    int64_t cache_eviction_stable_state_workers;
    int64_t cache_eviction_walks_active;
    int64_t cache_eviction_walks_started;
    int64_t cache_eviction_force_retune;
    int64_t cache_eviction_force_hs_fail;
    int64_t cache_eviction_force_hs;
    int64_t cache_eviction_force_hs_success;
    int64_t cache_eviction_force_clean;
    int64_t cache_eviction_force_clean_time;
    int64_t cache_eviction_force_dirty;
    int64_t cache_eviction_force_dirty_time;
    int64_t cache_eviction_force_long_update_list;
    int64_t cache_eviction_force_delete;
    int64_t cache_eviction_force;
    int64_t cache_eviction_force_fail;
    int64_t cache_eviction_force_fail_time;
    int64_t cache_eviction_hazard;
    int64_t cache_hazard_checks;
    int64_t cache_hazard_walks;
    int64_t cache_hazard_max;
    int64_t cache_hs_score;
    int64_t cache_hs_insert;
    int64_t cache_hs_insert_restart;
    int64_t cache_hs_ondisk_max;
    int64_t cache_hs_ondisk;
    int64_t cache_hs_order_lose_durable_timestamp;
    int64_t cache_hs_order_reinsert;
    int64_t cache_hs_read;
    int64_t cache_hs_read_miss;
    int64_t cache_hs_read_squash;
    int64_t cache_hs_key_truncate_rts_unstable;
    int64_t cache_hs_key_truncate_rts;
    int64_t cache_hs_key_truncate;
    int64_t cache_hs_key_truncate_onpage_removal;
    int64_t cache_hs_order_remove;
    int64_t cache_hs_write_squash;
    int64_t cache_inmem_splittable;
    int64_t cache_inmem_split;
    int64_t cache_eviction_internal;
    int64_t cache_eviction_internal_pages_queued;
    int64_t cache_eviction_internal_pages_seen;
    int64_t cache_eviction_internal_pages_already_queued;
    int64_t cache_eviction_split_internal;
    int64_t cache_eviction_split_leaf;
    int64_t cache_bytes_max;
    int64_t cache_eviction_maximum_page_size;
    int64_t cache_eviction_dirty;
    int64_t cache_eviction_app_dirty;
    int64_t cache_timed_out_ops;
    int64_t cache_read_overflow;
    int64_t cache_eviction_deepen;
    int64_t cache_write_hs;
    int64_t cache_pages_inuse;
    int64_t cache_eviction_app;
    int64_t cache_eviction_pages_in_parallel_with_checkpoint;
    int64_t cache_eviction_pages_queued;
    int64_t cache_eviction_pages_queued_post_lru;
    int64_t cache_eviction_pages_queued_urgent;
    int64_t cache_eviction_pages_queued_oldest;
    int64_t cache_eviction_pages_queued_urgent_hs_dirty;
    int64_t cache_read;
    int64_t cache_read_deleted;
    int64_t cache_read_deleted_prepared;
    int64_t cache_pages_requested;
    int64_t cache_eviction_pages_seen;
    int64_t cache_eviction_pages_already_queued;
    int64_t cache_eviction_fail;
    int64_t cache_eviction_fail_parent_has_overflow_items;
    int64_t cache_eviction_fail_active_children_on_an_internal_page;
    int64_t cache_eviction_fail_in_reconciliation;
    int64_t cache_eviction_fail_checkpoint_out_of_order_ts;
    int64_t cache_eviction_walk;
    int64_t cache_write;
    int64_t cache_write_restore;
    int64_t cache_overhead;
    int64_t cache_hs_insert_full_update;
    int64_t cache_hs_insert_reverse_modify;
    int64_t cache_bytes_internal;
    int64_t cache_bytes_leaf;
    int64_t cache_bytes_dirty;
    int64_t cache_pages_dirty;
    int64_t cache_eviction_clean;
    int64_t fsync_all_fh_total;
    int64_t fsync_all_fh;
    int64_t fsync_all_time;
    int64_t capacity_bytes_read;
    int64_t capacity_bytes_ckpt;
    int64_t capacity_bytes_evict;
    int64_t capacity_bytes_log;
    int64_t capacity_bytes_written;
    int64_t capacity_threshold;
    int64_t capacity_time_total;
    int64_t capacity_time_ckpt;
    int64_t capacity_time_evict;
    int64_t capacity_time_log;
    int64_t capacity_time_read;
    int64_t cc_pages_evict;
    int64_t cc_pages_removed;
    int64_t cc_pages_walk_skipped;
    int64_t cc_pages_visited;
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
    int64_t cond_wait;
    int64_t rwlock_read;
    int64_t rwlock_write;
    int64_t fsync_io;
    int64_t read_io;
    int64_t write_io;
    int64_t cursor_next_skip_total;
    int64_t cursor_prev_skip_total;
    int64_t cursor_skip_hs_cur_position;
    int64_t cursor_search_near_prefix_fast_paths;
    int64_t cursor_cached_count;
    int64_t cursor_insert_bulk;
    int64_t cursor_cache;
    int64_t cursor_create;
    int64_t cursor_insert;
    int64_t cursor_insert_bytes;
    int64_t cursor_modify;
    int64_t cursor_modify_bytes;
    int64_t cursor_modify_bytes_touch;
    int64_t cursor_next;
    int64_t cursor_next_hs_tombstone;
    int64_t cursor_next_skip_ge_100;
    int64_t cursor_next_skip_lt_100;
    int64_t cursor_restart;
    int64_t cursor_prev;
    int64_t cursor_prev_hs_tombstone;
    int64_t cursor_prev_skip_ge_100;
    int64_t cursor_prev_skip_lt_100;
    int64_t cursor_remove;
    int64_t cursor_remove_bytes;
    int64_t cursor_reserve;
    int64_t cursor_reset;
    int64_t cursor_search;
    int64_t cursor_search_hs;
    int64_t cursor_search_near;
    int64_t cursor_sweep_buckets;
    int64_t cursor_sweep_closed;
    int64_t cursor_sweep_examined;
    int64_t cursor_sweep;
    int64_t cursor_truncate;
    int64_t cursor_update;
    int64_t cursor_update_bytes;
    int64_t cursor_update_bytes_changed;
    int64_t cursor_reopen;
    int64_t cursor_open_count;
    int64_t dh_conn_handle_size;
    int64_t dh_conn_handle_count;
    int64_t dh_sweep_ref;
    int64_t dh_sweep_close;
    int64_t dh_sweep_remove;
    int64_t dh_sweep_tod;
    int64_t dh_sweeps;
    int64_t dh_sweep_skip_ckpt;
    int64_t dh_session_handles;
    int64_t dh_session_sweeps;
    int64_t lock_checkpoint_count;
    int64_t lock_checkpoint_wait_application;
    int64_t lock_checkpoint_wait_internal;
    int64_t lock_dhandle_wait_application;
    int64_t lock_dhandle_wait_internal;
    int64_t lock_dhandle_read_count;
    int64_t lock_dhandle_write_count;
    int64_t lock_durable_timestamp_wait_application;
    int64_t lock_durable_timestamp_wait_internal;
    int64_t lock_durable_timestamp_read_count;
    int64_t lock_durable_timestamp_write_count;
    int64_t lock_metadata_count;
    int64_t lock_metadata_wait_application;
    int64_t lock_metadata_wait_internal;
    int64_t lock_read_timestamp_wait_application;
    int64_t lock_read_timestamp_wait_internal;
    int64_t lock_read_timestamp_read_count;
    int64_t lock_read_timestamp_write_count;
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
    int64_t log_force_archive_sleep;
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
    int64_t perf_hist_fsread_latency_lt50;
    int64_t perf_hist_fsread_latency_lt100;
    int64_t perf_hist_fsread_latency_lt250;
    int64_t perf_hist_fsread_latency_lt500;
    int64_t perf_hist_fsread_latency_lt1000;
    int64_t perf_hist_fsread_latency_gt1000;
    int64_t perf_hist_fswrite_latency_lt50;
    int64_t perf_hist_fswrite_latency_lt100;
    int64_t perf_hist_fswrite_latency_lt250;
    int64_t perf_hist_fswrite_latency_lt500;
    int64_t perf_hist_fswrite_latency_lt1000;
    int64_t perf_hist_fswrite_latency_gt1000;
    int64_t perf_hist_opread_latency_lt250;
    int64_t perf_hist_opread_latency_lt500;
    int64_t perf_hist_opread_latency_lt1000;
    int64_t perf_hist_opread_latency_lt10000;
    int64_t perf_hist_opread_latency_gt10000;
    int64_t perf_hist_opwrite_latency_lt250;
    int64_t perf_hist_opwrite_latency_lt500;
    int64_t perf_hist_opwrite_latency_lt1000;
    int64_t perf_hist_opwrite_latency_lt10000;
    int64_t perf_hist_opwrite_latency_gt10000;
    int64_t rec_time_window_bytes_ts;
    int64_t rec_time_window_bytes_txn;
    int64_t rec_page_delete_fast;
    int64_t rec_overflow_key_internal;
    int64_t rec_overflow_key_leaf;
    int64_t rec_maximum_seconds;
    int64_t rec_pages;
    int64_t rec_pages_eviction;
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
    int64_t flush_tier;
    int64_t local_objects_removed;
    int64_t session_open;
    int64_t session_query_ts;
    int64_t session_table_alter_fail;
    int64_t session_table_alter_success;
    int64_t session_table_alter_trigger_checkpoint;
    int64_t session_table_alter_skip;
    int64_t session_table_compact_fail;
    int64_t session_table_compact_success;
    int64_t session_table_create_fail;
    int64_t session_table_create_success;
    int64_t session_table_drop_fail;
    int64_t session_table_drop_success;
    int64_t session_table_rename_fail;
    int64_t session_table_rename_success;
    int64_t session_table_salvage_fail;
    int64_t session_table_salvage_success;
    int64_t session_table_truncate_fail;
    int64_t session_table_truncate_success;
    int64_t session_table_verify_fail;
    int64_t session_table_verify_success;
    int64_t tiered_work_units_dequeued;
    int64_t tiered_work_units_created;
    int64_t tiered_retention;
    int64_t tiered_object_size;
    int64_t thread_fsync_active;
    int64_t thread_read_active;
    int64_t thread_write_active;
    int64_t application_evict_time;
    int64_t application_cache_time;
    int64_t txn_release_blocked;
    int64_t conn_close_blocked_lsm;
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
    int64_t txn_prepared_updates;
    int64_t txn_prepared_updates_committed;
    int64_t txn_prepared_updates_key_repeated;
    int64_t txn_prepared_updates_rolledback;
    int64_t txn_prepare;
    int64_t txn_prepare_commit;
    int64_t txn_prepare_active;
    int64_t txn_prepare_rollback;
    int64_t txn_prepare_rollback_do_not_remove_hs_update;
    int64_t txn_prepare_rollback_fix_hs_update_with_ckpt_reserved_txnid;
    int64_t txn_query_ts;
    int64_t txn_read_race_prepare_update;
    int64_t txn_rts;
    int64_t txn_rts_hs_stop_older_than_newer_start;
    int64_t txn_rts_inconsistent_ckpt;
    int64_t txn_rts_keys_removed;
    int64_t txn_rts_keys_restored;
    int64_t txn_rts_pages_visited;
    int64_t txn_rts_hs_restore_tombstones;
    int64_t txn_rts_hs_restore_updates;
    int64_t txn_rts_delete_rle_skipped;
    int64_t txn_rts_stable_rle_skipped;
    int64_t txn_rts_sweep_hs_keys;
    int64_t txn_rts_tree_walk_skip_pages;
    int64_t txn_rts_upd_aborted;
    int64_t txn_rts_hs_removed;
    int64_t txn_sessions_walked;
    int64_t txn_set_ts;
    int64_t txn_set_ts_durable;
    int64_t txn_set_ts_durable_upd;
    int64_t txn_set_ts_oldest;
    int64_t txn_set_ts_oldest_upd;
    int64_t txn_set_ts_stable;
    int64_t txn_set_ts_stable_upd;
    int64_t txn_begin;
    int64_t txn_checkpoint_running;
    int64_t txn_checkpoint_running_hs;
    int64_t txn_checkpoint_generation;
    int64_t txn_hs_ckpt_duration;
    int64_t txn_checkpoint_time_max;
    int64_t txn_checkpoint_time_min;
    int64_t txn_checkpoint_handle_duration;
    int64_t txn_checkpoint_handle_duration_apply;
    int64_t txn_checkpoint_handle_duration_skip;
    int64_t txn_checkpoint_handle_applied;
    int64_t txn_checkpoint_handle_skipped;
    int64_t txn_checkpoint_handle_walked;
    int64_t txn_checkpoint_time_recent;
    int64_t txn_checkpoint_prep_running;
    int64_t txn_checkpoint_prep_max;
    int64_t txn_checkpoint_prep_min;
    int64_t txn_checkpoint_prep_recent;
    int64_t txn_checkpoint_prep_total;
    int64_t txn_checkpoint_scrub_target;
    int64_t txn_checkpoint_scrub_time;
    int64_t txn_checkpoint_time_total;
    int64_t txn_checkpoint;
    int64_t txn_checkpoint_obsolete_applied;
    int64_t txn_checkpoint_skipped;
    int64_t txn_fail_cache;
    int64_t txn_checkpoint_fsync_post;
    int64_t txn_checkpoint_fsync_post_duration;
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
    int64_t bloom_false_positive;
    int64_t bloom_hit;
    int64_t bloom_miss;
    int64_t bloom_page_evict;
    int64_t bloom_page_read;
    int64_t bloom_count;
    int64_t lsm_chunk_count;
    int64_t lsm_generation_max;
    int64_t lsm_lookup_no_bloom;
    int64_t lsm_checkpoint_throttle;
    int64_t lsm_merge_throttle;
    int64_t bloom_size;
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
    int64_t btree_column_fix;
    int64_t btree_column_internal;
    int64_t btree_column_rle;
    int64_t btree_column_deleted;
    int64_t btree_column_variable;
    int64_t btree_fixed_len;
    int64_t btree_maxintlkey;
    int64_t btree_maxintlpage;
    int64_t btree_maxleafkey;
    int64_t btree_maxleafpage;
    int64_t btree_maxleafvalue;
    int64_t btree_maximum_depth;
    int64_t btree_entries;
    int64_t btree_overflow;
    int64_t btree_compact_rewrite;
    int64_t btree_row_empty_values;
    int64_t btree_row_internal;
    int64_t btree_row_leaf;
    int64_t cache_bytes_inuse;
    int64_t cache_bytes_dirty_total;
    int64_t cache_bytes_read;
    int64_t cache_bytes_write;
    int64_t cache_eviction_checkpoint;
    int64_t cache_eviction_blocked_checkpoint_hs;
    int64_t cache_eviction_fail;
    int64_t cache_eviction_blocked_ooo_checkpoint_race_1;
    int64_t cache_eviction_blocked_ooo_checkpoint_race_2;
    int64_t cache_eviction_blocked_ooo_checkpoint_race_3;
    int64_t cache_eviction_blocked_ooo_checkpoint_race_4;
    int64_t cache_eviction_walk_passes;
    int64_t cache_eviction_target_page_lt10;
    int64_t cache_eviction_target_page_lt32;
    int64_t cache_eviction_target_page_ge128;
    int64_t cache_eviction_target_page_lt64;
    int64_t cache_eviction_target_page_lt128;
    int64_t cache_eviction_target_page_reduced;
    int64_t cache_eviction_walks_abandoned;
    int64_t cache_eviction_walks_stopped;
    int64_t cache_eviction_walks_gave_up_no_targets;
    int64_t cache_eviction_walks_gave_up_ratio;
    int64_t cache_eviction_walks_ended;
    int64_t cache_eviction_walk_restart;
    int64_t cache_eviction_walk_from_root;
    int64_t cache_eviction_walk_saved_pos;
    int64_t cache_eviction_hazard;
    int64_t cache_hs_insert;
    int64_t cache_hs_insert_restart;
    int64_t cache_hs_order_lose_durable_timestamp;
    int64_t cache_hs_order_reinsert;
    int64_t cache_hs_read;
    int64_t cache_hs_read_miss;
    int64_t cache_hs_read_squash;
    int64_t cache_hs_key_truncate_rts_unstable;
    int64_t cache_hs_key_truncate_rts;
    int64_t cache_hs_key_truncate;
    int64_t cache_hs_key_truncate_onpage_removal;
    int64_t cache_hs_order_remove;
    int64_t cache_hs_write_squash;
    int64_t cache_inmem_splittable;
    int64_t cache_inmem_split;
    int64_t cache_eviction_internal;
    int64_t cache_eviction_split_internal;
    int64_t cache_eviction_split_leaf;
    int64_t cache_eviction_dirty;
    int64_t cache_read_overflow;
    int64_t cache_eviction_deepen;
    int64_t cache_write_hs;
    int64_t cache_read;
    int64_t cache_read_deleted;
    int64_t cache_read_deleted_prepared;
    int64_t cache_pages_requested;
    int64_t cache_eviction_pages_seen;
    int64_t cache_write;
    int64_t cache_write_restore;
    int64_t cache_hs_insert_full_update;
    int64_t cache_hs_insert_reverse_modify;
    int64_t cache_bytes_dirty;
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
    int64_t cc_pages_evict;
    int64_t cc_pages_removed;
    int64_t cc_pages_walk_skipped;
    int64_t cc_pages_visited;
    int64_t compress_precomp_intl_max_page_size;
    int64_t compress_precomp_leaf_max_page_size;
    int64_t compress_read;
    int64_t compress_write;
    int64_t compress_write_fail;
    int64_t compress_write_too_small;
    int64_t cursor_next_skip_total;
    int64_t cursor_prev_skip_total;
    int64_t cursor_skip_hs_cur_position;
    int64_t cursor_search_near_prefix_fast_paths;
    int64_t cursor_insert_bulk;
    int64_t cursor_reopen;
    int64_t cursor_cache;
    int64_t cursor_create;
    int64_t cursor_next_hs_tombstone;
    int64_t cursor_next_skip_ge_100;
    int64_t cursor_next_skip_lt_100;
    int64_t cursor_prev_hs_tombstone;
    int64_t cursor_prev_skip_ge_100;
    int64_t cursor_prev_skip_lt_100;
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
    int64_t rec_time_window_bytes_ts;
    int64_t rec_time_window_bytes_txn;
    int64_t rec_dictionary;
    int64_t rec_page_delete_fast;
    int64_t rec_suffix_compression;
    int64_t rec_multiblock_internal;
    int64_t rec_overflow_key_internal;
    int64_t rec_prefix_compression;
    int64_t rec_multiblock_leaf;
    int64_t rec_overflow_key_leaf;
    int64_t rec_multiblock_max;
    int64_t rec_overflow_value;
    int64_t rec_page_match;
    int64_t rec_pages;
    int64_t rec_pages_eviction;
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
    int64_t rec_time_window_prepared;
    int64_t rec_time_window_durable_start_ts;
    int64_t rec_time_window_start_ts;
    int64_t rec_time_window_start_txn;
    int64_t rec_time_window_durable_stop_ts;
    int64_t rec_time_window_stop_ts;
    int64_t rec_time_window_stop_txn;
    int64_t session_compact;
    int64_t tiered_work_units_dequeued;
    int64_t tiered_work_units_created;
    int64_t tiered_retention;
    int64_t tiered_object_size;
    int64_t txn_read_race_prepare_update;
    int64_t txn_rts_hs_stop_older_than_newer_start;
    int64_t txn_rts_inconsistent_ckpt;
    int64_t txn_rts_keys_removed;
    int64_t txn_rts_keys_restored;
    int64_t txn_rts_hs_restore_tombstones;
    int64_t txn_rts_hs_restore_updates;
    int64_t txn_rts_delete_rle_skipped;
    int64_t txn_rts_stable_rle_skipped;
    int64_t txn_rts_sweep_hs_keys;
    int64_t txn_rts_hs_removed;
    int64_t txn_checkpoint_obsolete_applied;
    int64_t txn_update_conflict;
};

/*
 * Statistics entries for join cursors.
 */
#define WT_JOIN_STATS_BASE 3000
struct __wt_join_stats {
    int64_t main_access;
    int64_t bloom_false_positive;
    int64_t membership_check;
    int64_t bloom_insert;
    int64_t iterated;
};

/*
 * Statistics entries for session.
 */
#define WT_SESSION_STATS_BASE 4000
struct __wt_session_stats {
    int64_t bytes_read;
    int64_t bytes_write;
    int64_t lock_dhandle_wait;
    int64_t read_time;
    int64_t write_time;
    int64_t lock_schema_wait;
    int64_t cache_time;
};

/* Statistics section: END */
