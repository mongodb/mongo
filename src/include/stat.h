/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#if defined(__cplusplus)
extern "C" {
#endif

struct __wt_stats {
	uint64_t	 v;				/* 64-bit value */
	const char	*desc;				/* text description */
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
	(stats)->fld.v = (value);					\
} while (0)

/*
 * DO NOT EDIT: automatically built by dist/stat.py.
 */
/* Statistics section: BEGIN */

/*
 * Statistics entries for BTREE handle.
 */
struct __wt_btree_stats {
	WT_STATS file_item_col_deleted;
	WT_STATS file_col_fix;
	WT_STATS file_col_internal;
	WT_STATS file_col_variable;
	WT_STATS alloc;
	WT_STATS extend;
	WT_STATS free;
	WT_STATS items_inserted;
	WT_STATS overflow_key;
	WT_STATS overflow_read;
	WT_STATS overflow_data;
	WT_STATS page_delete;
	WT_STATS page_read;
	WT_STATS page_write;
	WT_STATS file_fixed_len;
	WT_STATS file_magic;
	WT_STATS file_major;
	WT_STATS file_intlmax;
	WT_STATS file_leafmax;
	WT_STATS file_intlmin;
	WT_STATS file_leafmin;
	WT_STATS file_minor;
	WT_STATS file_freelist_entries;
	WT_STATS file_overflow;
	WT_STATS file_allocsize;
	WT_STATS file_row_internal;
	WT_STATS file_row_leaf;
	WT_STATS split_intl;
	WT_STATS split_leaf;
	WT_STATS file_item_total_data;
	WT_STATS file_item_total_key;
	WT_STATS __end;
};

/*
 * Statistics entries for CONNECTION handle.
 */
struct __wt_conn_stats {
	WT_STATS cache_bytes_inuse;
	WT_STATS cache_bytes_max;
	WT_STATS cache_evict_modified;
	WT_STATS cache_overflow_read;
	WT_STATS cache_pages_inuse;
	WT_STATS cache_page_read;
	WT_STATS cache_evict_hazard;
	WT_STATS cache_page_write;
	WT_STATS cache_evict_unmodified;
	WT_STATS file_open;
	WT_STATS memalloc;
	WT_STATS memfree;
	WT_STATS mtx_lock;
	WT_STATS total_read_io;
	WT_STATS total_write_io;
	WT_STATS workq_passes;
	WT_STATS workq_yield;
	WT_STATS __end;
};

/* Statistics section: END */

#if defined(__cplusplus)
}
#endif
