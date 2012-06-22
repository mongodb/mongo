/* DO NOT EDIT: automatically built by dist/stat.py. */

#include "wt_internal.h"

int
__wt_stat_alloc_btree_stats(WT_SESSION_IMPL *session, WT_BTREE_STATS **statsp)
{
	WT_BTREE_STATS *stats;

	WT_RET(__wt_calloc_def(session, 1, &stats));

	stats->alloc.desc = "file: block allocations";
	stats->cursor_inserts.desc = "cursor-inserts";
	stats->cursor_read.desc = "cursor-read";
	stats->cursor_read_near.desc = "cursor-read-near";
	stats->cursor_read_next.desc = "cursor-read-next";
	stats->cursor_read_prev.desc = "cursor-read-prev";
	stats->cursor_removes.desc = "cursor-removes";
	stats->cursor_resets.desc = "cursor-resets";
	stats->cursor_updates.desc = "cursor-updates";
	stats->extend.desc = "file: block allocations required file extension";
	stats->file_allocsize.desc = "page size allocation unit";
	stats->file_bulk_loaded.desc = "bulk-loaded entries";
	stats->file_col_deleted.desc = "column-store deleted values";
	stats->file_col_fix_pages.desc = "column-store fixed-size leaf pages";
	stats->file_col_int_pages.desc = "column-store internal pages";
	stats->file_col_var_pages.desc =
	    "column-store variable-size leaf pages";
	stats->file_entries.desc = "total entries";
	stats->file_fixed_len.desc = "fixed-record size";
	stats->file_magic.desc = "magic number";
	stats->file_major.desc = "major version number";
	stats->file_maxintlitem.desc = "maximum internal page item size";
	stats->file_maxintlpage.desc = "maximum internal page size";
	stats->file_maxleafitem.desc = "maximum leaf page item size";
	stats->file_maxleafpage.desc = "maximum leaf page size";
	stats->file_minor.desc = "minor version number";
	stats->file_overflow.desc = "overflow pages";
	stats->file_row_int_pages.desc = "row-store internal pages";
	stats->file_row_leaf_pages.desc = "row-store leaf pages";
	stats->file_size.desc = "file: size";
	stats->file_write_conflicts.desc = "write generation conflicts";
	stats->free.desc = "file: block frees";
	stats->overflow_read.desc = "file: overflow pages read from the file";
	stats->page_read.desc = "file: pages read from the file";
	stats->page_write.desc = "file: pages written to the file";
	stats->rec_hazard.desc =
	    "reconcile: unable to acquire hazard reference";
	stats->rec_ovfl_key.desc = "reconcile: overflow key";
	stats->rec_ovfl_value.desc = "reconcile: overflow value";
	stats->rec_page_delete.desc = "reconcile: pages deleted";
	stats->rec_page_merge.desc =
	    "reconcile: deleted or temporary pages merged";
	stats->rec_split_intl.desc = "reconcile: internal pages split";
	stats->rec_split_leaf.desc = "reconcile: leaf pages split";
	stats->rec_written.desc = "reconcile: pages written";
	stats->update_conflict.desc = "update conflicts";

	*statsp = stats;
	return (0);
}

void
__wt_stat_clear_btree_stats(WT_STATS *stats_arg)
{
	WT_BTREE_STATS *stats;

	stats = (WT_BTREE_STATS *)stats_arg;
	stats->alloc.v = 0;
	stats->cursor_inserts.v = 0;
	stats->cursor_read.v = 0;
	stats->cursor_read_near.v = 0;
	stats->cursor_read_next.v = 0;
	stats->cursor_read_prev.v = 0;
	stats->cursor_removes.v = 0;
	stats->cursor_resets.v = 0;
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
	stats->file_magic.v = 0;
	stats->file_major.v = 0;
	stats->file_maxintlitem.v = 0;
	stats->file_maxintlpage.v = 0;
	stats->file_maxleafitem.v = 0;
	stats->file_maxleafpage.v = 0;
	stats->file_minor.v = 0;
	stats->file_overflow.v = 0;
	stats->file_row_int_pages.v = 0;
	stats->file_row_leaf_pages.v = 0;
	stats->file_size.v = 0;
	stats->file_write_conflicts.v = 0;
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
	stats->rec_written.v = 0;
	stats->update_conflict.v = 0;
}

int
__wt_stat_alloc_connection_stats(WT_SESSION_IMPL *session, WT_CONNECTION_STATS **statsp)
{
	WT_CONNECTION_STATS *stats;

	WT_RET(__wt_calloc_def(session, 1, &stats));

	stats->block_read.desc = "blocks read from a file";
	stats->block_write.desc = "blocks written to a file";
	stats->cache_bytes_inuse.desc =
	    "cache: bytes currently held in the cache";
	stats->cache_bytes_max.desc = "cache: maximum bytes configured";
	stats->cache_evict_hazard.desc =
	    "cache: pages selected for eviction not evicted because of a hazard reference";
	stats->cache_evict_internal.desc = "cache: internal pages evicted";
	stats->cache_evict_modified.desc = "cache: modified pages evicted";
	stats->cache_evict_slow.desc =
	    "cache: eviction server unable to reach eviction goal";
	stats->cache_evict_unmodified.desc = "cache: unmodified pages evicted";
	stats->cache_pages_inuse.desc =
	    "cache: pages currently held in the cache";
	stats->checkpoint.desc = "checkpoints";
	stats->cond_wait.desc = "condition wait calls";
	stats->file_open.desc = "files currently open";
	stats->memalloc.desc = "total memory allocations";
	stats->memfree.desc = "total memory frees";
	stats->rwlock_rdlock.desc = "rwlock readlock calls";
	stats->rwlock_wrlock.desc = "rwlock writelock calls";
	stats->total_read_io.desc = "total read I/Os";
	stats->total_write_io.desc = "total write I/Os";
	stats->txn_begin.desc = "transactions";
	stats->txn_commit.desc = "transactions committed";
	stats->txn_rollback.desc = "transactions rolled-back";

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
	stats->cache_evict_internal.v = 0;
	stats->cache_evict_modified.v = 0;
	stats->cache_evict_slow.v = 0;
	stats->cache_evict_unmodified.v = 0;
	stats->checkpoint.v = 0;
	stats->cond_wait.v = 0;
	stats->file_open.v = 0;
	stats->memalloc.v = 0;
	stats->memfree.v = 0;
	stats->rwlock_rdlock.v = 0;
	stats->rwlock_wrlock.v = 0;
	stats->total_read_io.v = 0;
	stats->total_write_io.v = 0;
	stats->txn_begin.v = 0;
	stats->txn_commit.v = 0;
	stats->txn_rollback.v = 0;
}
