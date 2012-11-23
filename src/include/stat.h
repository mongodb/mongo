/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
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
#define	WT_STAT_DECRV(stats, fld, value) do {				\
	(stats)->fld.v -= (value);					\
} while (0)
#define	WT_STAT_INCRV(stats, fld, value) do {				\
	(stats)->fld.v += (value);					\
} while (0)
#define	WT_STAT_SET(stats, fld, value) do {				\
	(stats)->fld.v = (uint64_t)(value);				\
} while (0)

#define	WT_STAT_CHECK_SESSION(session)					\
	((session) != NULL && (session) != S2C(session)->default_session)

#define	WT_BSTAT_INCR(session, fld) do {				\
	if (WT_STAT_CHECK_SESSION(session)) {				\
		WT_STAT_INCR((session)->btree->stats, fld);		\
	}								\
} while (0)
#define	WT_BSTAT_INCRV(session, fld, v) do {				\
	if (WT_STAT_CHECK_SESSION(session)) {				\
		WT_STAT_INCRV((session)->btree->stats, fld, v);		\
	}								\
} while (0)
#define	WT_BSTAT_DECR(session, fld) do {				\
	if (WT_STAT_CHECK_SESSION(session)) {				\
		WT_STAT_DECR((session)->btree->stats, fld);		\
	}								\
} while (0)
#define	WT_BSTAT_SET(session, fld, v) do {				\
	if (WT_STAT_CHECK_SESSION(session)) {				\
		WT_STAT_SET((session)->btree->stats, fld, v);		\
	}								\
} while (0)

#define	WT_CSTAT_INCR(session, fld) do {				\
	if (WT_STAT_CHECK_SESSION(session)) {				\
		WT_STAT_INCR(S2C(session)->stats, fld);			\
	}								\
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
	WT_STATS block_extend;
	WT_STATS block_free;
	WT_STATS bloom_count;
	WT_STATS bloom_false_positive;
	WT_STATS bloom_hit;
	WT_STATS bloom_miss;
	WT_STATS bloom_page_evict;
	WT_STATS bloom_page_read;
	WT_STATS bloom_size;
	WT_STATS ckpt_size;
	WT_STATS cursor_insert;
	WT_STATS cursor_read;
	WT_STATS cursor_read_near;
	WT_STATS cursor_read_next;
	WT_STATS cursor_read_prev;
	WT_STATS cursor_remove;
	WT_STATS cursor_reset;
	WT_STATS cursor_update;
	WT_STATS entries;
	WT_STATS file_allocsize;
	WT_STATS file_bulk_loaded;
	WT_STATS file_compact_rewrite;
	WT_STATS file_fixed_len;
	WT_STATS file_magic;
	WT_STATS file_major;
	WT_STATS file_maxintlitem;
	WT_STATS file_maxintlpage;
	WT_STATS file_maxleafitem;
	WT_STATS file_maxleafpage;
	WT_STATS file_minor;
	WT_STATS file_size;
	WT_STATS lsm_chunk_count;
	WT_STATS lsm_generation_max;
	WT_STATS lsm_lookup_no_bloom;
	WT_STATS overflow_page;
	WT_STATS overflow_read;
	WT_STATS overflow_value_cache;
	WT_STATS page_col_deleted;
	WT_STATS page_col_fix;
	WT_STATS page_col_int;
	WT_STATS page_col_var;
	WT_STATS page_evict;
	WT_STATS page_evict_fail;
	WT_STATS page_read;
	WT_STATS page_row_int;
	WT_STATS page_row_leaf;
	WT_STATS page_write;
	WT_STATS rec_dictionary;
	WT_STATS rec_hazard;
	WT_STATS rec_ovfl_key;
	WT_STATS rec_ovfl_value;
	WT_STATS rec_page_delete;
	WT_STATS rec_page_merge;
	WT_STATS rec_split_intl;
	WT_STATS rec_split_leaf;
	WT_STATS rec_written;
	WT_STATS txn_update_conflict;
	WT_STATS txn_write_conflict;
};

/*
 * Statistics entries for connections.
 */
struct __wt_connection_stats {
	WT_STATS block_read;
	WT_STATS block_write;
	WT_STATS cache_bytes_inuse;
	WT_STATS cache_bytes_max;
	WT_STATS cache_evict_hazard;
	WT_STATS cache_evict_internal;
	WT_STATS cache_evict_modified;
	WT_STATS cache_evict_slow;
	WT_STATS cache_evict_unmodified;
	WT_STATS cache_pages_inuse;
	WT_STATS checkpoint;
	WT_STATS cond_wait;
	WT_STATS file_open;
	WT_STATS memalloc;
	WT_STATS memfree;
	WT_STATS rwlock_rdlock;
	WT_STATS rwlock_wrlock;
	WT_STATS total_read_io;
	WT_STATS total_write_io;
	WT_STATS txn_ancient;
	WT_STATS txn_begin;
	WT_STATS txn_commit;
	WT_STATS txn_fail_cache;
	WT_STATS txn_rollback;
};

/* Statistics section: END */
