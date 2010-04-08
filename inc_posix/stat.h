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

#define	WT_STAT(stats, def)						\
	(stats)[WT_STAT_ ## def].v
#define	WT_STAT_INCR(stats, def) do {					\
	++(stats)[WT_STAT_ ## def].v;					\
} while (0)
#define	WT_STAT_INCRV(stats, def, value) do {				\
	(stats)[WT_STAT_ ## def].v += (value);				\
} while (0)
#define	WT_STAT_SET(stats, def, value) do {				\
	(stats)[WT_STAT_ ## def].v = (value);				\
} while (0)

/*
 * DO NOT EDIT: automatically built by dist/stat.py.
 */
/* Statistics section: BEGIN */

/*
 * Statistics entries for CACHE handle.
 */
#define	WT_STAT_CACHE_ALLOC			    0
#define	WT_STAT_CACHE_BYTES_INUSE		    1
#define	WT_STAT_CACHE_BYTES_MAX			    2
#define	WT_STAT_CACHE_EVICT			    3
#define	WT_STAT_CACHE_EVICT_HAZARD		    4
#define	WT_STAT_CACHE_EVICT_MODIFIED		    5
#define	WT_STAT_CACHE_HASH_BUCKETS		    6
#define	WT_STAT_CACHE_HIT			    7
#define	WT_STAT_CACHE_MAX_BUCKET_ENTRIES	    8
#define	WT_STAT_CACHE_MISS			    9
#define	WT_STAT_CACHE_PAGES_INUSE		   10
#define	WT_STAT_CACHE_READ_LOCKOUT		   11

/*
 * Statistics entries for DB/IDB database.
 */
#define	WT_STAT_BASE_RECNO			    0
#define	WT_STAT_EXTSIZE				    1
#define	WT_STAT_FIXED_LEN			    2
#define	WT_STAT_FRAGSIZE			    3
#define	WT_STAT_INTLSIZE			    4
#define	WT_STAT_ITEM_DATA_OVFL			    5
#define	WT_STAT_ITEM_DUP_DATA			    6
#define	WT_STAT_ITEM_KEY_OVFL			    7
#define	WT_STAT_ITEM_TOTAL_DATA			    8
#define	WT_STAT_ITEM_TOTAL_KEY			    9
#define	WT_STAT_LEAFSIZE			   10
#define	WT_STAT_MAGIC				   11
#define	WT_STAT_MAJOR				   12
#define	WT_STAT_MINOR				   13
#define	WT_STAT_PAGE_COL_FIXED			   14
#define	WT_STAT_PAGE_COL_INTERNAL		   15
#define	WT_STAT_PAGE_COL_VARIABLE		   16
#define	WT_STAT_PAGE_DUP_INTERNAL		   17
#define	WT_STAT_PAGE_DUP_LEAF			   18
#define	WT_STAT_PAGE_FREE			   19
#define	WT_STAT_PAGE_INTERNAL			   20
#define	WT_STAT_PAGE_LEAF			   21
#define	WT_STAT_PAGE_OVERFLOW			   22
#define	WT_STAT_TREE_LEVEL			   23

/*
 * Statistics entries for DB/IDB handle.
 */
#define	WT_STAT_BULK_DUP_DATA_READ		    0
#define	WT_STAT_BULK_HUFFMAN_DATA		    1
#define	WT_STAT_BULK_HUFFMAN_KEY		    2
#define	WT_STAT_BULK_OVERFLOW_DATA		    3
#define	WT_STAT_BULK_OVERFLOW_KEY		    4
#define	WT_STAT_BULK_PAIRS_READ			    5
#define	WT_STAT_BULK_REPEAT_COUNT		    6
#define	WT_STAT_DB_CACHE_ALLOC			    7
#define	WT_STAT_DB_CACHE_HIT			    8
#define	WT_STAT_DB_CACHE_MISS			    9

/*
 * Statistics entries for ENV/IENV handle.
 */
#define	WT_STAT_DATABASE_OPEN			    0
#define	WT_STAT_MEMALLOC			    1
#define	WT_STAT_MEMFREE				    2
#define	WT_STAT_MTX_LOCK			    3
#define	WT_STAT_TOTAL_READ_IO			    4
#define	WT_STAT_TOTAL_WRITE_IO			    5
#define	WT_STAT_WORKQ_PASSES			    6
#define	WT_STAT_WORKQ_SLEEP			    7
#define	WT_STAT_WORKQ_YIELD			    8

/*
 * Statistics entries for FH handle.
 */
#define	WT_STAT_READ_IO				    0
#define	WT_STAT_WRITE_IO			    1

/*
 * Statistics entries for Methods.
 */
#define	WT_STAT_DB_BTREE_COMPARE_DUP_GET	    0
#define	WT_STAT_DB_BTREE_COMPARE_DUP_SET	    1
#define	WT_STAT_DB_BTREE_COMPARE_GET		    2
#define	WT_STAT_DB_BTREE_COMPARE_INT_GET	    3
#define	WT_STAT_DB_BTREE_COMPARE_INT_SET	    4
#define	WT_STAT_DB_BTREE_COMPARE_SET		    5
#define	WT_STAT_DB_BTREE_DUP_OFFPAGE_GET	    6
#define	WT_STAT_DB_BTREE_DUP_OFFPAGE_SET	    7
#define	WT_STAT_DB_BTREE_ITEMSIZE_GET		    8
#define	WT_STAT_DB_BTREE_ITEMSIZE_SET		    9
#define	WT_STAT_DB_BTREE_PAGESIZE_GET		   10
#define	WT_STAT_DB_BTREE_PAGESIZE_SET		   11
#define	WT_STAT_DB_BULK_LOAD			   12
#define	WT_STAT_DB_CLOSE			   13
#define	WT_STAT_DB_COLUMN_SET			   14
#define	WT_STAT_DB_COL_DEL			   15
#define	WT_STAT_DB_COL_GET			   16
#define	WT_STAT_DB_COL_PUT			   17
#define	WT_STAT_DB_DUMP				   18
#define	WT_STAT_DB_ERR				   19
#define	WT_STAT_DB_ERRCALL_GET			   20
#define	WT_STAT_DB_ERRCALL_SET			   21
#define	WT_STAT_DB_ERRFILE_GET			   22
#define	WT_STAT_DB_ERRFILE_SET			   23
#define	WT_STAT_DB_ERRPFX_GET			   24
#define	WT_STAT_DB_ERRPFX_SET			   25
#define	WT_STAT_DB_ERRX				   26
#define	WT_STAT_DB_HUFFMAN_SET			   27
#define	WT_STAT_DB_OPEN				   28
#define	WT_STAT_DB_ROW_DEL			   29
#define	WT_STAT_DB_ROW_GET			   30
#define	WT_STAT_DB_ROW_PUT			   31
#define	WT_STAT_DB_STAT_CLEAR			   32
#define	WT_STAT_DB_STAT_PRINT			   33
#define	WT_STAT_DB_SYNC				   34
#define	WT_STAT_DB_VERIFY			   35
#define	WT_STAT_ENV_CACHE_HASH_SIZE_GET		   36
#define	WT_STAT_ENV_CACHE_HASH_SIZE_SET		   37
#define	WT_STAT_ENV_CACHE_SIZE_GET		   38
#define	WT_STAT_ENV_CACHE_SIZE_SET		   39
#define	WT_STAT_ENV_CLOSE			   40
#define	WT_STAT_ENV_DB				   41
#define	WT_STAT_ENV_ERR				   42
#define	WT_STAT_ENV_ERRCALL_GET			   43
#define	WT_STAT_ENV_ERRCALL_SET			   44
#define	WT_STAT_ENV_ERRFILE_GET			   45
#define	WT_STAT_ENV_ERRFILE_SET			   46
#define	WT_STAT_ENV_ERRPFX_GET			   47
#define	WT_STAT_ENV_ERRPFX_SET			   48
#define	WT_STAT_ENV_ERRX			   49
#define	WT_STAT_ENV_HAZARD_SIZE_GET		   50
#define	WT_STAT_ENV_HAZARD_SIZE_SET		   51
#define	WT_STAT_ENV_MSGCALL_GET			   52
#define	WT_STAT_ENV_MSGCALL_SET			   53
#define	WT_STAT_ENV_MSGFILE_GET			   54
#define	WT_STAT_ENV_MSGFILE_SET			   55
#define	WT_STAT_ENV_OPEN			   56
#define	WT_STAT_ENV_STAT_CLEAR			   57
#define	WT_STAT_ENV_STAT_PRINT			   58
#define	WT_STAT_ENV_SYNC			   59
#define	WT_STAT_ENV_TOC				   60
#define	WT_STAT_ENV_TOC_SIZE_GET		   61
#define	WT_STAT_ENV_TOC_SIZE_SET		   62
#define	WT_STAT_ENV_VERBOSE_GET			   63
#define	WT_STAT_ENV_VERBOSE_SET			   64
#define	WT_STAT_WT_TOC_CLOSE			   65

/* Statistics section: END */

#if defined(__cplusplus)
}
#endif
