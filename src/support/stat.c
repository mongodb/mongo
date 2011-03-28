/* DO NOT EDIT: automatically built by dist/stat.py. */

#include "wt_internal.h"

int
__wt_stat_alloc_btree_handle_stats(SESSION *session, WT_STATS **statsp)
{
	WT_STATS *stats;

	WT_RET(__wt_calloc(session, 12, sizeof(WT_STATS), &stats));

	stats[WT_STAT_FILE_ALLOC].desc = "file: block allocations";
	stats[WT_STAT_FILE_EXTEND].desc =
	    "file: block allocations require file extension";
	stats[WT_STAT_FILE_FREE].desc = "file: block frees";
	stats[WT_STAT_FILE_HUFFMAN_DATA].desc =
	    "file: huffman data compression in bytes";
	stats[WT_STAT_FILE_HUFFMAN_KEY].desc =
	    "file: huffman key compression in bytes";
	stats[WT_STAT_FILE_ITEMS_INSERTED].desc =
	    "file: key/data pairs inserted";
	stats[WT_STAT_FILE_OVERFLOW_DATA].desc =
	    "file: overflow data items inserted";
	stats[WT_STAT_FILE_OVERFLOW_KEY].desc =
	    "file: overflow key items inserted";
	stats[WT_STAT_FILE_OVERFLOW_READ].desc =
	    "file: overflow pages read from the file";
	stats[WT_STAT_FILE_PAGE_READ].desc = "file: pages read from a file";
	stats[WT_STAT_FILE_PAGE_WRITE].desc = "file: pages written to a file";

	*statsp = stats;
	return (0);
}

void
__wt_stat_clear_btree_handle_stats(WT_STATS *stats)
{
	stats[WT_STAT_FILE_ALLOC].v = 0;
	stats[WT_STAT_FILE_EXTEND].v = 0;
	stats[WT_STAT_FILE_FREE].v = 0;
	stats[WT_STAT_FILE_HUFFMAN_DATA].v = 0;
	stats[WT_STAT_FILE_HUFFMAN_KEY].v = 0;
	stats[WT_STAT_FILE_ITEMS_INSERTED].v = 0;
	stats[WT_STAT_FILE_OVERFLOW_DATA].v = 0;
	stats[WT_STAT_FILE_OVERFLOW_KEY].v = 0;
	stats[WT_STAT_FILE_OVERFLOW_READ].v = 0;
	stats[WT_STAT_FILE_PAGE_READ].v = 0;
	stats[WT_STAT_FILE_PAGE_WRITE].v = 0;
}

int
__wt_stat_alloc_btree_file_stats(SESSION *session, WT_STATS **statsp)
{
	WT_STATS *stats;

	WT_RET(__wt_calloc(session, 23, sizeof(WT_STATS), &stats));

	stats[WT_STAT_BASE_RECNO].desc = "base record number";
	stats[WT_STAT_FIXED_LEN].desc = "fixed-record size";
	stats[WT_STAT_FREELIST_ENTRIES].desc =
	    "number of entries in the freelist";
	stats[WT_STAT_INTLMAX].desc = "maximum internal page size";
	stats[WT_STAT_INTLMIN].desc = "minimum internal page size";
	stats[WT_STAT_ITEM_COL_DELETED].desc =
	    "column-store deleted data items";
	stats[WT_STAT_ITEM_TOTAL_DATA].desc = "total data items";
	stats[WT_STAT_ITEM_TOTAL_KEY].desc = "total keys";
	stats[WT_STAT_LEAFMAX].desc = "maximum leaf page size";
	stats[WT_STAT_LEAFMIN].desc = "minimum leaf page size";
	stats[WT_STAT_MAGIC].desc = "magic number";
	stats[WT_STAT_MAJOR].desc = "major version number";
	stats[WT_STAT_MINOR].desc = "minor version number";
	stats[WT_STAT_PAGE_COL_FIX].desc =
	    "column-store fixed-size leaf pages";
	stats[WT_STAT_PAGE_COL_INTERNAL].desc = "column-store internal pages";
	stats[WT_STAT_PAGE_COL_RLE].desc =
	    "column-store repeat-count compressed fixed-size leaf pages";
	stats[WT_STAT_PAGE_COL_VARIABLE].desc =
	    "column-store variable-size leaf pages";
	stats[WT_STAT_PAGE_OVERFLOW].desc = "overflow pages";
	stats[WT_STAT_PAGE_ROW_INTERNAL].desc = "row-store internal pages";
	stats[WT_STAT_PAGE_ROW_LEAF].desc = "row-store leaf pages";
	stats[WT_STAT_PAGE_SPLIT_INTL].desc = "split internal pages";
	stats[WT_STAT_PAGE_SPLIT_LEAF].desc = "split leaf pages";

	*statsp = stats;
	return (0);
}

