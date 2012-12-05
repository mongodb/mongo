/* DO NOT EDIT: automatically built by dist/stat.py. */

#include "wt_internal.h"

int
__wt_stat_alloc_dsrc_stats(WT_SESSION_IMPL *session, WT_DSRC_STATS **statsp)
{
	WT_DSRC_STATS *stats;

	WT_RET(__wt_calloc_def(session, 1, &stats));

	stats->block_alloc.desc = "blocks allocated";
	stats->block_allocsize.desc =
	    "block manager file allocation unit size";
	stats->block_checkpoint_size.desc = "checkpoint size";
	stats->block_extension.desc =
	    "block allocations requiring file extension";
	stats->block_free.desc = "blocks freed";
	stats->block_magic.desc = "file magic number";
	stats->block_major.desc = "file major version number";
	stats->block_minor.desc = "minor version number";
	stats->block_size.desc = "block manager size";
	stats->bloom_count.desc = "bloom filters in the LSM tree";
	stats->bloom_false_positive.desc = "bloom filter false positives";
	stats->bloom_hit.desc = "bloom filter hits";
	stats->bloom_miss.desc = "bloom filter misses";
	stats->bloom_page_evict.desc = "bloom filter pages evicted from cache";
	stats->bloom_page_read.desc = "bloom filter pages read into cache";
	stats->bloom_size.desc = "total size of bloom filters";
	stats->btree_column_deleted.desc =
	    "column-store variable-size deleted values";
	stats->btree_column_fix.desc = "column-store fixed-size leaf pages";
	stats->btree_column_internal.desc = "column-store internal pages";
	stats->btree_column_variable.desc =
	    "column-store variable-size leaf pages";
	stats->btree_compact_rewrite.desc =
	    "tree pages rewritten by compaction";
	stats->btree_entries.desc = "total key/value pairs";
	stats->btree_entries_bulk_loaded.desc =
	    "total bulk-loaded key/value pairs";
	stats->btree_fixed_len.desc = "fixed-record size";
	stats->btree_maxintlitem.desc = "maximum internal page item size";
	stats->btree_maxintlpage.desc = "maximum internal page size";
	stats->btree_maxleafitem.desc = "maximum leaf page item size";
	stats->btree_maxleafpage.desc = "maximum leaf page size";
	stats->btree_overflow.desc = "overflow pages";
	stats->btree_row_internal.desc = "row-store internal pages";
	stats->btree_row_leaf.desc = "row-store leaf pages";
	stats->cache_bytes_changed.desc =
	    "approximate measure of bytes changed: counts key and value bytes inserted with cursor.insert, value bytes updated with cursor.update and key bytes removed using cursor.remove";
	stats->cache_bytes_read.desc = "bytes read into cache";
	stats->cache_bytes_write.desc = "bytes written from cache";
	stats->cache_eviction_clean.desc = "unmodified pages evicted";
	stats->cache_eviction_dirty.desc = "modified pages evicted";
	stats->cache_eviction_fail.desc =
	    "data source pages selected for eviction unable to be evicted";
	stats->cache_eviction_hazard.desc =
	    "eviction unable to acquire hazard reference";
	stats->cache_eviction_internal.desc = "internal pages evicted";
	stats->cache_overflow_value.desc = "overflow values cached in memory";
	stats->cache_read.desc = "pages read into cache";
	stats->cache_read_overflow.desc = "overflow pages read into cache";
	stats->cache_write.desc = "pages written from cache";
	stats->cursor_insert.desc = "cursor-inserts";
	stats->cursor_next.desc = "cursor next";
	stats->cursor_prev.desc = "cursor prev";
	stats->cursor_remove.desc = "cursor remove";
	stats->cursor_reset.desc = "cursor reset";
	stats->cursor_search.desc = "cursor search";
	stats->cursor_search_near.desc = "cursor search near";
	stats->cursor_update.desc = "cursor update";
	stats->lsm_chunk_count.desc = "chunks in the LSM tree";
	stats->lsm_generation_max.desc =
	    "highest merge generation in the LSM tree";
	stats->lsm_lookup_no_bloom.desc =
	    "queries that could have benefited from a Bloom filter that did not exist";
	stats->rec_dictionary.desc = "reconciliation dictionary matches";
	stats->rec_ovfl_key.desc = "reconciliation overflow keys written";
	stats->rec_ovfl_value.desc = "reconciliation overflow values written";
	stats->rec_page_delete.desc = "reconciliation pages deleted";
	stats->rec_page_merge.desc = "reconciliation pages merged";
	stats->rec_split_intl.desc = "reconciliation internal pages split";
	stats->rec_split_leaf.desc = "reconciliation leaf pages split";
	stats->rec_written.desc = "reconciliation pages written";
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
	stats->block_allocsize.v = 0;
	stats->block_checkpoint_size.v = 0;
	stats->block_extension.v = 0;
	stats->block_free.v = 0;
	stats->block_magic.v = 0;
	stats->block_major.v = 0;
	stats->block_minor.v = 0;
	stats->block_size.v = 0;
	stats->bloom_count.v = 0;
	stats->bloom_false_positive.v = 0;
	stats->bloom_hit.v = 0;
	stats->bloom_miss.v = 0;
	stats->bloom_page_evict.v = 0;
	stats->bloom_page_read.v = 0;
	stats->bloom_size.v = 0;
	stats->btree_column_deleted.v = 0;
	stats->btree_column_fix.v = 0;
	stats->btree_column_internal.v = 0;
	stats->btree_column_variable.v = 0;
	stats->btree_compact_rewrite.v = 0;
	stats->btree_entries.v = 0;
	stats->btree_entries_bulk_loaded.v = 0;
	stats->btree_fixed_len.v = 0;
	stats->btree_maxintlitem.v = 0;
	stats->btree_maxintlpage.v = 0;
	stats->btree_maxleafitem.v = 0;
	stats->btree_maxleafpage.v = 0;
	stats->btree_overflow.v = 0;
	stats->btree_row_internal.v = 0;
	stats->btree_row_leaf.v = 0;
	stats->cache_bytes_changed.v = 0;
	stats->cache_bytes_read.v = 0;
	stats->cache_bytes_write.v = 0;
	stats->cache_eviction_clean.v = 0;
	stats->cache_eviction_dirty.v = 0;
	stats->cache_eviction_fail.v = 0;
	stats->cache_eviction_hazard.v = 0;
	stats->cache_eviction_internal.v = 0;
	stats->cache_overflow_value.v = 0;
	stats->cache_read.v = 0;
	stats->cache_read_overflow.v = 0;
	stats->cache_write.v = 0;
	stats->cursor_insert.v = 0;
	stats->cursor_next.v = 0;
	stats->cursor_prev.v = 0;
	stats->cursor_remove.v = 0;
	stats->cursor_reset.v = 0;
	stats->cursor_search.v = 0;
	stats->cursor_search_near.v = 0;
	stats->cursor_update.v = 0;
	stats->lsm_chunk_count.v = 0;
	stats->lsm_generation_max.v = 0;
	stats->lsm_lookup_no_bloom.v = 0;
	stats->rec_dictionary.v = 0;
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

	stats->block_byte_read.desc = "bytes read by the block manager";
	stats->block_byte_write.desc = "bytes written by the block manager";
	stats->block_read.desc = "blocks read by the block manager";
	stats->block_write.desc = "blocks written by the block manager";
	stats->btree_file_open.desc =
	    "btree (including LSM) files currently open";
	stats->cache_bytes_dirty.desc =
	    "cache: tracked dirty bytes in the cache";
	stats->cache_bytes_inuse.desc = "cache: bytes currently in the cache";
	stats->cache_bytes_max.desc = "cache: maximum bytes configured";
	stats->cache_bytes_read.desc = "bytes read into cache";
	stats->cache_bytes_write.desc = "bytes written from cache";
	stats->cache_eviction_clean.desc = "cache: unmodified pages evicted";
	stats->cache_eviction_dirty.desc = "cache: modified pages evicted";
	stats->cache_eviction_fail.desc =
	    "cache: pages selected for eviction unable to be evicted";
	stats->cache_eviction_hazard.desc =
	    "cache: eviction unable to acquire hazard reference";
	stats->cache_eviction_internal.desc = "cache: internal pages evicted";
	stats->cache_eviction_slow.desc =
	    "cache: eviction server unable to reach eviction goal";
	stats->cache_pages_dirty.desc =
	    "cache: tracked dirty pages in the cache";
	stats->cache_pages_inuse.desc =
	    "cache: pages currently held in the cache";
	stats->cache_read.desc = "pages read into cache";
	stats->cache_write.desc = "pages written from cache";
	stats->cond_wait.desc = "pthread mutex condition wait calls";
	stats->memory_allocation.desc = "total heap memory allocations";
	stats->memory_free.desc = "total heap memory frees";
	stats->read_io.desc = "total read I/Os";
	stats->rwlock_read.desc = "pthread mutex shared lock read-lock calls";
	stats->rwlock_write.desc =
	    "pthread mutex shared lock write-lock calls";
	stats->txn_ancient.desc = "ancient transactions";
	stats->txn_begin.desc = "transactions";
	stats->txn_checkpoint.desc = "transaction checkpoints";
	stats->txn_commit.desc = "transactions committed";
	stats->txn_fail_cache.desc =
	    "transaction failures due to cache overflow";
	stats->txn_rollback.desc = "transactions rolled-back";
	stats->write_io.desc = "total write I/Os";

	*statsp = stats;
	return (0);
}

