/* DO NOT EDIT: automatically built by dist/stat.py. */

#include "wt_internal.h"

int
__wt_stat_alloc_dsrc_stats(WT_SESSION_IMPL *session, WT_DSRC_STATS **statsp)
{
	WT_DSRC_STATS *stats;

	WT_RET(__wt_calloc_def(session, 1, &stats));

	stats->block_alloc.desc = "block allocations";
	stats->block_extend.desc = "block allocations required file extension";
	stats->block_free.desc = "block frees";
	stats->bloom_count.desc = "Number of Bloom filters in the LSM tree";
	stats->bloom_false_positive.desc =
	    "Number of Bloom filter false positives";
	stats->bloom_hit.desc = "Number of Bloom filter hits";
	stats->bloom_miss.desc = "Number of Bloom filter misses";
	stats->bloom_page_evict.desc =
	    "Number of Bloom pages evicted from cache";
	stats->bloom_page_read.desc = "Number of Bloom pages read into cache";
	stats->bloom_size.desc = "Total size of Bloom filters";
	stats->ckpt_size.desc = "checkpoint size";
	stats->cursor_insert.desc = "cursor-inserts";
	stats->cursor_read.desc = "cursor-read";
	stats->cursor_read_near.desc = "cursor-read-near";
	stats->cursor_read_next.desc = "cursor-read-next";
	stats->cursor_read_prev.desc = "cursor-read-prev";
	stats->cursor_remove.desc = "cursor-removes";
	stats->cursor_reset.desc = "cursor-resets";
	stats->cursor_update.desc = "cursor-updates";
	stats->entries.desc = "total entries";
	stats->file_allocsize.desc = "page size allocation unit";
	stats->file_bulk_loaded.desc = "bulk-loaded entries";
	stats->file_compact_rewrite.desc = "pages rewritten by compaction";
	stats->file_fixed_len.desc = "fixed-record size";
	stats->file_magic.desc = "magic number";
	stats->file_major.desc = "major version number";
	stats->file_maxintlitem.desc = "maximum internal page item size";
	stats->file_maxintlpage.desc = "maximum internal page size";
	stats->file_maxleafitem.desc = "maximum leaf page item size";
	stats->file_maxleafpage.desc = "maximum leaf page size";
	stats->file_minor.desc = "minor version number";
	stats->file_size.desc = "file size";
	stats->lsm_chunk_count.desc = "Number of chunks in the LSM tree";
	stats->lsm_generation_max.desc =
	    "Highest merge generation in the LSM tree";
	stats->lsm_lookup_no_bloom.desc =
	    "Number of queries that could have benefited from a Bloom filter that did not exist";
	stats->overflow_page.desc = "overflow pages";
	stats->overflow_read.desc = "overflow pages read into cache";
	stats->overflow_value_cache.desc = "overflow values cached in memory";
	stats->page_col_deleted.desc = "column-store deleted values";
	stats->page_col_fix.desc = "column-store fixed-size leaf pages";
	stats->page_col_int.desc = "column-store internal pages";
	stats->page_col_var.desc = "column-store variable-size leaf pages";
	stats->page_evict.desc = "pages evicted from the data source";
	stats->page_evict_fail.desc =
	    "pages that were selected for eviction that could not be evicted";
	stats->page_read.desc = "pages read into cache";
	stats->page_row_int.desc = "row-store internal pages";
	stats->page_row_leaf.desc = "row-store leaf pages";
	stats->page_write.desc = "pages written from cache";
	stats->rec_dictionary.desc = "reconcile: dictionary match";
	stats->rec_hazard.desc =
	    "reconciliation unable to acquire hazard reference";
	stats->rec_ovfl_key.desc = "reconciliation overflow key";
	stats->rec_ovfl_value.desc = "reconciliation overflow value";
	stats->rec_page_delete.desc = "pages deleted";
	stats->rec_page_merge.desc = "deleted or temporary pages merged";
	stats->rec_split_intl.desc = "internal pages split";
	stats->rec_split_leaf.desc = "leaf pages split";
	stats->rec_written.desc = "pages written";
	stats->txn_update_conflict.desc = "update conflicts";
	stats->txn_write_conflict.desc = "write generation conflicts";

	*statsp = stats;
	return (0);
}