void
__wt_stat_clear_btree_file_stats(WT_STATS *stats)
{
	stats[WT_STAT_BASE_RECNO].v = 0;
	stats[WT_STAT_FIXED_LEN].v = 0;
	stats[WT_STAT_FREELIST_ENTRIES].v = 0;
	stats[WT_STAT_INTLMAX].v = 0;
	stats[WT_STAT_INTLMIN].v = 0;
	stats[WT_STAT_ITEM_COL_DELETED].v = 0;
	stats[WT_STAT_ITEM_TOTAL_DATA].v = 0;
	stats[WT_STAT_ITEM_TOTAL_KEY].v = 0;
	stats[WT_STAT_LEAFMAX].v = 0;
	stats[WT_STAT_LEAFMIN].v = 0;
	stats[WT_STAT_MAGIC].v = 0;
	stats[WT_STAT_MAJOR].v = 0;
	stats[WT_STAT_MINOR].v = 0;
	stats[WT_STAT_PAGE_COL_FIX].v = 0;
	stats[WT_STAT_PAGE_COL_INTERNAL].v = 0;
	stats[WT_STAT_PAGE_COL_RLE].v = 0;
	stats[WT_STAT_PAGE_COL_VARIABLE].v = 0;
	stats[WT_STAT_PAGE_OVERFLOW].v = 0;
	stats[WT_STAT_PAGE_ROW_INTERNAL].v = 0;
	stats[WT_STAT_PAGE_ROW_LEAF].v = 0;
	stats[WT_STAT_PAGE_SPLIT_INTL].v = 0;
	stats[WT_STAT_PAGE_SPLIT_LEAF].v = 0;
}

int
__wt_stat_alloc_cache_stats(SESSION *session, WT_STATS **statsp)
{
	WT_STATS *stats;

	WT_RET(__wt_calloc(session, 10, sizeof(WT_STATS), &stats));

	stats[WT_STAT_CACHE_BYTES_INUSE].desc =
	    "cache: bytes currently held in the cache";
	stats[WT_STAT_CACHE_BYTES_MAX].desc =
	    "cache: maximum bytes configured";
	stats[WT_STAT_CACHE_EVICT_HAZARD].desc =
	    "cache: pages selected for eviction not evicted because of a hazard reference";
	stats[WT_STAT_CACHE_EVICT_MODIFIED].desc =
	    "cache: modified pages selected for eviction";
	stats[WT_STAT_CACHE_EVICT_UNMODIFIED].desc =
	    "cache: unmodified pages selected for eviction";
	stats[WT_STAT_CACHE_OVERFLOW_READ].desc =
	    "cache: overflow pages read from the file";
	stats[WT_STAT_CACHE_PAGES_INUSE].desc =
	    "cache: pages currently held in the cache";
	stats[WT_STAT_CACHE_PAGE_READ].desc = "cache: pages read from a file";
	stats[WT_STAT_CACHE_PAGE_WRITE].desc =
	    "cache: pages written to a file";

	*statsp = stats;
	return (0);
}

