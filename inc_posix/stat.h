/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#if defined(__cplusplus)
extern "C" {
#endif

struct __wt_stats {
	u_int64_t	 v;				/* 64-bit value */
	const char	*desc;				/* text description */
};

#define	WT_STAT_SET(stats, def, value)					\
	(stats)[WT_STAT_ ## def].v = (value)
#define	WT_STAT_INCR(stats, def)					\
	++(stats)[WT_STAT_ ## def].v
#define	WT_STAT_INCRV(stats, def, value)				\
	(stats)[WT_STAT_ ## def].v += (value)
#define	WT_STAT(stats, def)						\
	(stats)[WT_STAT_ ## def].v

/*
 * DO NOT EDIT: automatically built by dist/stat.py.
 */
/* Statistics section: BEGIN */

/*
 * Statistics entries for CACHE handle.
 */
#define	WT_STAT_CACHE_TOTAL		   12

#define	WT_STAT_CACHE_ALLOC		    0
#define	WT_STAT_CACHE_BYTES_INUSE	    1
#define	WT_STAT_CACHE_BYTES_MAX		    2
#define	WT_STAT_CACHE_EVICT		    3
#define	WT_STAT_CACHE_HAZARD_EVICT	    4
#define	WT_STAT_CACHE_HIT		    5
#define	WT_STAT_CACHE_MAX_BUCKET_ENTRIES	    6
#define	WT_STAT_CACHE_MISS		    7
#define	WT_STAT_CACHE_PAGES_INUSE	    8
#define	WT_STAT_CACHE_READ_LOCKOUT	    9
#define	WT_STAT_CACHE_WRITE		   10
#define	WT_STAT_CACHE_WRITE_EVICT	   11

/*
 * Statistics entries for DB/IDB database.
 */
#define	WT_STAT_DATABASE_TOTAL		   24

#define	WT_STAT_BASE_RECNO		    0
#define	WT_STAT_EXTSIZE			    1
#define	WT_STAT_FIXED_LEN		    2
#define	WT_STAT_FRAGSIZE		    3
#define	WT_STAT_INTLSIZE		    4
#define	WT_STAT_ITEM_DATA_OVFL		    5
#define	WT_STAT_ITEM_DUP_DATA		    6
#define	WT_STAT_ITEM_KEY_OVFL		    7
#define	WT_STAT_ITEM_TOTAL_DATA		    8
#define	WT_STAT_ITEM_TOTAL_KEY		    9
#define	WT_STAT_LEAFSIZE		   10
#define	WT_STAT_MAGIC			   11
#define	WT_STAT_MAJOR			   12
#define	WT_STAT_MINOR			   13
#define	WT_STAT_PAGE_COL_FIXED		   14
#define	WT_STAT_PAGE_COL_INTERNAL	   15
#define	WT_STAT_PAGE_COL_VARIABLE	   16
#define	WT_STAT_PAGE_DUP_INTERNAL	   17
#define	WT_STAT_PAGE_DUP_LEAF		   18
#define	WT_STAT_PAGE_FREE		   19
#define	WT_STAT_PAGE_INTERNAL		   20
#define	WT_STAT_PAGE_LEAF		   21
#define	WT_STAT_PAGE_OVERFLOW		   22
#define	WT_STAT_TREE_LEVEL		   23

/*
 * Statistics entries for DB/IDB handle.
 */
#define	WT_STAT_DB_TOTAL		   14

#define	WT_STAT_BULK_DUP_DATA_READ	    0
#define	WT_STAT_BULK_HUFFMAN_DATA	    1
#define	WT_STAT_BULK_HUFFMAN_KEY	    2
#define	WT_STAT_BULK_OVERFLOW_DATA	    3
#define	WT_STAT_BULK_OVERFLOW_KEY	    4
#define	WT_STAT_BULK_PAIRS_READ		    5
#define	WT_STAT_BULK_REPEAT_COUNT	    6
#define	WT_STAT_DB_CACHE_ALLOC		    7
#define	WT_STAT_DB_CACHE_HIT		    8
#define	WT_STAT_DB_CACHE_MISS		    9
#define	WT_STAT_DB_DELETE_BY_KEY	   10
#define	WT_STAT_DB_READ_BY_KEY		   11
#define	WT_STAT_DB_READ_BY_RECNO	   12
#define	WT_STAT_DB_WRITE_BY_KEY		   13

/*
 * Statistics entries for ENV/IENV handle.
 */
#define	WT_STAT_ENV_TOTAL		   11

#define	WT_STAT_DATABASE_OPEN		    0
#define	WT_STAT_HASH_BUCKETS		    1
#define	WT_STAT_LONGEST_BUCKET		    2
#define	WT_STAT_MEMALLOC		    3
#define	WT_STAT_MEMFREE			    4
#define	WT_STAT_MTX_LOCK		    5
#define	WT_STAT_TOTAL_READ_IO		    6
#define	WT_STAT_TOTAL_WRITE_IO		    7
#define	WT_STAT_WORKQ_PASSES		    8
#define	WT_STAT_WORKQ_SLEEP		    9
#define	WT_STAT_WORKQ_YIELD		   10

/*
 * Statistics entries for FH handle.
 */
#define	WT_STAT_FH_TOTAL		    2

#define	WT_STAT_READ_IO			    0
#define	WT_STAT_WRITE_IO		    1

/* Statistics section: END */

#if defined(__cplusplus)
}
#endif
