/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

struct __wt_stats {
	const char	*desc;				/* text description */
	uint64_t	 v;				/* 64-bit value */
};

#define	WT_STAT(stats, fld)						\
	(stats)->fld.v
#define	WT_STAT_DECR(stats, fld) do {					\
	--(stats)->fld.v;						\
} while (0)
#define	WT_STAT_INCR(stats, fld) do {					\
	++(stats)->fld.v;						\
} while (0)
#define	WT_STAT_INCRV(stats, fld, value) do {				\
	(stats)->fld.v += (value);					\
} while (0)
#define	WT_STAT_INCRKV(stats, key, value) do {				\
	((WT_STATS *)stats)[(key)].v += (value);			\
} while (0)
#define	WT_STAT_SET(stats, fld, value) do {				\
	(stats)->fld.v = (uint64_t)(value);				\
} while (0)

/* Connection statistics. */
#define	WT_CSTAT_DECR(session, fld) do {				\
	if (S2C(session)->statistics)					\
		WT_STAT_DECR(&S2C(session)->stats, fld);		\
} while (0)
#define	WT_CSTAT_INCR(session, fld) do {				\
	if (S2C(session)->statistics)					\
		WT_STAT_INCR(&S2C(session)->stats, fld);		\
} while (0)
#define	WT_CSTAT_INCRV(session, fld, v) do {				\
	if (S2C(session)->statistics)					\
		WT_STAT_INCRV(&S2C(session)->stats, fld, v);		\
} while (0)
#define	WT_CSTAT_SET(session, fld, v) do {				\
	if (S2C(session)->statistics)					\
		WT_STAT_SET(&S2C(session)->stats, fld, v);		\
} while (0)

/* Data-source statistics. */
#define	WT_DSTAT_DECR(session, fld) do {				\
	if (S2C(session)->statistics)					\
		WT_STAT_DECR(&(session)->dhandle->stats, fld);		\
} while (0)
#define	WT_DSTAT_INCR(session, fld) do {				\
	if (S2C(session)->statistics)					\
		WT_STAT_INCR(&session->dhandle->stats, fld);		\
} while (0)
#define	WT_DSTAT_INCRV(session, fld, v) do {				\
	if (S2C(session)->statistics)					\
		WT_STAT_INCRV(&session->dhandle->stats, fld, v);	\
} while (0)
#define	WT_DSTAT_SET(session, fld, v) do {				\
	if (S2C(session)->statistics)					\
		WT_STAT_SET(&session->dhandle->stats, fld, v);		\
} while (0)

/* Flags used by statistics initialization. */
#define	WT_STATISTICS_CLEAR	0x01
#define	WT_STATISTICS_FAST	0x02

/*
 * DO NOT EDIT: automatically built by dist/stat.py.
 */
/* Statistics section: BEGIN */

/*
 * Statistics entries for data sources.
 */
