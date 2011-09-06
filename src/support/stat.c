/* DO NOT EDIT: automatically built by dist/stat.py. */

#include "wt_internal.h"

int
__wt_stat_alloc_btree_stats(WT_SESSION_IMPL *session, WT_BTREE_STATS **statsp)
{
	WT_BTREE_STATS *stats;

	WT_RET(__wt_calloc_def(session, 1, &stats));

	stats->alloc.name = "alloc";
	stats->alloc.desc = "file: block allocations";
	stats->cursor_first.name = "cursor_first";
	stats->cursor_first.desc = "cursor-first";
	stats->cursor_inserts.name = "cursor_inserts";
	stats->cursor_inserts.desc = "cursor-inserts";
	stats->cursor_last.name = "cursor_last";
	stats->cursor_last.desc = "cursor-last";
	stats->cursor_read.name = "cursor_read";
	stats->cursor_read.desc = "cursor-read";
	stats->cursor_read_near.name = "cursor_read_near";
	stats->cursor_read_near.desc = "cursor-read-near";
	stats->cursor_read_next.name = "cursor_read_next";
	stats->cursor_read_next.desc = "cursor-read-next";
	stats->cursor_read_prev.name = "cursor_read_prev";
	stats->cursor_read_prev.desc = "cursor-read-prev";
	stats->cursor_removes.name = "cursor_removes";
	stats->cursor_removes.desc = "cursor-removes";
	stats->cursor_updates.name = "cursor_updates";
	stats->cursor_updates.desc = "cursor-updates";
	stats->extend.name = "extend";
	stats->extend.desc = "file: block allocations required file extension";
	stats->file_allocsize.name = "file_allocsize";
	stats->file_allocsize.desc = "page size allocation unit";
	stats->file_bulk_loaded.name = "file_bulk_loaded";
	stats->file_bulk_loaded.desc = "bulk-loaded entries";
	stats->file_col_deleted.name = "file_col_deleted";
	stats->file_col_deleted.desc = "column-store deleted values";
	stats->file_col_fix_pages.name = "file_col_fix_pages";
	stats->file_col_fix_pages.desc = "column-store fixed-size leaf pages";
	stats->file_col_int_pages.name = "file_col_int_pages";
	stats->file_col_int_pages.desc = "column-store internal pages";
	stats->file_col_var_pages.name = "file_col_var_pages";
	stats->file_col_var_pages.desc =
	    "column-store variable-size leaf pages";
	stats->file_entries.name = "file_entries";
	stats->file_entries.desc = "total entries";
	stats->file_fixed_len.name = "file_fixed_len";
	stats->file_fixed_len.desc = "fixed-record size";
	stats->file_freelist_bytes.name = "file_freelist_bytes";
	stats->file_freelist_bytes.desc = "number of bytes in the freelist";
	stats->file_freelist_entries.name = "file_freelist_entries";
	stats->file_freelist_entries.desc =
	    "number of entries in the freelist";
	stats->file_intlmax.name = "file_intlmax";
	stats->file_intlmax.desc = "maximum internal page size";
	stats->file_intlmin.name = "file_intlmin";
	stats->file_intlmin.desc = "minimum internal page size";
	stats->file_leafmax.name = "file_leafmax";
	stats->file_leafmax.desc = "maximum leaf page size";
	stats->file_leafmin.name = "file_leafmin";
	stats->file_leafmin.desc = "minimum leaf page size";
	stats->file_magic.name = "file_magic";
	stats->file_magic.desc = "magic number";
	stats->file_major.name = "file_major";
	stats->file_major.desc = "major version number";
	stats->file_minor.name = "file_minor";
	stats->file_minor.desc = "minor version number";
	stats->file_overflow.name = "file_overflow";
	stats->file_overflow.desc = "overflow pages";
	stats->file_row_int_pages.name = "file_row_int_pages";
	stats->file_row_int_pages.desc = "row-store internal pages";
	stats->file_row_leaf_pages.name = "file_row_leaf_pages";
	stats->file_row_leaf_pages.desc = "row-store leaf pages";
	stats->file_size.name = "file_size";
	stats->file_size.desc = "file: size";
	stats->free.name = "free";
	stats->free.desc = "file: block frees";
	stats->overflow_read.name = "overflow_read";
	stats->overflow_read.desc = "file: overflow pages read from the file";
	stats->page_read.name = "page_read";
	stats->page_read.desc = "file: pages read from the file";
	stats->page_write.name = "page_write";
	stats->page_write.desc = "file: pages written to the file";
	stats->rec_hazard.name = "rec_hazard";
	stats->rec_hazard.desc =
	    "reconcile: unable to acquire hazard reference";
	stats->rec_ovfl_key.name = "rec_ovfl_key";
	stats->rec_ovfl_key.desc = "reconcile: overflow key";
	stats->rec_ovfl_value.name = "rec_ovfl_value";
	stats->rec_ovfl_value.desc = "reconcile: overflow value";
	stats->rec_page_delete.name = "rec_page_delete";
	stats->rec_page_delete.desc = "reconcile: pages deleted";
	stats->rec_page_merge.name = "rec_page_merge";
	stats->rec_page_merge.desc =
	    "reconcile: deleted or temporary pages merged";
	stats->rec_split_intl.name = "rec_split_intl";
	stats->rec_split_intl.desc = "reconcile: internal pages split";
	stats->rec_split_leaf.name = "rec_split_leaf";
	stats->rec_split_leaf.desc = "reconcile: leaf pages split";

	*statsp = stats;
	return (0);
}