void
__wt_stat_clear_cache_stats(WT_STATS *stats)
{
	stats[WT_STAT_CACHE_EVICT_HAZARD].v = 0;
	stats[WT_STAT_CACHE_EVICT_MODIFIED].v = 0;
	stats[WT_STAT_CACHE_EVICT_UNMODIFIED].v = 0;
	stats[WT_STAT_CACHE_OVERFLOW_READ].v = 0;
	stats[WT_STAT_CACHE_PAGE_READ].v = 0;
	stats[WT_STAT_CACHE_PAGE_WRITE].v = 0;
}

int
__wt_stat_alloc_connection_stats(SESSION *session, WT_STATS **statsp)
{
	WT_STATS *stats;

	WT_RET(__wt_calloc(session, 9, sizeof(WT_STATS), &stats));

	stats[WT_STAT_FILE_OPEN].desc = "file open";
	stats[WT_STAT_MEMALLOC].desc = "memory allocations";
	stats[WT_STAT_MEMFREE].desc = "memory frees";
	stats[WT_STAT_MTX_LOCK].desc = "mutex lock calls";
	stats[WT_STAT_TOTAL_READ_IO].desc = "total read I/Os";
	stats[WT_STAT_TOTAL_WRITE_IO].desc = "total write I/Os";
	stats[WT_STAT_WORKQ_PASSES].desc = "workQ queue passes";
	stats[WT_STAT_WORKQ_YIELD].desc = "workQ yields";

	*statsp = stats;
	return (0);
}

void
__wt_stat_clear_connection_stats(WT_STATS *stats)
{
	stats[WT_STAT_FILE_OPEN].v = 0;
	stats[WT_STAT_MEMALLOC].v = 0;
	stats[WT_STAT_MEMFREE].v = 0;
	stats[WT_STAT_MTX_LOCK].v = 0;
	stats[WT_STAT_TOTAL_READ_IO].v = 0;
	stats[WT_STAT_TOTAL_WRITE_IO].v = 0;
	stats[WT_STAT_WORKQ_PASSES].v = 0;
	stats[WT_STAT_WORKQ_YIELD].v = 0;
}

int
__wt_stat_alloc_fh_stats(SESSION *session, WT_STATS **statsp)
{
	WT_STATS *stats;

	WT_RET(__wt_calloc(session, 4, sizeof(WT_STATS), &stats));

	stats[WT_STAT_FSYNC].desc = "fsyncs";
	stats[WT_STAT_READ_IO].desc = "read I/Os";
	stats[WT_STAT_WRITE_IO].desc = "write I/Os";

	*statsp = stats;
	return (0);
}

void
__wt_stat_clear_fh_stats(WT_STATS *stats)
{
	stats[WT_STAT_FSYNC].v = 0;
	stats[WT_STAT_READ_IO].v = 0;
	stats[WT_STAT_WRITE_IO].v = 0;
}

