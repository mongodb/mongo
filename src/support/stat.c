/* DO NOT EDIT: automatically built by dist/stat.py. */

#include "wt_internal.h"

int
__wt_stat_alloc_btree_stats(SESSION *session, WT_BTREE_STATS **statsp)
{
	WT_BTREE_STATS *stats;

	WT_RET(__wt_calloc_def(session, 1, &stats));

	stats->alloc.desc = "file: block allocations";
	stats->extend.desc = "file: block allocations require file extension";
	stats->free.desc = "file: block frees";
	stats->huffman_key.desc = "file: huffman key compression in bytes";
	stats->huffman_value.desc = "file: huffman value compression in bytes";
	stats->items_inserted.desc = "file: key/value pairs inserted";
	stats->overflow_data.desc = "file: overflow values inserted";
	stats->overflow_key.desc = "file: overflow key items inserted";
	stats->overflow_read.desc = "file: overflow pages read from the file";
	stats->page_read.desc = "file: pages read from a file";
	stats->page_write.desc = "file: pages written to a file";
	stats->split_intl.desc = "split internal pages";
	stats->split_leaf.desc = "split leaf pages";

	*statsp = stats;
	return (0);
}

void
__wt_stat_clear_btree_stats(WT_BTREE_STATS *stats)
{
	stats->alloc.v = 0;
	stats->extend.v = 0;
	stats->free.v = 0;
	stats->huffman_key.v = 0;
	stats->huffman_value.v = 0;
	stats->items_inserted.v = 0;
	stats->overflow_data.v = 0;
	stats->overflow_key.v = 0;
	stats->overflow_read.v = 0;
	stats->page_read.v = 0;
	stats->page_write.v = 0;
	stats->split_intl.v = 0;
	stats->split_leaf.v = 0;
}

void
__wt_stat_print_btree_stats(WT_BTREE_STATS *stats, FILE *stream)
{
	__wt_stat_print(&stats->alloc, stream);
	__wt_stat_print(&stats->extend, stream);
	__wt_stat_print(&stats->free, stream);
	__wt_stat_print(&stats->huffman_key, stream);
	__wt_stat_print(&stats->huffman_value, stream);
	__wt_stat_print(&stats->items_inserted, stream);
	__wt_stat_print(&stats->overflow_data, stream);
	__wt_stat_print(&stats->overflow_key, stream);
	__wt_stat_print(&stats->overflow_read, stream);
	__wt_stat_print(&stats->page_read, stream);
	__wt_stat_print(&stats->page_write, stream);
	__wt_stat_print(&stats->split_intl, stream);
	__wt_stat_print(&stats->split_leaf, stream);
}

int
__wt_stat_alloc_btree_file_stats(SESSION *session, WT_BTREE_FILE_STATS **statsp)
{
	WT_BTREE_FILE_STATS *stats;

	WT_RET(__wt_calloc_def(session, 1, &stats));

	stats->file_allocsize.desc = "page size allocation unit";
	stats->file_base_recno.desc = "base record number";
	stats->file_col_fix.desc = "column-store fixed-size leaf pages";
	stats->file_col_internal.desc = "column-store internal pages";
	stats->file_col_rle.desc =
	    "column-store repeat-count compressed fixed-size leaf pages";
	stats->file_col_variable.desc =
	    "column-store variable-size leaf pages";
	stats->file_fixed_len.desc = "fixed-record size";
	stats->file_freelist_entries.desc =
	    "number of entries in the freelist";
	stats->file_intlmax.desc = "maximum internal page size";
	stats->file_intlmin.desc = "minimum internal page size";
	stats->file_item_col_deleted.desc = "column-store deleted data items";
	stats->file_item_total_data.desc = "total data items";
	stats->file_item_total_key.desc = "total keys";
	stats->file_leafmax.desc = "maximum leaf page size";
	stats->file_leafmin.desc = "minimum leaf page size";
	stats->file_magic.desc = "magic number";
	stats->file_major.desc = "major version number";
	stats->file_minor.desc = "minor version number";
	stats->file_overflow.desc = "overflow pages";
	stats->file_row_internal.desc = "row-store internal pages";
	stats->file_row_leaf.desc = "row-store leaf pages";

	*statsp = stats;
	return (0);
}

void
__wt_stat_clear_btree_file_stats(WT_BTREE_FILE_STATS *stats)
{
	stats->file_allocsize.v = 0;
	stats->file_base_recno.v = 0;
	stats->file_col_fix.v = 0;
	stats->file_col_internal.v = 0;
	stats->file_col_rle.v = 0;
	stats->file_col_variable.v = 0;
	stats->file_fixed_len.v = 0;
	stats->file_freelist_entries.v = 0;
	stats->file_intlmax.v = 0;
	stats->file_intlmin.v = 0;
	stats->file_item_col_deleted.v = 0;
	stats->file_item_total_data.v = 0;
	stats->file_item_total_key.v = 0;
	stats->file_leafmax.v = 0;
	stats->file_leafmin.v = 0;
	stats->file_magic.v = 0;
	stats->file_major.v = 0;
	stats->file_minor.v = 0;
	stats->file_overflow.v = 0;
	stats->file_row_internal.v = 0;
	stats->file_row_leaf.v = 0;
}

