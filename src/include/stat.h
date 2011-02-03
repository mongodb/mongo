/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
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
#define	WT_STAT_CACHE_PAGES_INUSE		    5
#define	WT_STAT_OVERFLOW_READ			    6
#define	WT_STAT_PAGE_READ			    7
#define	WT_STAT_PAGE_WRITE			    8

/*
 * Statistics entries for DB/IDB database.
 */
#define	WT_STAT_BASE_RECNO			    0
#define	WT_STAT_DUP_TREE			    1
#define	WT_STAT_FIXED_LEN			    2
#define	WT_STAT_FREELIST_ENTRIES		    3
#define	WT_STAT_INTLMAX				    4
#define	WT_STAT_INTLMIN				    5
#define	WT_STAT_ITEM_COL_DELETED		    6
#define	WT_STAT_ITEM_DATA_OVFL			    7
#define	WT_STAT_ITEM_DUP_DATA			    8
#define	WT_STAT_ITEM_KEY_OVFL			    9
#define	WT_STAT_ITEM_TOTAL_DATA			   10
#define	WT_STAT_ITEM_TOTAL_KEY			   11
#define	WT_STAT_LEAFMAX				   12
#define	WT_STAT_LEAFMIN				   13
#define	WT_STAT_MAGIC				   14
#define	WT_STAT_MAJOR				   15
#define	WT_STAT_MINOR				   16
#define	WT_STAT_PAGE_COL_FIX			   17
#define	WT_STAT_PAGE_COL_INTERNAL		   18
#define	WT_STAT_PAGE_COL_RLE			   19
#define	WT_STAT_PAGE_COL_VARIABLE		   20
#define	WT_STAT_PAGE_DUP_INTERNAL		   21
#define	WT_STAT_PAGE_DUP_LEAF			   22
#define	WT_STAT_PAGE_OVERFLOW			   23
#define	WT_STAT_PAGE_ROW_INTERNAL		   24
#define	WT_STAT_PAGE_ROW_LEAF			   25
#define	WT_STAT_TREE_LEVEL			   26

/*
 * Statistics entries for DB/IDB handle.
 */
#define	WT_STAT_DB_ALLOC			    0
#define	WT_STAT_DB_ALLOC_FILE			    1
#define	WT_STAT_DB_FREE				    2
#define	WT_STAT_DUPLICATE_ITEMS_INSERTED	    3
#define	WT_STAT_HUFFMAN_DATA			    4
#define	WT_STAT_HUFFMAN_KEY			    5
#define	WT_STAT_ITEMS_INSERTED			    6
#define	WT_STAT_OVERFLOW_DATA			    7
#define	WT_STAT_OVERFLOW_KEY			    8
#define	WT_STAT_REPEAT_COUNT			    9

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
#define	WT_STAT_DB_COL_DEL_RESTART		   16
#define	WT_STAT_DB_COL_GET			   17
#define	WT_STAT_DB_COL_PUT			   18
#define	WT_STAT_DB_COL_PUT_RESTART		   19
#define	WT_STAT_DB_DUMP				   20
#define	WT_STAT_DB_ERRCALL_GET			   21
#define	WT_STAT_DB_ERRCALL_SET			   22
#define	WT_STAT_DB_ERRFILE_GET			   23
#define	WT_STAT_DB_ERRFILE_SET			   24
#define	WT_STAT_DB_ERRPFX_GET			   25
#define	WT_STAT_DB_ERRPFX_SET			   26
#define	WT_STAT_DB_HUFFMAN_SET			   27
#define	WT_STAT_DB_OPEN				   28
#define	WT_STAT_DB_ROW_DEL			   29
#define	WT_STAT_DB_ROW_DEL_RESTART		   30
#define	WT_STAT_DB_ROW_GET			   31
#define	WT_STAT_DB_ROW_PUT			   32
#define	WT_STAT_DB_ROW_PUT_RESTART		   33
#define	WT_STAT_DB_STAT_CLEAR			   34
#define	WT_STAT_DB_STAT_PRINT			   35
#define	WT_STAT_DB_SYNC				   36
#define	WT_STAT_DB_VERIFY			   37
#define	WT_STAT_ENV_CACHE_SIZE_GET		   38
#define	WT_STAT_ENV_CACHE_SIZE_SET		   39
#define	WT_STAT_ENV_CLOSE			   40
#define	WT_STAT_ENV_DATA_UPDATE_INITIAL_GET	   41
#define	WT_STAT_ENV_DATA_UPDATE_INITIAL_SET	   42
#define	WT_STAT_ENV_DATA_UPDATE_MAX_GET		   43
#define	WT_STAT_ENV_DATA_UPDATE_MAX_SET		   44
#define	WT_STAT_ENV_DB				   45
#define	WT_STAT_ENV_ERRCALL_GET			   46
#define	WT_STAT_ENV_ERRCALL_SET			   47
#define	WT_STAT_ENV_ERRFILE_GET			   48
#define	WT_STAT_ENV_ERRFILE_SET			   49
#define	WT_STAT_ENV_ERRPFX_GET			   50
#define	WT_STAT_ENV_ERRPFX_SET			   51
#define	WT_STAT_ENV_HAZARD_SIZE_GET		   52
#define	WT_STAT_ENV_HAZARD_SIZE_SET		   53
#define	WT_STAT_ENV_MSGCALL_GET			   54
#define	WT_STAT_ENV_MSGCALL_SET			   55
#define	WT_STAT_ENV_MSGFILE_GET			   56
#define	WT_STAT_ENV_MSGFILE_SET			   57
#define	WT_STAT_ENV_OPEN			   58
#define	WT_STAT_ENV_STAT_CLEAR			   59
#define	WT_STAT_ENV_STAT_PRINT			   60
#define	WT_STAT_ENV_SYNC			   61
#define	WT_STAT_ENV_TOC				   62
#define	WT_STAT_ENV_TOC_SIZE_GET		   63
#define	WT_STAT_ENV_TOC_SIZE_SET		   64
#define	WT_STAT_ENV_VERBOSE_GET			   65
#define	WT_STAT_ENV_VERBOSE_SET			   66
#define	WT_STAT_WT_TOC_CLOSE			   67

/* Statistics section: END */

#if defined(__cplusplus)
}
#endif
