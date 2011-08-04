/* DO NOT EDIT: automatically built by dist/stat.py. */

#include "wt_internal.h"

int
__wt_stat_alloc_btree_stats(WT_SESSION_IMPL *session, WT_BTREE_STATS **statsp)
{
	WT_BTREE_STATS *stats;

	WT_RET(__wt_calloc_def(session, 1, &stats));

	stats->alloc.desc = "file: block allocations";
	stats->extend.desc = "file: block allocations require file extension";
	stats->file_allocsize.desc = "page size allocation unit";
	stats->file_col_fix.desc = "column-store fixed-size leaf pages";
	stats->file_col_internal.desc = "column-store internal pages";
	stats->file_col_variable.desc =
	    "column-store variable-size leaf pages";
	stats->file_fixed_len.desc = "fixed-record size";
	stats->file_freelist_entries.desc =
	    "number of entries in the freelist";
	stats->file_intlmax.desc = "maximum internal page size";
	stats->file_intlmin.desc = "minimum internal page size";
	stats->file_item_col_deleted.desc = "column-store deleted value";
	stats->file_item_total_key.desc = "total keys";
	stats->file_item_total_value.desc = "total value items";
	stats->file_leafmax.desc = "maximum leaf page size";
	stats->file_leafmin.desc = "minimum leaf page size";
	stats->file_magic.desc = "magic number";
	stats->file_major.desc = "major version number";
	stats->file_minor.desc = "minor version number";
	stats->file_overflow.desc = "overflow pages";
	stats->file_row_internal.desc = "row-store internal pages";
	stats->file_row_leaf.desc = "row-store leaf pages";
	stats->free.desc = "file: block frees";
	stats->items_inserted.desc = "file: key/value pairs inserted";
	stats->overflow_read.desc = "file: overflow pages read from the file";
	stats->page_read.desc = "file: pages read from a file";
	stats->page_write.desc = "file: pages written to a file";
	stats->rec_hazard.desc =
	    "reconcile: unable to acquire hazard reference";
	stats->rec_ovfl_key.desc = "reconcile: overflow key";
	stats->rec_ovfl_value.desc = "reconcile: overflow value";
	stats->rec_page_delete.desc = "reconcile: pages deleted";
	stats->rec_page_merge.desc =
	    "reconcile: deleted or temporary pages merged";
	stats->rec_split_intl.desc = "reconcile: internal pages split";
	stats->rec_split_leaf.desc = "reconcile: leaf pages split";

	*statsp = stats;
	return (0);
}

void
__wt_stat_clear_btree_stats(WT_BTREE_STATS *stats)
{
	stats->alloc.v = 0;
	stats->extend.v = 0;
	stats->file_allocsize.v = 0;
	stats->file_col_fix.v = 0;
	stats->file_col_internal.v = 0;
	stats->file_col_variable.v = 0;
	stats->file_fixed_len.v = 0;
	stats->file_freelist_entries.v = 0;
	stats->file_intlmax.v = 0;
	stats->file_intlmin.v = 0;
	stats->file_item_col_deleted.v = 0;
	stats->file_item_total_key.v = 0;
	stats->file_item_total_value.v = 0;
	stats->file_leafmax.v = 0;
	stats->file_leafmin.v = 0;
	stats->file_magic.v = 0;
	stats->file_major.v = 0;
	stats->file_minor.v = 0;
	stats->file_overflow.v = 0;
	stats->file_row_internal.v = 0;
	stats->file_row_leaf.v = 0;
	stats->free.v = 0;
	stats->items_inserted.v = 0;
	stats->overflow_read.v = 0;
	stats->page_read.v = 0;
	stats->page_write.v = 0;
	stats->rec_hazard.v = 0;
	stats->rec_ovfl_key.v = 0;
	stats->rec_ovfl_value.v = 0;
	stats->rec_page_delete.v = 0;
	stats->rec_page_merge.v = 0;
	stats->rec_split_intl.v = 0;
	stats->rec_split_leaf.v = 0;
}

int
__wt_stat_alloc_conn_stats(WT_SESSION_IMPL *session, WT_CONN_STATS **statsp)
{
	WT_CONN_STATS *stats;

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
	stats->cache_evict_hazard.v = 0;
	stats->cache_evict_modified.v = 0;
	stats->cache_evict_unmodified.v = 0;
	stats->cache_overflow_read.v = 0;
	stats->cache_page_read.v = 0;
	stats->cache_page_write.v = 0;
	stats->file_open.v = 0;
	stats->memalloc.v = 0;
	stats->memfree.v = 0;
	stats->mtx_lock.v = 0;
	stats->total_read_io.v = 0;
	stats->total_write_io.v = 0;
	stats->workq_passes.v = 0;
	stats->workq_yield.v = 0;
}
