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

#define	WT_STAT(stats, def)						\
	(stats)[WT_STAT_ ## def].v
#define	WT_STAT_DECR(stats, def) do {					\
	--(stats)[WT_STAT_ ## def].v;					\
} while (0)
#define	WT_STAT_INCR(stats, def) do {					\
	++(stats)[WT_STAT_ ## def].v;					\
} while (0)
#define	WT_STAT_DECRV(stats, def, value) do {				\
	(stats)[WT_STAT_ ## def].v -= (value);				\
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
 * Statistics entries for BTREE handle.
 */
#define	WT_STAT_FILE_ALLOC			    0
#define	WT_STAT_FILE_EXTEND			    1
#define	WT_STAT_FILE_FREE			    2
#define	WT_STAT_FILE_HUFFMAN_DATA		    3
#define	WT_STAT_FILE_HUFFMAN_KEY		    4
#define	WT_STAT_FILE_ITEMS_INSERTED		    5
#define	WT_STAT_FILE_OVERFLOW_DATA		    6
#define	WT_STAT_FILE_OVERFLOW_KEY		    7
#define	WT_STAT_FILE_OVERFLOW_READ		    8
#define	WT_STAT_FILE_PAGE_READ			    9
#define	WT_STAT_FILE_PAGE_WRITE			   10

/*
 * Statistics entries for BTREE file.
 */
#define	WT_STAT_BASE_RECNO			    0
#define	WT_STAT_FIXED_LEN			    1
#define	WT_STAT_FREELIST_ENTRIES		    2
#define	WT_STAT_INTLMAX				    3
#define	WT_STAT_INTLMIN				    4
#define	WT_STAT_ITEM_COL_DELETED		    5
#define	WT_STAT_ITEM_TOTAL_DATA			    6
#define	WT_STAT_ITEM_TOTAL_KEY			    7
#define	WT_STAT_LEAFMAX				    8
#define	WT_STAT_LEAFMIN				    9
#define	WT_STAT_MAGIC				   10
#define	WT_STAT_MAJOR				   11
#define	WT_STAT_MINOR				   12
#define	WT_STAT_PAGE_COL_FIX			   13
#define	WT_STAT_PAGE_COL_INTERNAL		   14
#define	WT_STAT_PAGE_COL_RLE			   15
#define	WT_STAT_PAGE_COL_VARIABLE		   16
#define	WT_STAT_PAGE_OVERFLOW			   17
#define	WT_STAT_PAGE_ROW_INTERNAL		   18
#define	WT_STAT_PAGE_ROW_LEAF			   19
#define	WT_STAT_PAGE_SPLIT_INTL			   20
#define	WT_STAT_PAGE_SPLIT_LEAF			   21

/*
 * Statistics entries for CACHE handle.
 */
#define	WT_STAT_CACHE_BYTES_INUSE		    0
#define	WT_STAT_CACHE_BYTES_MAX			    1
#define	WT_STAT_CACHE_EVICT_HAZARD		    2
#define	WT_STAT_CACHE_EVICT_MODIFIED		    3
#define	WT_STAT_CACHE_EVICT_UNMODIFIED		    4
#define	WT_STAT_CACHE_OVERFLOW_READ		    5
#define	WT_STAT_CACHE_PAGES_INUSE		    6
#define	WT_STAT_CACHE_PAGE_READ			    7
#define	WT_STAT_CACHE_PAGE_WRITE		    8

/*
 * Statistics entries for CONNECTION handle.
 */
#define	WT_STAT_FILE_OPEN			    0
#define	WT_STAT_MEMALLOC			    1
#define	WT_STAT_MEMFREE				    2
#define	WT_STAT_MTX_LOCK			    3
#define	WT_STAT_TOTAL_READ_IO			    4
#define	WT_STAT_TOTAL_WRITE_IO			    5
#define	WT_STAT_WORKQ_PASSES			    6
#define	WT_STAT_WORKQ_YIELD			    7

/*
 * Statistics entries for FH handle.
 */
#define	WT_STAT_FSYNC				    0
#define	WT_STAT_READ_IO				    1
#define	WT_STAT_WRITE_IO			    2

/*
 * Statistics entries for Methods.
 */
#define	WT_STAT_BTREE_BTREE_COMPARE_GET		    0
#define	WT_STAT_BTREE_BTREE_COMPARE_INT_GET	    1
#define	WT_STAT_BTREE_BTREE_COMPARE_INT_SET	    2
#define	WT_STAT_BTREE_BTREE_COMPARE_SET		    3
#define	WT_STAT_BTREE_BTREE_ITEMSIZE_GET	    4
#define	WT_STAT_BTREE_BTREE_ITEMSIZE_SET	    5
#define	WT_STAT_BTREE_BTREE_PAGESIZE_GET	    6
#define	WT_STAT_BTREE_BTREE_PAGESIZE_SET	    7
#define	WT_STAT_BTREE_BULK_LOAD			    8
#define	WT_STAT_BTREE_CLOSE			    9
#define	WT_STAT_BTREE_COLUMN_SET		   10
#define	WT_STAT_BTREE_COL_DEL			   11
#define	WT_STAT_BTREE_COL_DEL_RESTART		   12
#define	WT_STAT_BTREE_COL_GET			   13
#define	WT_STAT_BTREE_COL_PUT			   14
#define	WT_STAT_BTREE_COL_PUT_RESTART		   15
#define	WT_STAT_BTREE_DUMP			   16
#define	WT_STAT_BTREE_HUFFMAN_SET		   17
#define	WT_STAT_BTREE_OPEN			   18
#define	WT_STAT_BTREE_ROW_DEL			   19
#define	WT_STAT_BTREE_ROW_DEL_RESTART		   20
#define	WT_STAT_BTREE_ROW_GET			   21
#define	WT_STAT_BTREE_ROW_PUT			   22
#define	WT_STAT_BTREE_ROW_PUT_RESTART		   23
#define	WT_STAT_BTREE_SALVAGE			   24
#define	WT_STAT_BTREE_STAT_CLEAR		   25
#define	WT_STAT_BTREE_STAT_PRINT		   26
#define	WT_STAT_BTREE_SYNC			   27
#define	WT_STAT_BTREE_VERIFY			   28
#define	WT_STAT_CONNECTION_BTREE		   29
#define	WT_STAT_CONNECTION_CACHE_SIZE_GET	   30
#define	WT_STAT_CONNECTION_CACHE_SIZE_SET	   31
#define	WT_STAT_CONNECTION_CLOSE		   32
#define	WT_STAT_CONNECTION_HAZARD_SIZE_GET	   33
#define	WT_STAT_CONNECTION_HAZARD_SIZE_SET	   34
#define	WT_STAT_CONNECTION_MSGCALL_GET		   35
#define	WT_STAT_CONNECTION_MSGCALL_SET		   36
#define	WT_STAT_CONNECTION_MSGFILE_GET		   37
#define	WT_STAT_CONNECTION_MSGFILE_SET		   38
#define	WT_STAT_CONNECTION_OPEN			   39
#define	WT_STAT_CONNECTION_SESSION		   40
#define	WT_STAT_CONNECTION_SESSION_SIZE_GET	   41
#define	WT_STAT_CONNECTION_SESSION_SIZE_SET	   42
#define	WT_STAT_CONNECTION_STAT_CLEAR		   43
#define	WT_STAT_CONNECTION_STAT_PRINT		   44
#define	WT_STAT_CONNECTION_SYNC			   45
#define	WT_STAT_CONNECTION_VERBOSE_GET		   46
#define	WT_STAT_CONNECTION_VERBOSE_SET		   47
#define	WT_STAT_SESSION_CLOSE			   48

/* Statistics section: END */

#if defined(__cplusplus)
}
#endif
