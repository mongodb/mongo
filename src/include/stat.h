/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Statistics counters:
 *
 * We use an array of statistics structures; threads write different structures
 * to avoid writing the same cache line and incurring cache coherency overheads,
 * which can dramatically slow fast and otherwise read-mostly workloads.
 *
 * With an 8B statistics value and 64B cache-line alignment, 8 values share the
 * same cache line. There are collisions when different threads choose the same
 * statistics structure and update values that live on the cache line. There is
 * likely some locality however: a thread updating the cursor search statistic
 * is likely to update other cursor statistics with a chance of hitting already
 * cached values.
 *
 * The actual statistic value must be signed, because one thread might increment
 * the value in its structure, and then another thread might decrement the same
 * value in another structure (where the value was initially zero), so the value
 * in the second thread's slot will go negative.
 *
 * When reading a statistics value, the array values are summed and returned to
 * the caller. The summation is performed without locking, so the value read
 * may be inconsistent (and might be negative, if increments/decrements race
 * with the reader).
 *
 * Choosing how many structures isn't easy: obviously, a smaller number creates
 * more conflicts while a larger number uses more memory.
 *
 * Ideally, if the application running on the system is CPU-intensive, and using
 * all CPUs on the system, we want to use the same number of slots as there are
 * CPUs (because their L1 caches are the units of coherency). However, in
 * practice we cannot easily determine how many CPUs are actually available to
 * the application.
 *
 * Our next best option is to use the number of threads in the application as a
 * heuristic for the number of CPUs (presumably, the application architect has
 * figured out how many CPUs are available). However, inside WiredTiger we don't
 * know when the application creates its threads.
 *
 * For now, we use a fixed number of slots. Ideally, we would approximate the
 * largest number of cores we expect on any machine where WiredTiger might be
 * run, however, we don't want to waste that much memory on smaller machines.
 * As of 2015, machines with more than 24 CPUs are relatively rare.
 *
 * Default hash table size; use a prime number of buckets rather than assuming
 * a good hash (Reference Sedgewick, Algorithms in C, "Hash Functions").
 */
#define	WT_COUNTER_SLOTS	23

/*
 * WT_STATS_SLOT_ID is the thread's slot ID for the array of structures.
 *
 * Ideally, we want a slot per CPU, and we want each thread to index the slot
 * corresponding to the CPU it runs on. Unfortunately, getting the ID of the
 * current CPU is difficult: some operating systems provide a system call to
 * acquire a CPU ID, but not all (regardless, making a system call to increment
 * a statistics value is far too expensive).
 *
 * Our second-best option is to use the thread ID. Unfortunately, there is no
 * portable way to obtain a unique thread ID that's a small-enough number to
 * be used as an array index (portable thread IDs are usually a pointer or an
 * opaque chunk, not a simple integer).
 *
 * Our solution is to use the session ID; there is normally a session per thread
 * and the session ID is a small, monotonically increasing number.
 */
#define	WT_STATS_SLOT_ID(session)					\
	((session)->id) % WT_COUNTER_SLOTS

/*
 * Statistic structures are arrays of int64_t's. We have functions to read/write
 * those structures regardless of the specific statistic structure we're working
 * with, by translating statistics structure field names to structure offsets.
 *
 * Translate a statistic's value name to an offset.
 */
#define	WT_STATS_FIELD_TO_SLOT(stats, fld)				\
	(int)(&(stats)[0]->fld - (int64_t *)(stats)[0])

/*
 * Sum the values from all structures in the array.
 */
