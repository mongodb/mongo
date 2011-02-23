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
 * Statistics entries for DB/IDB file.
 */
#define	WT_STAT_BASE_RECNO			    0
#define	WT_STAT_FIXED_LEN			    1
#define	WT_STAT_FREELIST_ENTRIES		    2
#define	WT_STAT_INTLMAX				    3
#define	WT_STAT_INTLMIN				    4
#define	WT_STAT_ITEM_COL_DELETED		    5
#define	WT_STAT_ITEM_DATA_OVFL			    6
#define	WT_STAT_ITEM_KEY_OVFL			    7
#define	WT_STAT_ITEM_TOTAL_DATA			    8
#define	WT_STAT_ITEM_TOTAL_KEY			    9
#define	WT_STAT_LEAFMAX				   10
#define	WT_STAT_LEAFMIN				   11
#define	WT_STAT_MAGIC				   12
#define	WT_STAT_MAJOR				   13
#define	WT_STAT_MINOR				   14
#define	WT_STAT_PAGE_COL_FIX			   15
#define	WT_STAT_PAGE_COL_INTERNAL		   16
#define	WT_STAT_PAGE_COL_RLE			   17
#define	WT_STAT_PAGE_COL_VARIABLE		   18
#define	WT_STAT_PAGE_OVERFLOW			   19
#define	WT_STAT_PAGE_ROW_INTERNAL		   20
#define	WT_STAT_PAGE_ROW_LEAF			   21
#define	WT_STAT_TREE_LEVEL			   22

/*
 * Statistics entries for DB/IDB handle.
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
 * Statistics entries for ENV/IENV handle.
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
#define	WT_STAT_DB_BTREE_COMPARE_GET		    0
#define	WT_STAT_DB_BTREE_COMPARE_INT_GET	    1
#define	WT_STAT_DB_BTREE_COMPARE_INT_SET	    2
#define	WT_STAT_DB_BTREE_COMPARE_SET		    3
#define	WT_STAT_DB_BTREE_ITEMSIZE_GET		    4
#define	WT_STAT_DB_BTREE_ITEMSIZE_SET		    5
#define	WT_STAT_DB_BTREE_PAGESIZE_GET		    6
#define	WT_STAT_DB_BTREE_PAGESIZE_SET		    7
#define	WT_STAT_DB_BULK_LOAD			    8
#define	WT_STAT_DB_CLOSE			    9
#define	WT_STAT_DB_COLUMN_SET			   10
#define	WT_STAT_DB_COL_DEL			   11
#define	WT_STAT_DB_COL_DEL_RESTART		   12
#define	WT_STAT_DB_COL_GET			   13
#define	WT_STAT_DB_COL_PUT			   14
#define	WT_STAT_DB_COL_PUT_RESTART		   15
#define	WT_STAT_DB_DUMP				   16
#define	WT_STAT_DB_ERRCALL_GET			   17
#define	WT_STAT_DB_ERRCALL_SET			   18
#define	WT_STAT_DB_ERRFILE_GET			   19
#define	WT_STAT_DB_ERRFILE_SET			   20
#define	WT_STAT_DB_ERRPFX_GET			   21
#define	WT_STAT_DB_ERRPFX_SET			   22
#define	WT_STAT_DB_HUFFMAN_SET			   23
#define	WT_STAT_DB_OPEN				   24
#define	WT_STAT_DB_ROW_DEL			   25
#define	WT_STAT_DB_ROW_DEL_RESTART		   26
#define	WT_STAT_DB_ROW_GET			   27
#define	WT_STAT_DB_ROW_PUT			   28
#define	WT_STAT_DB_ROW_PUT_RESTART		   29
#define	WT_STAT_DB_STAT_CLEAR			   30
#define	WT_STAT_DB_STAT_PRINT			   31
#define	WT_STAT_DB_SYNC				   32
#define	WT_STAT_DB_VERIFY			   33
#define	WT_STAT_ENV_CACHE_SIZE_GET		   34
#define	WT_STAT_ENV_CACHE_SIZE_SET		   35
#define	WT_STAT_ENV_CLOSE			   36
#define	WT_STAT_ENV_DATA_UPDATE_INITIAL_GET	   37
#define	WT_STAT_ENV_DATA_UPDATE_INITIAL_SET	   38
#define	WT_STAT_ENV_DATA_UPDATE_MAX_GET		   39
#define	WT_STAT_ENV_DATA_UPDATE_MAX_SET		   40
#define	WT_STAT_ENV_DB				   41
#define	WT_STAT_ENV_ERRCALL_GET			   42
#define	WT_STAT_ENV_ERRCALL_SET			   43
#define	WT_STAT_ENV_ERRFILE_GET			   44
#define	WT_STAT_ENV_ERRFILE_SET			   45
#define	WT_STAT_ENV_ERRPFX_GET			   46
#define	WT_STAT_ENV_ERRPFX_SET			   47
#define	WT_STAT_ENV_HAZARD_SIZE_GET		   48
#define	WT_STAT_ENV_HAZARD_SIZE_SET		   49
#define	WT_STAT_ENV_MSGCALL_GET			   50
#define	WT_STAT_ENV_MSGCALL_SET			   51
#define	WT_STAT_ENV_MSGFILE_GET			   52
#define	WT_STAT_ENV_MSGFILE_SET			   53
#define	WT_STAT_ENV_OPEN			   54
#define	WT_STAT_ENV_STAT_CLEAR			   55
#define	WT_STAT_ENV_STAT_PRINT			   56
#define	WT_STAT_ENV_SYNC			   57
#define	WT_STAT_ENV_TOC				   58
#define	WT_STAT_ENV_TOC_SIZE_GET		   59
#define	WT_STAT_ENV_TOC_SIZE_SET		   60
#define	WT_STAT_ENV_VERBOSE_GET			   61
#define	WT_STAT_ENV_VERBOSE_SET			   62
#define	WT_STAT_WT_TOC_CLOSE			   63

/* Statistics section: END */

#if defined(__cplusplus)
}
#endif