void
__wt_stat_clear_dsrc_stats(WT_STATS *stats_arg)
{
	WT_DSRC_STATS *stats;

	stats = (WT_DSRC_STATS *)stats_arg;
	stats->block_alloc.v = 0;
	stats->block_extend.v = 0;
	stats->block_free.v = 0;
	stats->bloom_count.v = 0;
	stats->bloom_false_positive.v = 0;
	stats->bloom_hit.v = 0;
	stats->bloom_miss.v = 0;
	stats->bloom_page_evict.v = 0;
	stats->bloom_page_read.v = 0;
	stats->bloom_size.v = 0;
	stats->ckpt_size.v = 0;
	stats->cursor_insert.v = 0;
	stats->cursor_read.v = 0;
	stats->cursor_read_near.v = 0;
	stats->cursor_read_next.v = 0;
	stats->cursor_read_prev.v = 0;
	stats->cursor_remove.v = 0;
	stats->cursor_reset.v = 0;
	stats->cursor_update.v = 0;
	stats->entries.v = 0;
	stats->file_allocsize.v = 0;
	stats->file_bulk_loaded.v = 0;
	stats->file_compact_rewrite.v = 0;
	stats->file_fixed_len.v = 0;
	stats->file_magic.v = 0;
	stats->file_major.v = 0;
	stats->file_maxintlitem.v = 0;
	stats->file_maxintlpage.v = 0;
	stats->file_maxleafitem.v = 0;
	stats->file_maxleafpage.v = 0;
	stats->file_minor.v = 0;
	stats->file_size.v = 0;
	stats->lsm_chunk_count.v = 0;
	stats->lsm_generation_max.v = 0;
	stats->lsm_lookup_no_bloom.v = 0;
	stats->overflow_page.v = 0;
	stats->overflow_read.v = 0;
	stats->overflow_value_cache.v = 0;
	stats->page_col_deleted.v = 0;
	stats->page_col_fix.v = 0;
	stats->page_col_int.v = 0;
	stats->page_col_var.v = 0;
	stats->page_evict.v = 0;
	stats->page_evict_fail.v = 0;
	stats->page_read.v = 0;
	stats->page_row_int.v = 0;
	stats->page_row_leaf.v = 0;
	stats->page_write.v = 0;
	stats->rec_dictionary.v = 0;
	stats->rec_hazard.v = 0;
	stats->rec_ovfl_key.v = 0;
	stats->rec_ovfl_value.v = 0;
	stats->rec_page_delete.v = 0;
	stats->rec_page_merge.v = 0;
	stats->rec_split_intl.v = 0;
	stats->rec_split_leaf.v = 0;
	stats->rec_written.v = 0;
	stats->txn_update_conflict.v = 0;
	stats->txn_write_conflict.v = 0;
}

int
__wt_stat_alloc_connection_stats(WT_SESSION_IMPL *session, WT_CONNECTION_STATS **statsp)
{
	WT_CONNECTION_STATS *stats;

	WT_RET(__wt_calloc_def(session, 1, &stats));

	stats->block_read.desc = "blocks read from a file";
	stats->block_write.desc = "blocks written to a file";
	stats->cache_bytes_dirty.desc =
	    "cache: tracked dirty bytes in the cache";
	stats->cache_bytes_dirty_calc.desc =
	    "cache: counted dirty bytes in the cache";
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
	stats->cache_pages_dirty.desc =
	    "cache: tracked dirty pages in the cache";
	stats->cache_pages_dirty_calc.desc =
	    "cache: counted dirty pages in the cache";
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
	stats->txn_ancient.desc = "ancient transactions";
	stats->txn_begin.desc = "transactions";
	stats->txn_commit.desc = "transactions committed";
	stats->txn_fail_cache.desc =
	    "transaction failures due to cache overflow";
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
	stats->cache_bytes_dirty.v = 0;
	stats->cache_bytes_dirty_calc.v = 0;
	stats->cache_evict_hazard.v = 0;
	stats->cache_evict_internal.v = 0;
	stats->cache_evict_modified.v = 0;
	stats->cache_evict_slow.v = 0;
	stats->cache_evict_unmodified.v = 0;
	stats->cache_pages_dirty.v = 0;
	stats->cache_pages_dirty_calc.v = 0;
	stats->checkpoint.v = 0;
	stats->cond_wait.v = 0;
	stats->file_open.v = 0;
	stats->memalloc.v = 0;
	stats->memfree.v = 0;
	stats->rwlock_rdlock.v = 0;
	stats->rwlock_wrlock.v = 0;
	stats->total_read_io.v = 0;
	stats->total_write_io.v = 0;
	stats->txn_ancient.v = 0;
	stats->txn_begin.v = 0;
	stats->txn_commit.v = 0;
	stats->txn_fail_cache.v = 0;
	stats->txn_rollback.v = 0;
}