static inline int64_t
__wt_stats_aggregate(void *stats_arg, int slot)
{
	int64_t **stats, aggr_v;
	int i;

	stats = stats_arg;
	for (aggr_v = 0, i = 0; i < WT_COUNTER_SLOTS; i++)
		aggr_v += stats[i][slot];

	/*
	 * This can race. However, any implementation with a single value can
	 * race as well, different threads could set the same counter value
	 * simultaneously. While we are making races more likely, we are not
	 * fundamentally weakening the isolation semantics found in updating a
	 * single value.
	 *
	 * Additionally, the aggregation can go negative (imagine a thread
	 * incrementing a value after aggregation has passed its slot and a
	 * second thread decrementing a value before aggregation has reached
	 * its slot).
	 *
	 * For historic API compatibility, the external type is a uint64_t;
	 * limit our return to positive values, negative numbers would just
	 * look really, really large.
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

	stats = stats_arg;
	for (i = 0; i < WT_COUNTER_SLOTS; i++)
		stats[i][slot] = 0;
}

/*
 * Read/write statistics without any test for statistics configuration. Reading
 * and writing the field requires different actions: reading sums the values
 * across the array of structures, writing updates a single structure's value.
 */
#define	WT_STAT_READ(stats, fld)					\
	__wt_stats_aggregate(stats, WT_STATS_FIELD_TO_SLOT(stats, fld))
#define	WT_STAT_WRITE(stats, fld, v)					\
	(stats)->fld = (int64_t)(v)

#define	WT_STAT_DECRV(session, stats, fld, value)			\
	(stats)[WT_STATS_SLOT_ID(session)]->fld -= (int64_t)(value)
#define	WT_STAT_DECRV_ATOMIC(session, stats, fld, value)		\
	__wt_atomic_addi64(						\
	    &(stats)[WT_STATS_SLOT_ID(session)]->fld, (int64_t)(value))
#define	WT_STAT_DECR(session, stats, fld)				\
	WT_STAT_DECRV(session, stats, fld, 1)
#define	WT_STAT_INCRV(session, stats, fld, value)			\
	(stats)[WT_STATS_SLOT_ID(session)]->fld += (int64_t)(value)
#define	WT_STAT_INCRV_ATOMIC(session, stats, fld, value)		\
	__wt_atomic_subi64(						\
	    &(stats)[WT_STATS_SLOT_ID(session)]->fld, (int64_t)(value))
#define	WT_STAT_INCR(session, stats, fld)				\
	WT_STAT_INCRV(session, stats, fld, 1)
#define	WT_STAT_SET(session, stats, fld, value) do {			\
	__wt_stats_clear(stats, WT_STATS_FIELD_TO_SLOT(stats, fld));	\
	(stats)[0]->fld = (int64_t)(value);				\
} while (0)

/*
 * Update statistics if "fast" statistics are configured.
 */
#define	WT_STAT_FAST_DECRV(session, stats, fld, value) do {		\
	if (FLD_ISSET(S2C(session)->stat_flags, WT_CONN_STAT_FAST))	\
		WT_STAT_DECRV(session, stats, fld, value);		\
} while (0)
#define	WT_STAT_FAST_DECR(session, stats, fld)				\
	WT_STAT_FAST_DECRV(session, stats, fld, 1)
#define	WT_STAT_FAST_DECRV_ATOMIC(session, stats, fld, value) do {	\
	if (FLD_ISSET(S2C(session)->stat_flags, WT_CONN_STAT_FAST))	\
		WT_STAT_DECRV_ATOMIC(session, stats, fld, value);	\
} while (0)
#define	WT_STAT_FAST_INCRV(session, stats, fld, value) do {		\
	if (FLD_ISSET(S2C(session)->stat_flags, WT_CONN_STAT_FAST))	\
		WT_STAT_INCRV(session, stats, fld, value);		\
} while (0)
#define	WT_STAT_FAST_INCR(session, stats, fld)				\
	WT_STAT_FAST_INCRV(session, stats, fld, 1)
#define	WT_STAT_FAST_INCRV_ATOMIC(session, stats, fld, value) do {	\
	if (FLD_ISSET(S2C(session)->stat_flags, WT_CONN_STAT_FAST))	\
		WT_STAT_INCRV_ATOMIC(session, stats, fld, value);	\
} while (0)
#define	WT_STAT_FAST_SET(session, stats, fld, value) do {		\
	if (FLD_ISSET(S2C(session)->stat_flags, WT_CONN_STAT_FAST))	\
		WT_STAT_SET(session, stats, fld, value);		\
} while (0)

/*
 * Update connection handle statistics if "fast" statistics are configured.
 */
#define	WT_STAT_FAST_CONN_DECR(session, fld)				\
	WT_STAT_FAST_DECR(session, S2C(session)->stats, fld)
#define	WT_STAT_FAST_CONN_DECR_ATOMIC(session, fld)			\
	WT_STAT_FAST_DECRV_ATOMIC(session, S2C(session)->stats, fld, 1)
#define	WT_STAT_FAST_CONN_DECRV(session, fld, value)			\
	WT_STAT_FAST_DECRV(session, S2C(session)->stats, fld, value)
#define	WT_STAT_FAST_CONN_INCR(session, fld)				\
	WT_STAT_FAST_INCR(session, S2C(session)->stats, fld)
#define	WT_STAT_FAST_CONN_INCR_ATOMIC(session, fld)			\
	WT_STAT_FAST_INCRV_ATOMIC(session, S2C(session)->stats, fld, 1)
#define	WT_STAT_FAST_CONN_INCRV(session, fld, value)			\
	WT_STAT_FAST_INCRV(session, S2C(session)->stats, fld, value)
#define	WT_STAT_FAST_CONN_SET(session, fld, value)			\
	WT_STAT_FAST_SET(session, S2C(session)->stats, fld, value)

/*
 * Update data-source handle statistics if "fast" statistics are configured
 * and the data-source handle is set.
 *
 * XXX
 * We shouldn't have to check if the data-source handle is NULL, but it's
 * necessary until everything is converted to using data-source handles.
 */