void
__wt_stat_print_btree_file_stats(WT_BTREE_FILE_STATS *stats, FILE *stream)
{
	__wt_stat_print(&stats->file_allocsize, stream);
	__wt_stat_print(&stats->file_base_recno, stream);
	__wt_stat_print(&stats->file_col_fix, stream);
	__wt_stat_print(&stats->file_col_internal, stream);
	__wt_stat_print(&stats->file_col_rle, stream);
	__wt_stat_print(&stats->file_col_variable, stream);
	__wt_stat_print(&stats->file_fixed_len, stream);
	__wt_stat_print(&stats->file_freelist_entries, stream);
	__wt_stat_print(&stats->file_intlmax, stream);
	__wt_stat_print(&stats->file_intlmin, stream);
	__wt_stat_print(&stats->file_item_col_deleted, stream);
	__wt_stat_print(&stats->file_item_total_data, stream);
	__wt_stat_print(&stats->file_item_total_key, stream);
	__wt_stat_print(&stats->file_leafmax, stream);
	__wt_stat_print(&stats->file_leafmin, stream);
	__wt_stat_print(&stats->file_magic, stream);
	__wt_stat_print(&stats->file_major, stream);
	__wt_stat_print(&stats->file_minor, stream);
	__wt_stat_print(&stats->file_overflow, stream);
	__wt_stat_print(&stats->file_row_internal, stream);
	__wt_stat_print(&stats->file_row_leaf, stream);
}

int
__wt_stat_alloc_cache_stats(SESSION *session, WT_CACHE_STATS **statsp)
{
	WT_CACHE_STATS *stats;

	WT_RET(__wt_calloc_def(session, 1, &stats));

	stats->cache_bytes_inuse.desc =
	    "cache: bytes currently held in the cache";
	stats->cache_bytes_max.desc = "cache: maximum bytes configured";
	stats->cache_evict_hazard.desc =
	    "cache: pages selected for eviction not evicted because of a hazard reference";
	stats->cache_evict_modified.desc =
	    "cache: modified pages selected for eviction";
	stats->cache_evict_unmodified.desc =
	    "cache: unmodified pages selected for eviction";
	stats->cache_overflow_read.desc =
	    "cache: overflow pages read from the file";
	stats->cache_page_read.desc = "cache: pages read from a file";
	stats->cache_page_write.desc = "cache: pages written to a file";
	stats->cache_pages_inuse.desc =
	    "cache: pages currently held in the cache";

	*statsp = stats;
	return (0);
}

void
__wt_stat_clear_cache_stats(WT_CACHE_STATS *stats)
{
	stats->cache_evict_hazard.v = 0;
	stats->cache_evict_modified.v = 0;
	stats->cache_evict_unmodified.v = 0;
	stats->cache_overflow_read.v = 0;
	stats->cache_page_read.v = 0;
	stats->cache_page_write.v = 0;
}

void
__wt_stat_print_cache_stats(WT_CACHE_STATS *stats, FILE *stream)
{
	__wt_stat_print(&stats->cache_bytes_inuse, stream);
	__wt_stat_print(&stats->cache_bytes_max, stream);
	__wt_stat_print(&stats->cache_evict_hazard, stream);
	__wt_stat_print(&stats->cache_evict_modified, stream);
	__wt_stat_print(&stats->cache_evict_unmodified, stream);
	__wt_stat_print(&stats->cache_overflow_read, stream);
	__wt_stat_print(&stats->cache_page_read, stream);
	__wt_stat_print(&stats->cache_page_write, stream);
	__wt_stat_print(&stats->cache_pages_inuse, stream);
}

int
__wt_stat_alloc_conn_stats(SESSION *session, WT_CONN_STATS **statsp)
{
	WT_CONN_STATS *stats;

	WT_RET(__wt_calloc_def(session, 1, &stats));

	stats->file_open.desc = "file open";
	stats->memalloc.desc = "memory allocations";
	stats->memfree.desc = "memory frees";
	stats->mtx_lock.desc = "mutex lock calls";
	stats->total_read_io.desc = "total read I/Os";
	stats->total_write_io.desc = "total write I/Os";
	stats->workq_passes.desc = "workQ queue passes";
	stats->workq_yield.desc = "workQ yields";

	*statsp = stats;
	return (0);
}

void
__wt_stat_clear_conn_stats(WT_CONN_STATS *stats)
{
	stats->file_open.v = 0;
	stats->memalloc.v = 0;
	stats->memfree.v = 0;
	stats->mtx_lock.v = 0;
	stats->total_read_io.v = 0;
	stats->total_write_io.v = 0;
	stats->workq_passes.v = 0;
	stats->workq_yield.v = 0;
}

void
__wt_stat_print_conn_stats(WT_CONN_STATS *stats, FILE *stream)
{
	__wt_stat_print(&stats->file_open, stream);
	__wt_stat_print(&stats->memalloc, stream);
	__wt_stat_print(&stats->memfree, stream);
	__wt_stat_print(&stats->mtx_lock, stream);
	__wt_stat_print(&stats->total_read_io, stream);
	__wt_stat_print(&stats->total_write_io, stream);
	__wt_stat_print(&stats->workq_passes, stream);
	__wt_stat_print(&stats->workq_yield, stream);
}

int
__wt_stat_alloc_file_stats(SESSION *session, WT_FILE_STATS **statsp)
{
	WT_FILE_STATS *stats;

	WT_RET(__wt_calloc_def(session, 1, &stats));

	stats->fsync.desc = "fsyncs";
	stats->read_io.desc = "read I/Os";
	stats->write_io.desc = "write I/Os";

	*statsp = stats;
	return (0);
}

void
__wt_stat_clear_file_stats(WT_FILE_STATS *stats)
{
	stats->fsync.v = 0;
	stats->read_io.v = 0;
	stats->write_io.v = 0;
}

void
__wt_stat_print_file_stats(WT_FILE_STATS *stats, FILE *stream)
{
	__wt_stat_print(&stats->fsync, stream);
	__wt_stat_print(&stats->read_io, stream);
	__wt_stat_print(&stats->write_io, stream);
}