void
__wt_stat_clear_connection_stats(WT_STATS *stats_arg)
{
	WT_CONNECTION_STATS *stats;

	stats = (WT_CONNECTION_STATS *)stats_arg;
	stats->block_byte_read.v = 0;
	stats->block_byte_write.v = 0;
	stats->block_read.v = 0;
	stats->block_write.v = 0;
	stats->btree_file_open.v = 0;
	stats->cache_bytes_dirty.v = 0;
	stats->cache_bytes_read.v = 0;
	stats->cache_bytes_write.v = 0;
	stats->cache_eviction_clean.v = 0;
	stats->cache_eviction_dirty.v = 0;
	stats->cache_eviction_fail.v = 0;
	stats->cache_eviction_hazard.v = 0;
	stats->cache_eviction_internal.v = 0;
	stats->cache_eviction_slow.v = 0;
	stats->cache_pages_dirty.v = 0;
	stats->cache_read.v = 0;
	stats->cache_write.v = 0;
	stats->cond_wait.v = 0;
	stats->memory_allocation.v = 0;
	stats->memory_free.v = 0;
	stats->read_io.v = 0;
	stats->rwlock_read.v = 0;
	stats->rwlock_write.v = 0;
	stats->txn_ancient.v = 0;
	stats->txn_begin.v = 0;
	stats->txn_checkpoint.v = 0;
	stats->txn_commit.v = 0;
	stats->txn_fail_cache.v = 0;
	stats->txn_rollback.v = 0;
	stats->write_io.v = 0;
}