#define	WT_STAT_FAST_DATA_DECRV(session, fld, value) do {		\
	if ((session)->dhandle != NULL)					\
		WT_STAT_FAST_DECRV(					\
		    session, (session)->dhandle->stats, fld, value);	\
} while (0)
#define	WT_STAT_FAST_DATA_DECR(session, fld)				\
	WT_STAT_FAST_DATA_DECRV(session, fld, 1)
#define	WT_STAT_FAST_DATA_INCRV(session, fld, value) do {		\
	if ((session)->dhandle != NULL)					\
		WT_STAT_FAST_INCRV(					\
		    session, (session)->dhandle->stats, fld, value);	\
} while (0)
#define	WT_STAT_FAST_DATA_INCR(session, fld)				\
	WT_STAT_FAST_DATA_INCRV(session, fld, 1)
#define	WT_STAT_FAST_DATA_SET(session, fld, value) do {			\
	if ((session)->dhandle != NULL)					\
		WT_STAT_FAST_SET(					\
		    session, (session)->dhandle->stats, fld, value);	\
} while (0)

/*
 * DO NOT EDIT: automatically built by dist/stat.py.
 */
/* Statistics section: BEGIN */

/*
 * Statistics entries for connections.
 */
#define	WT_CONNECTION_STATS_BASE	1000
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
	int64_t async_cur_queue;
	int64_t async_max_queue;
	int64_t async_alloc_race;
	int64_t async_flush;
	int64_t async_alloc_view;
	int64_t async_full;
	int64_t async_nowork;
	int64_t async_op_alloc;
	int64_t async_op_compact;
	int64_t async_op_insert;
	int64_t async_op_remove;
	int64_t async_op_search;
	int64_t async_op_update;
	int64_t block_preload;
	int64_t block_read;
	int64_t block_write;
	int64_t block_byte_read;
	int64_t block_byte_write;
	int64_t block_map_read;
	int64_t block_byte_map_read;
	int64_t cache_bytes_inuse;
	int64_t cache_bytes_read;
	int64_t cache_bytes_write;
	int64_t cache_eviction_checkpoint;
	int64_t cache_eviction_get_ref;
	int64_t cache_eviction_get_ref_empty;
	int64_t cache_eviction_get_ref_empty2;
	int64_t cache_eviction_aggressive_set;
	int64_t cache_eviction_queue_empty;
	int64_t cache_eviction_queue_not_empty;
	int64_t cache_eviction_server_evicting;
	int64_t cache_eviction_server_not_evicting;
	int64_t cache_eviction_server_toobig;
	int64_t cache_eviction_server_slept;
	int64_t cache_eviction_slow;
	int64_t cache_eviction_worker_evicting;
	int64_t cache_eviction_force_fail;
	int64_t cache_eviction_walks_active;
	int64_t cache_eviction_walks_started;
	int64_t cache_eviction_hazard;
	int64_t cache_hazard_checks;
	int64_t cache_hazard_walks;
	int64_t cache_hazard_max;
	int64_t cache_inmem_splittable;
	int64_t cache_inmem_split;
	int64_t cache_eviction_internal;
	int64_t cache_eviction_split_internal;
	int64_t cache_eviction_split_leaf;
	int64_t cache_lookaside_insert;
	int64_t cache_lookaside_remove;
	int64_t cache_bytes_max;
	int64_t cache_eviction_maximum_page_size;
	int64_t cache_eviction_dirty;
	int64_t cache_eviction_app_dirty;
	int64_t cache_eviction_deepen;
	int64_t cache_write_lookaside;
	int64_t cache_pages_inuse;
	int64_t cache_eviction_force;
	int64_t cache_eviction_force_delete;
	int64_t cache_eviction_app;
	int64_t cache_eviction_pages_queued;
	int64_t cache_eviction_pages_queued_oldest;
	int64_t cache_read;
	int64_t cache_read_lookaside;
	int64_t cache_pages_requested;
	int64_t cache_eviction_pages_seen;
	int64_t cache_eviction_fail;
	int64_t cache_eviction_walk;
	int64_t cache_write;
	int64_t cache_write_restore;
	int64_t cache_overhead;
	int64_t cache_bytes_internal;
	int64_t cache_bytes_leaf;
	int64_t cache_bytes_overflow;
	int64_t cache_bytes_dirty;
	int64_t cache_pages_dirty;
	int64_t cache_eviction_clean;
	int64_t cond_auto_wait_reset;
	int64_t cond_auto_wait;
	int64_t file_open;
	int64_t memory_allocation;
	int64_t memory_free;
	int64_t memory_grow;
	int64_t cond_wait;
	int64_t rwlock_read;
	int64_t rwlock_write;
	int64_t fsync_io;
	int64_t read_io;
	int64_t write_io;
	int64_t cursor_create;
	int64_t cursor_insert;
	int64_t cursor_next;
	int64_t cursor_prev;
	int64_t cursor_remove;
	int64_t cursor_reset;
	int64_t cursor_restart;
	int64_t cursor_search;
	int64_t cursor_search_near;
	int64_t cursor_update;
	int64_t cursor_truncate;
	int64_t dh_conn_handle_count;
	int64_t dh_sweep_ref;
	int64_t dh_sweep_close;
	int64_t dh_sweep_remove;
	int64_t dh_sweep_tod;
	int64_t dh_sweeps;
	int64_t dh_session_handles;
	int64_t dh_session_sweeps;
	int64_t log_slot_switch_busy;
	int64_t log_slot_closes;
	int64_t log_slot_races;
	int64_t log_slot_transitions;
	int64_t log_slot_joins;
	int64_t log_slot_unbuffered;
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
	int64_t log_compress_mem;
	int64_t log_buffer_size;
	int64_t log_compress_len;
	int64_t log_slot_coalesced;
	int64_t log_close_yields;
	int64_t rec_page_delete_fast;
	int64_t rec_pages;
	int64_t rec_pages_eviction;
	int64_t rec_page_delete;
	int64_t rec_split_stashed_bytes;
	int64_t rec_split_stashed_objects;
	int64_t session_cursor_open;
	int64_t session_open;
	int64_t fsync_active;
	int64_t read_active;
	int64_t write_active;
	int64_t page_busy_blocked;
	int64_t page_forcible_evict_blocked;
	int64_t page_locked_blocked;
	int64_t page_read_blocked;
	int64_t page_sleep;
	int64_t txn_snapshots_created;
	int64_t txn_snapshots_dropped;
	int64_t txn_begin;
	int64_t txn_checkpoint_running;
	int64_t txn_checkpoint_generation;
	int64_t txn_checkpoint_time_max;
	int64_t txn_checkpoint_time_min;
	int64_t txn_checkpoint_time_recent;
	int64_t txn_checkpoint_time_total;
	int64_t txn_checkpoint;
	int64_t txn_fail_cache;
	int64_t txn_checkpoint_fsync_post;
	int64_t txn_checkpoint_fsync_pre;
	int64_t txn_checkpoint_fsync_post_duration;
	int64_t txn_checkpoint_fsync_pre_duration;
	int64_t txn_pinned_range;
	int64_t txn_pinned_checkpoint_range;
	int64_t txn_pinned_snapshot_range;
	int64_t txn_sync;
	int64_t txn_commit;
	int64_t txn_rollback;
};