void
__wt_stat_clear_btree_stats(WT_STATS *stats_arg)
{
	WT_BTREE_STATS *stats;

	stats = (WT_BTREE_STATS *)stats_arg;
	stats->alloc.v = 0;
	stats->cursor_first.v = 0;
	stats->cursor_inserts.v = 0;
	stats->cursor_last.v = 0;
	stats->cursor_read.v = 0;
	stats->cursor_read_near.v = 0;
	stats->cursor_read_next.v = 0;
	stats->cursor_read_prev.v = 0;
	stats->cursor_removes.v = 0;
	stats->cursor_updates.v = 0;
	stats->extend.v = 0;
	stats->file_allocsize.v = 0;
	stats->file_bulk_loaded.v = 0;
	stats->file_col_deleted.v = 0;
	stats->file_col_fix_pages.v = 0;
	stats->file_col_int_pages.v = 0;
	stats->file_col_var_pages.v = 0;
	stats->file_entries.v = 0;
	stats->file_fixed_len.v = 0;
	stats->file_freelist_bytes.v = 0;
	stats->file_freelist_entries.v = 0;
	stats->file_intlmax.v = 0;
	stats->file_intlmin.v = 0;
	stats->file_leafmax.v = 0;
	stats->file_leafmin.v = 0;
	stats->file_magic.v = 0;
	stats->file_major.v = 0;
	stats->file_minor.v = 0;
	stats->file_overflow.v = 0;
	stats->file_row_int_pages.v = 0;
	stats->file_row_leaf_pages.v = 0;
	stats->file_size.v = 0;
	stats->free.v = 0;
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
__wt_stat_alloc_connection_stats(WT_SESSION_IMPL *session, WT_CONNECTION_STATS **statsp)
{
	WT_CONNECTION_STATS *stats;

	WT_RET(__wt_calloc_def(session, 1, &stats));

	stats->block_read.name = "block_read";
	stats->block_read.desc = "blocks read from a file";
	stats->block_write.name = "block_write";
	stats->block_write.desc = "blocks written to a file";
	stats->cache_bytes_inuse.name = "cache_bytes_inuse";
	stats->cache_bytes_inuse.desc =
	    "cache: bytes currently held in the cache";
	stats->cache_bytes_max.name = "cache_bytes_max";
	stats->cache_bytes_max.desc = "cache: maximum bytes configured";
	stats->cache_evict_hazard.name = "cache_evict_hazard";
	stats->cache_evict_hazard.desc =
	    "cache: pages selected for eviction not evicted because of a hazard reference";
	stats->cache_evict_modified.name = "cache_evict_modified";
	stats->cache_evict_modified.desc =
	    "cache: modified pages selected for eviction";
	stats->cache_evict_slow.name = "cache_evict_slow";
	stats->cache_evict_slow.desc =
	    "cache: eviction server unable to reach eviction goal";
	stats->cache_evict_unmodified.name = "cache_evict_unmodified";
	stats->cache_evict_unmodified.desc =
	    "cache: unmodified pages selected for eviction";
	stats->cache_pages_inuse.name = "cache_pages_inuse";
	stats->cache_pages_inuse.desc =
	    "cache: pages currently held in the cache";
	stats->file_open.name = "file_open";
	stats->file_open.desc = "files currently open";
	stats->memalloc.name = "memalloc";
	stats->memalloc.desc = "total memory allocations";
	stats->memfree.name = "memfree";
	stats->memfree.desc = "total memory frees";
	stats->mtx_lock.name = "mtx_lock";
	stats->mtx_lock.desc = "mutex lock calls";
	stats->total_read_io.name = "total_read_io";
	stats->total_read_io.desc = "total read I/Os";
	stats->total_write_io.name = "total_write_io";
	stats->total_write_io.desc = "total write I/Os";
	stats->workq_passes.name = "workq_passes";
	stats->workq_passes.desc = "workQ queue passes";
	stats->workq_yield.name = "workq_yield";
	stats->workq_yield.desc = "workQ yields";

	*statsp = stats;
	return (0);
}

void
__wt_stat_clear_connection_stats(WT_STATS *stats_arg)
{
	WT_CONNECTION_STATS *stats;

	stats = (WT_CONNECTION_STATS *)stats_arg;
	stats->block_read.v = 0;
	stats->block_write.v = 0;
	stats->cache_evict_hazard.v = 0;
	stats->cache_evict_modified.v = 0;
	stats->cache_evict_slow.v = 0;
	stats->cache_evict_unmodified.v = 0;
	stats->file_open.v = 0;
	stats->memalloc.v = 0;
	stats->memfree.v = 0;
	stats->mtx_lock.v = 0;
	stats->total_read_io.v = 0;
	stats->total_write_io.v = 0;
	stats->workq_passes.v = 0;
	stats->workq_yield.v = 0;
}