int
__wt_stat_alloc_method_stats(SESSION *session, WT_STATS **statsp)
{
	WT_STATS *stats;

	WT_RET(__wt_calloc(session, 49, sizeof(WT_STATS), &stats));

	stats[WT_STAT_BTREE_BTREE_COMPARE_GET].desc =
	    "btree.btree_compare_get";
	stats[WT_STAT_BTREE_BTREE_COMPARE_INT_GET].desc =
	    "btree.btree_compare_int_get";
	stats[WT_STAT_BTREE_BTREE_COMPARE_INT_SET].desc =
	    "btree.btree_compare_int_set";
	stats[WT_STAT_BTREE_BTREE_COMPARE_SET].desc =
	    "btree.btree_compare_set";
	stats[WT_STAT_BTREE_BTREE_ITEMSIZE_GET].desc =
	    "btree.btree_itemsize_get";
	stats[WT_STAT_BTREE_BTREE_ITEMSIZE_SET].desc =
	    "btree.btree_itemsize_set";
	stats[WT_STAT_BTREE_BTREE_PAGESIZE_GET].desc =
	    "btree.btree_pagesize_get";
	stats[WT_STAT_BTREE_BTREE_PAGESIZE_SET].desc =
	    "btree.btree_pagesize_set";
	stats[WT_STAT_BTREE_BULK_LOAD].desc = "btree.bulk_load";
	stats[WT_STAT_BTREE_CLOSE].desc = "btree.close";
	stats[WT_STAT_BTREE_COLUMN_SET].desc = "btree.column_set";
	stats[WT_STAT_BTREE_COL_DEL].desc = "btree.col_del";
	stats[WT_STAT_BTREE_COL_DEL_RESTART].desc =
	    "btree.col_del method restarts";
	stats[WT_STAT_BTREE_COL_GET].desc = "btree.col_get";
	stats[WT_STAT_BTREE_COL_PUT].desc = "btree.col_put";
	stats[WT_STAT_BTREE_COL_PUT_RESTART].desc =
	    "btree.col_put method restarts";
	stats[WT_STAT_BTREE_DUMP].desc = "btree.dump";
	stats[WT_STAT_BTREE_HUFFMAN_SET].desc = "btree.huffman_set";
	stats[WT_STAT_BTREE_OPEN].desc = "btree.open";
	stats[WT_STAT_BTREE_ROW_DEL].desc = "btree.row_del";
	stats[WT_STAT_BTREE_ROW_DEL_RESTART].desc =
	    "btree.row_del method restarts";
	stats[WT_STAT_BTREE_ROW_GET].desc = "btree.row_get";
	stats[WT_STAT_BTREE_ROW_PUT].desc = "btree.row_put";
	stats[WT_STAT_BTREE_ROW_PUT_RESTART].desc =
	    "btree.row_put method restarts";
	stats[WT_STAT_BTREE_STAT_CLEAR].desc = "btree.stat_clear";
	stats[WT_STAT_BTREE_STAT_PRINT].desc = "btree.stat_print";
	stats[WT_STAT_BTREE_SYNC].desc = "btree.sync";
	stats[WT_STAT_BTREE_VERIFY].desc = "btree.verify";
	stats[WT_STAT_CONNECTION_BTREE].desc = "connection.btree";
	stats[WT_STAT_CONNECTION_CACHE_SIZE_GET].desc =
	    "connection.cache_size_get";
	stats[WT_STAT_CONNECTION_CACHE_SIZE_SET].desc =
	    "connection.cache_size_set";
	stats[WT_STAT_CONNECTION_CLOSE].desc = "connection.close";
	stats[WT_STAT_CONNECTION_HAZARD_SIZE_GET].desc =
	    "connection.hazard_size_get";
	stats[WT_STAT_CONNECTION_HAZARD_SIZE_SET].desc =
	    "connection.hazard_size_set";
	stats[WT_STAT_CONNECTION_MSGCALL_GET].desc = "connection.msgcall_get";
	stats[WT_STAT_CONNECTION_MSGCALL_SET].desc = "connection.msgcall_set";
	stats[WT_STAT_CONNECTION_MSGFILE_GET].desc = "connection.msgfile_get";
	stats[WT_STAT_CONNECTION_MSGFILE_SET].desc = "connection.msgfile_set";
	stats[WT_STAT_CONNECTION_OPEN].desc = "connection.open";
	stats[WT_STAT_CONNECTION_SESSION].desc = "connection.session";
	stats[WT_STAT_CONNECTION_SESSION_SIZE_GET].desc =
	    "connection.session_size_get";
	stats[WT_STAT_CONNECTION_SESSION_SIZE_SET].desc =
	    "connection.session_size_set";
	stats[WT_STAT_CONNECTION_STAT_CLEAR].desc = "connection.stat_clear";
	stats[WT_STAT_CONNECTION_STAT_PRINT].desc = "connection.stat_print";
	stats[WT_STAT_CONNECTION_SYNC].desc = "connection.sync";
	stats[WT_STAT_CONNECTION_VERBOSE_GET].desc = "connection.verbose_get";
	stats[WT_STAT_CONNECTION_VERBOSE_SET].desc = "connection.verbose_set";
	stats[WT_STAT_SESSION_CLOSE].desc = "session.close";

	*statsp = stats;
	return (0);
}

