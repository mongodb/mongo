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

#define	WT_BSTAT_INCR(session, fld)					\
	WT_STAT_INCR((session)->btree->stats, fld)
#define	WT_BSTAT_INCRV(session, fld, v)					\
	WT_STAT_INCRV((session)->btree->stats, fld, v)
#define	WT_BSTAT_DECR(session, fld)					\
	WT_STAT_DECR((session)->btree->stats, fld)
#define	WT_BSTAT_SET(session, fld, v)					\
	WT_STAT_SET((session)->btree->stats, fld, v)

#define	WT_CSTAT_INCR(session, fld)					\
	WT_STAT_INCR(S2C(session)->stats, fld)

/*
 * DO NOT EDIT: automatically built by dist/stat.py.
 */
/* Statistics section: BEGIN */

/*
 * Statistics entries for BTREE handle.
 */
struct __wt_btree_stats {
	WT_STATS file_bulk_loaded;
	WT_STATS file_col_deleted;
	WT_STATS file_col_fix_pages;
	WT_STATS file_col_int_pages;
	WT_STATS file_col_var_pages;
	WT_STATS cursor_inserts;
	WT_STATS cursor_read;
	WT_STATS cursor_read_near;
	WT_STATS cursor_read_next;
	WT_STATS cursor_read_prev;
	WT_STATS cursor_removes;
	WT_STATS cursor_resets;
	WT_STATS cursor_updates;
	WT_STATS alloc;
	WT_STATS extend;
	WT_STATS free;
	WT_STATS overflow_read;
	WT_STATS page_read;
	WT_STATS page_write;
	WT_STATS file_size;
	WT_STATS file_fixed_len;
	WT_STATS file_magic;
	WT_STATS file_major;
	WT_STATS file_maxintlitem;
	WT_STATS file_maxintlpage;
	WT_STATS file_maxleafitem;
	WT_STATS file_maxleafpage;
	WT_STATS file_minor;
	WT_STATS file_overflow;
	WT_STATS file_allocsize;
	WT_STATS rec_page_merge;
	WT_STATS rec_split_intl;
	WT_STATS rec_split_leaf;
	WT_STATS rec_ovfl_key;
	WT_STATS rec_ovfl_value;
	WT_STATS rec_page_delete;
	WT_STATS rec_written;
	WT_STATS rec_hazard;
	WT_STATS file_row_int_pages;
	WT_STATS file_row_leaf_pages;
	WT_STATS file_entries;
	WT_STATS update_conflict;
	WT_STATS file_write_conflicts;
};

/*
 * Statistics entries for CONNECTION handle.
 */
struct __wt_connection_stats {
	WT_STATS block_read;
	WT_STATS block_write;
	WT_STATS cache_bytes_inuse;
	WT_STATS cache_evict_slow;
	WT_STATS cache_evict_internal;
	WT_STATS cache_bytes_max;
	WT_STATS cache_evict_modified;
	WT_STATS cache_pages_inuse;
	WT_STATS cache_evict_hazard;
	WT_STATS cache_evict_unmodified;
	WT_STATS checkpoint;
	WT_STATS cond_wait;
	WT_STATS file_open;
	WT_STATS rwlock_rdlock;
	WT_STATS rwlock_wrlock;
	WT_STATS memalloc;
	WT_STATS memfree;
	WT_STATS total_read_io;
	WT_STATS total_write_io;
	WT_STATS txn_begin;
	WT_STATS txn_commit;
	WT_STATS txn_rollback;
};

/* Statistics section: END */