/*
 * Statistics entries for data sources.
 */
#define	WT_DSRC_STATS_BASE	2000
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
	int64_t btree_row_internal;
	int64_t btree_row_leaf;
	int64_t cache_bytes_read;
	int64_t cache_bytes_write;
	int64_t cache_eviction_checkpoint;
	int64_t cache_eviction_fail;
	int64_t cache_eviction_hazard;
	int64_t cache_inmem_splittable;
	int64_t cache_inmem_split;
	int64_t cache_eviction_internal;
	int64_t cache_eviction_split_internal;
	int64_t cache_eviction_split_leaf;
	int64_t cache_eviction_dirty;
	int64_t cache_read_overflow;
	int64_t cache_overflow_value;
	int64_t cache_eviction_deepen;
	int64_t cache_write_lookaside;
	int64_t cache_read;
	int64_t cache_read_lookaside;
	int64_t cache_pages_requested;
	int64_t cache_write;
	int64_t cache_write_restore;
	int64_t cache_eviction_clean;
	int64_t compress_read;
	int64_t compress_write;
	int64_t compress_write_fail;
	int64_t compress_write_too_small;
	int64_t compress_raw_fail_temporary;
	int64_t compress_raw_fail;
	int64_t compress_raw_ok;
	int64_t cursor_insert_bulk;
	int64_t cursor_create;
	int64_t cursor_insert_bytes;
	int64_t cursor_remove_bytes;
	int64_t cursor_update_bytes;
	int64_t cursor_insert;
	int64_t cursor_next;
	int64_t cursor_prev;
	int64_t cursor_remove;
	int64_t cursor_reset;
	int64_t cursor_restart;
	int64_t cursor_search;
	int64_t cursor_search_near;
	int64_t cursor_truncate;
	int64_t cursor_update;
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
	int64_t session_compact;
	int64_t session_cursor_open;
	int64_t txn_update_conflict;
};

/*
 * Statistics entries for join cursors.
 */
#define	WT_JOIN_STATS_BASE	3000
struct __wt_join_stats {
	int64_t main_access;
	int64_t bloom_false_positive;
	int64_t membership_check;
	int64_t bloom_insert;
	int64_t iterated;
};

/* Statistics section: END */