void
__wt_stat_clear_method_stats(WT_STATS *stats)
{
	stats[WT_STAT_BTREE_BTREE_COMPARE_GET].v = 0;
	stats[WT_STAT_BTREE_BTREE_COMPARE_INT_GET].v = 0;
	stats[WT_STAT_BTREE_BTREE_COMPARE_INT_SET].v = 0;
	stats[WT_STAT_BTREE_BTREE_COMPARE_SET].v = 0;
	stats[WT_STAT_BTREE_BTREE_ITEMSIZE_GET].v = 0;
	stats[WT_STAT_BTREE_BTREE_ITEMSIZE_SET].v = 0;
	stats[WT_STAT_BTREE_BTREE_PAGESIZE_GET].v = 0;
	stats[WT_STAT_BTREE_BTREE_PAGESIZE_SET].v = 0;
	stats[WT_STAT_BTREE_BULK_LOAD].v = 0;
	stats[WT_STAT_BTREE_CLOSE].v = 0;
	stats[WT_STAT_BTREE_COLUMN_SET].v = 0;
	stats[WT_STAT_BTREE_COL_DEL].v = 0;
	stats[WT_STAT_BTREE_COL_DEL_RESTART].v = 0;
	stats[WT_STAT_BTREE_COL_GET].v = 0;
	stats[WT_STAT_BTREE_COL_PUT].v = 0;
	stats[WT_STAT_BTREE_COL_PUT_RESTART].v = 0;
	stats[WT_STAT_BTREE_DUMP].v = 0;
	stats[WT_STAT_BTREE_HUFFMAN_SET].v = 0;
	stats[WT_STAT_BTREE_OPEN].v = 0;
	stats[WT_STAT_BTREE_ROW_DEL].v = 0;
	stats[WT_STAT_BTREE_ROW_DEL_RESTART].v = 0;
	stats[WT_STAT_BTREE_ROW_GET].v = 0;
	stats[WT_STAT_BTREE_ROW_PUT].v = 0;
	stats[WT_STAT_BTREE_ROW_PUT_RESTART].v = 0;
	stats[WT_STAT_BTREE_STAT_CLEAR].v = 0;
	stats[WT_STAT_BTREE_STAT_PRINT].v = 0;
	stats[WT_STAT_BTREE_SYNC].v = 0;
	stats[WT_STAT_BTREE_VERIFY].v = 0;
	stats[WT_STAT_CONNECTION_BTREE].v = 0;
	stats[WT_STAT_CONNECTION_CACHE_SIZE_GET].v = 0;
	stats[WT_STAT_CONNECTION_CACHE_SIZE_SET].v = 0;
	stats[WT_STAT_CONNECTION_CLOSE].v = 0;
	stats[WT_STAT_CONNECTION_HAZARD_SIZE_GET].v = 0;
	stats[WT_STAT_CONNECTION_HAZARD_SIZE_SET].v = 0;
	stats[WT_STAT_CONNECTION_MSGCALL_GET].v = 0;
	stats[WT_STAT_CONNECTION_MSGCALL_SET].v = 0;
	stats[WT_STAT_CONNECTION_MSGFILE_GET].v = 0;
	stats[WT_STAT_CONNECTION_MSGFILE_SET].v = 0;
	stats[WT_STAT_CONNECTION_OPEN].v = 0;
	stats[WT_STAT_CONNECTION_SESSION].v = 0;
	stats[WT_STAT_CONNECTION_SESSION_SIZE_GET].v = 0;
	stats[WT_STAT_CONNECTION_SESSION_SIZE_SET].v = 0;
	stats[WT_STAT_CONNECTION_STAT_CLEAR].v = 0;
	stats[WT_STAT_CONNECTION_STAT_PRINT].v = 0;
	stats[WT_STAT_CONNECTION_SYNC].v = 0;
	stats[WT_STAT_CONNECTION_VERBOSE_GET].v = 0;
	stats[WT_STAT_CONNECTION_VERBOSE_SET].v = 0;
	stats[WT_STAT_SESSION_CLOSE].v = 0;
}