struct __wt_dsrc_stats {
	WT_STATS block_alloc;
	WT_STATS block_allocsize;
	WT_STATS block_checkpoint_size;
	WT_STATS block_extension;
	WT_STATS block_free;
	WT_STATS block_magic;
	WT_STATS block_major;
	WT_STATS block_minor;
	WT_STATS block_size;
	WT_STATS bloom_count;
	WT_STATS bloom_false_positive;
	WT_STATS bloom_hit;
	WT_STATS bloom_miss;
	WT_STATS bloom_page_evict;
	WT_STATS bloom_page_read;
	WT_STATS bloom_size;
	WT_STATS btree_column_deleted;
	WT_STATS btree_column_fix;
	WT_STATS btree_column_internal;
	WT_STATS btree_column_variable;
	WT_STATS btree_compact_rewrite;
	WT_STATS btree_entries;
	WT_STATS btree_fixed_len;
	WT_STATS btree_maximum_depth;
	WT_STATS btree_maxintlitem;
	WT_STATS btree_maxintlpage;
	WT_STATS btree_maxleafitem;
	WT_STATS btree_maxleafpage;
	WT_STATS btree_overflow;
	WT_STATS btree_row_internal;
	WT_STATS btree_row_leaf;
	WT_STATS cache_bytes_read;
	WT_STATS cache_bytes_write;
	WT_STATS cache_eviction_checkpoint;
	WT_STATS cache_eviction_clean;
	WT_STATS cache_eviction_dirty;
	WT_STATS cache_eviction_fail;
	WT_STATS cache_eviction_force;
	WT_STATS cache_eviction_hazard;
	WT_STATS cache_eviction_internal;
	WT_STATS cache_eviction_merge;
	WT_STATS cache_eviction_merge_fail;
	WT_STATS cache_eviction_merge_levels;
	WT_STATS cache_overflow_value;
	WT_STATS cache_read;
	WT_STATS cache_read_overflow;
	WT_STATS cache_write;
	WT_STATS compress_raw_fail;
	WT_STATS compress_raw_fail_temporary;
	WT_STATS compress_raw_ok;
	WT_STATS compress_read;
	WT_STATS compress_write;
	WT_STATS compress_write_fail;
	WT_STATS compress_write_too_small;
	WT_STATS cursor_create;
	WT_STATS cursor_insert;
	WT_STATS cursor_insert_bulk;
	WT_STATS cursor_insert_bytes;
	WT_STATS cursor_next;
	WT_STATS cursor_prev;
	WT_STATS cursor_remove;
	WT_STATS cursor_remove_bytes;
	WT_STATS cursor_reset;
	WT_STATS cursor_search;
	WT_STATS cursor_search_near;
	WT_STATS cursor_update;
	WT_STATS cursor_update_bytes;
	WT_STATS lsm_chunk_count;
	WT_STATS lsm_generation_max;
	WT_STATS lsm_lookup_no_bloom;
	WT_STATS rec_dictionary;
	WT_STATS rec_ovfl_key;
	WT_STATS rec_ovfl_value;
	WT_STATS rec_page_delete;
	WT_STATS rec_page_merge;
	WT_STATS rec_pages;
	WT_STATS rec_pages_eviction;
	WT_STATS rec_skipped_update;
	WT_STATS rec_split_intl;
	WT_STATS rec_split_leaf;
	WT_STATS rec_split_max;
	WT_STATS session_compact;
	WT_STATS txn_update_conflict;
	WT_STATS txn_write_conflict;
};

/*
 * Statistics entries for connections.
 */
struct __wt_connection_stats {
	WT_STATS block_byte_map_read;
	WT_STATS block_byte_read;
	WT_STATS block_byte_write;
	WT_STATS block_map_read;
	WT_STATS block_read;
	WT_STATS block_write;
	WT_STATS cache_bytes_dirty;
	WT_STATS cache_bytes_inuse;
	WT_STATS cache_bytes_max;
	WT_STATS cache_bytes_read;
	WT_STATS cache_bytes_write;
	WT_STATS cache_eviction_checkpoint;
	WT_STATS cache_eviction_clean;
	WT_STATS cache_eviction_dirty;
	WT_STATS cache_eviction_fail;
	WT_STATS cache_eviction_force;
	WT_STATS cache_eviction_hazard;
	WT_STATS cache_eviction_internal;
	WT_STATS cache_eviction_merge;
	WT_STATS cache_eviction_merge_fail;
	WT_STATS cache_eviction_merge_levels;
	WT_STATS cache_eviction_slow;
	WT_STATS cache_eviction_walk;
	WT_STATS cache_pages_dirty;
	WT_STATS cache_pages_inuse;
	WT_STATS cache_read;
	WT_STATS cache_write;
	WT_STATS cond_wait;
	WT_STATS cursor_create;
	WT_STATS cursor_insert;
	WT_STATS cursor_next;
	WT_STATS cursor_prev;
	WT_STATS cursor_remove;
	WT_STATS cursor_reset;
	WT_STATS cursor_search;
	WT_STATS cursor_search_near;
	WT_STATS cursor_update;
	WT_STATS file_open;
	WT_STATS lsm_rows_merged;
	WT_STATS memory_allocation;
	WT_STATS memory_free;
	WT_STATS memory_grow;
	WT_STATS read_io;
	WT_STATS rec_pages;
	WT_STATS rec_pages_eviction;
	WT_STATS rec_skipped_update;
	WT_STATS rwlock_read;
	WT_STATS rwlock_write;
	WT_STATS txn_ancient;
	WT_STATS txn_begin;
	WT_STATS txn_checkpoint;
	WT_STATS txn_commit;
	WT_STATS txn_fail_cache;
	WT_STATS txn_rollback;
	WT_STATS write_io;
};

/* Statistics section: END */
