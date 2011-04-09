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
	struct __wt_stats alloc;
	struct __wt_stats extend;
	struct __wt_stats free;
	struct __wt_stats huffman_key;
	struct __wt_stats huffman_value;
	struct __wt_stats items_inserted;
	struct __wt_stats overflow_data;
	struct __wt_stats overflow_key;
	struct __wt_stats overflow_read;
	struct __wt_stats page_read;
	struct __wt_stats page_write;
	struct __wt_stats split_intl;
	struct __wt_stats split_leaf;
};

/*
 * Statistics entries for BTREE FILE handle.
 */
struct __wt_btree_file_stats {
	struct __wt_stats file_allocsize;
	struct __wt_stats file_base_recno;
	struct __wt_stats file_col_fix;
	struct __wt_stats file_col_internal;
	struct __wt_stats file_col_rle;
	struct __wt_stats file_col_variable;
	struct __wt_stats file_fixed_len;
	struct __wt_stats file_freelist_entries;
	struct __wt_stats file_intlmax;
	struct __wt_stats file_intlmin;
	struct __wt_stats file_item_col_deleted;
	struct __wt_stats file_item_total_data;
	struct __wt_stats file_item_total_key;
	struct __wt_stats file_leafmax;
	struct __wt_stats file_leafmin;
	struct __wt_stats file_magic;
	struct __wt_stats file_major;
	struct __wt_stats file_minor;
	struct __wt_stats file_overflow;
	struct __wt_stats file_row_internal;
	struct __wt_stats file_row_leaf;
};

/*
 * Statistics entries for CACHE handle.
 */
struct __wt_cache_stats {
	struct __wt_stats cache_bytes_inuse;
	struct __wt_stats cache_bytes_max;
	struct __wt_stats cache_evict_hazard;
	struct __wt_stats cache_evict_modified;
	struct __wt_stats cache_evict_unmodified;
	struct __wt_stats cache_overflow_read;
	struct __wt_stats cache_page_read;
	struct __wt_stats cache_page_write;
	struct __wt_stats cache_pages_inuse;
};

/*
 * Statistics entries for CONNECTION handle.
 */
struct __wt_conn_stats {
	struct __wt_stats file_open;
	struct __wt_stats memalloc;
	struct __wt_stats memfree;
	struct __wt_stats mtx_lock;
	struct __wt_stats total_read_io;
	struct __wt_stats total_write_io;
	struct __wt_stats workq_passes;
	struct __wt_stats workq_yield;
};

/*
 * Statistics entries for FH handle.
 */
struct __wt_file_stats {
	struct __wt_stats fsync;
	struct __wt_stats read_io;
	struct __wt_stats write_io;
};

/* Statistics section: END */

#if defined(__cplusplus)
}
#endif
