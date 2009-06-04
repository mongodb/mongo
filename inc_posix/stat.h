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

/*
 * Statistics are optional to minimize the footprint.
 */
#ifdef	HAVE_STATISTICS

struct __wt_stats {
	u_int64_t	 v;				/* Value */
	const char	*desc;				/* Description */
};

#define	WT_STAT_SET(stats, def, str, value)				\
	(stats)[WT_STAT_ ## def].v = (value)
#define	WT_STAT_INCR(stats, def, str)					\
	++(stats)[WT_STAT_ ## def].v
#define	WT_STAT_INCRV(stats, def, str, value)				\
	(stats)[WT_STAT_ ## def].v += (value)
#define	WT_STAT_DECR(stats, def, str)					\
	--(stats)[WT_STAT_ ## def].v

/*
 * DO NOT EDIT: automatically built by dist/stat.py.
 */
/* Statistics section: BEGIN */

/*
 * Statistics entries for DB_DSTATS
 */
#define	WT_STAT_BASE_RECNO		    0
#define	WT_STAT_EXTSIZE			    1
#define	WT_STAT_FRAGSIZE		    2
#define	WT_STAT_INTLSIZE		    3
#define	WT_STAT_ITEM_DATA_OVFL		    4
#define	WT_STAT_ITEM_DUP_DATA		    5
#define	WT_STAT_ITEM_KEY_OVFL		    6
#define	WT_STAT_ITEM_TOTAL_DATA		    7
#define	WT_STAT_ITEM_TOTAL_KEY		    8
#define	WT_STAT_LEAFSIZE		    9
#define	WT_STAT_MAGIC			   10
#define	WT_STAT_MAJOR			   11
#define	WT_STAT_MINOR			   12
#define	WT_STAT_PAGE_DUP_INTERNAL	   13
#define	WT_STAT_PAGE_DUP_LEAF		   14
#define	WT_STAT_PAGE_FREE		   15
#define	WT_STAT_PAGE_INTERNAL		   16
#define	WT_STAT_PAGE_LEAF		   17
#define	WT_STAT_PAGE_OVERFLOW		   18
#define	WT_STAT_TREE_LEVEL		   19
#define	WT_STAT_DB_DSTATS_TOTAL		   20

/*
 * Statistics entries for DB_HSTATS
 */
#define	WT_STAT_BULK_DUP_DATA_READ	    0
#define	WT_STAT_BULK_OVERFLOW_DATA	    1
#define	WT_STAT_BULK_OVERFLOW_KEY	    2
#define	WT_STAT_BULK_PAIRS_READ		    3
#define	WT_STAT_DB_CACHE_ALLOC		    4
#define	WT_STAT_DB_CACHE_HIT		    5
#define	WT_STAT_DB_CACHE_MISS		    6
#define	WT_STAT_DB_HSTATS_TOTAL		    7

/*
 * Statistics entries for ENV_HSTATS
 */
#define	WT_STAT_CACHE_ALLOC		    0
#define	WT_STAT_CACHE_CLEAN		    1
#define	WT_STAT_CACHE_DIRTY		    2
#define	WT_STAT_CACHE_EVICT		    3
#define	WT_STAT_CACHE_HIT		    4
#define	WT_STAT_CACHE_MISS		    5
#define	WT_STAT_CACHE_WRITE		    6
#define	WT_STAT_CACHE_WRITE_EVICT	    7
#define	WT_STAT_ENV_HSTATS_TOTAL	    8

/*
 * Statistics entries for FH_STATS
 */
#define	WT_STAT_READ_IO			    0
#define	WT_STAT_WRITE_IO		    1
#define	WT_STAT_FH_STATS_TOTAL		    2

/*
 * Statistics entries for STOC_STATS
 */
#define	WT_STAT_STOC_ARRAY		    0
#define	WT_STAT_STOC_OPS		    1
#define	WT_STAT_STOC_SLEEP		    2
#define	WT_STAT_STOC_YIELD		    3
#define	WT_STAT_STOC_STATS_TOTAL	    4

/* Statistics section: END */

#else
#define	WT_STAT_INCR(handle, def, str)
#define	WT_STAT_DECR(handle, def, str)
#endif

#if defined(__cplusplus)
}
#endif
