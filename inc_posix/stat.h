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

#define	WT_STAT_SET(handle, def, str, value) do {			\
	(handle)->stats[WT_STAT_ ## def].v = (value);			\
} while (0)
#define	WT_STAT_INCR(handle, def, str) do {				\
	++(handle)->stats[WT_STAT_ ## def].v;				\
} while (0)
#define	WT_STAT_DECR(handle, def, str) do {				\
	--(handle)->stats[WT_STAT_ ## def].v;				\
} while (0)

/*
 * DO NOT EDIT: automatically built by dist/stat.py.
 */
/* Statistics section: BEGIN */

/*
 * Statistics entries for the FH handle.
 */
#define	WT_STAT_READ_IO			    0
#define	WT_STAT_WRITE_IO		    1
#define	WT_STAT_FH_TOTAL_ENTRIES	    2

/*
 * Statistics entries for the DB handle.
 */
#define	WT_STAT_BULK_DUP_DATA_READ	    0
#define	WT_STAT_BULK_OVERFLOW_DATA	    1
#define	WT_STAT_BULK_OVERFLOW_KEY	    2
#define	WT_STAT_BULK_PAIRS_READ		    3
#define	WT_STAT_DB_CACHE_ALLOC		    4
#define	WT_STAT_DB_CACHE_CLEAN		    5
#define	WT_STAT_DB_CACHE_DIRTY		    6
#define	WT_STAT_DB_CACHE_HIT		    7
#define	WT_STAT_DB_CACHE_MISS		    8
#define	WT_STAT_EXTSIZE			    9
#define	WT_STAT_FRAGSIZE		   10
#define	WT_STAT_INTLSIZE		   11
#define	WT_STAT_LEAFSIZE		   12
#define	WT_STAT_DB_TOTAL_ENTRIES	   13

/*
 * Statistics entries for the ENV handle.
 */
#define	WT_STAT_CACHE_ALLOC		    0
#define	WT_STAT_CACHE_CLEAN		    1
#define	WT_STAT_CACHE_DIRTY		    2
#define	WT_STAT_CACHE_EVICT		    3
#define	WT_STAT_CACHE_HIT		    4
#define	WT_STAT_CACHE_MISS		    5
#define	WT_STAT_CACHE_WRITE		    6
#define	WT_STAT_CACHE_WRITE_EVICT	    7
#define	WT_STAT_ENV_TOTAL_ENTRIES	    8

/* Statistics section: END */

#else
#define	WT_STAT_INCR(handle, def, str)
#define	WT_STAT_DECR(handle, def, str)
#endif

#if defined(__cplusplus)
}
#endif
