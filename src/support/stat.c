/* DO NOT EDIT: automatically built by dist/stat.py. */

#include "wt_internal.h"

void
__wt_stat_init_dsrc_stats(WT_DSRC_STATS *stats)
{
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
	stats->block_size.desc = "block manager file size in bytes";
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
	stats->btree_compact_rewrite.desc = "pages rewritten by compaction";
	stats->btree_entries.desc =
	    "total LSM, table or file object key/value pairs";
	stats->btree_fixed_len.desc = "fixed-record size";
	stats->btree_maximum_depth.desc = "maximum tree depth";
	stats->btree_maxintlitem.desc = "maximum internal page item size";
	stats->btree_maxintlpage.desc = "maximum internal page size";
	stats->btree_maxleafitem.desc = "maximum leaf page item size";
	stats->btree_maxleafpage.desc = "maximum leaf page size";
	stats->btree_overflow.desc = "overflow pages";
	stats->btree_row_internal.desc = "row-store internal pages";
	stats->btree_row_leaf.desc = "row-store leaf pages";
	stats->cache_bytes_read.desc = "bytes read into cache";
	stats->cache_bytes_write.desc = "bytes written from cache";
	stats->cache_eviction_checkpoint.desc =
	    "cache: checkpoint blocked page eviction";
	stats->cache_eviction_clean.desc = "unmodified pages evicted";
	stats->cache_eviction_dirty.desc = "modified pages evicted";
	stats->cache_eviction_fail.desc =
	    "data source pages selected for eviction unable to be evicted";
	stats->cache_eviction_hazard.desc =
	    "cache: hazard pointer blocked page eviction";
	stats->cache_eviction_internal.desc = "internal pages evicted";
	stats->cache_eviction_merge.desc =
	    "cache: internal page merge operations completed";
	stats->cache_eviction_merge_fail.desc =
	    "cache: internal page merge attempts that could not complete";
	stats->cache_eviction_merge_levels.desc =
	    "cache: internal levels merged";
	stats->cache_overflow_value.desc = "overflow values cached in memory";
	stats->cache_read.desc = "pages read into cache";
	stats->cache_read_overflow.desc = "overflow pages read into cache";
	stats->cache_write.desc = "pages written from cache";
	stats->compress_raw_fail.desc =
	    "raw compression call failed, no additional data available";
	stats->compress_raw_fail_temporary.desc =
	    "raw compression call failed, additional data available";
	stats->compress_raw_ok.desc = "raw compression call succeeded";
	stats->compress_read.desc = "compressed pages read";
	stats->compress_write.desc = "compressed pages written";
	stats->compress_write_fail.desc = "page written failed to compress";
	stats->compress_write_too_small.desc =
	    "page written was too small to compress";
	stats->cursor_create.desc = "cursor creation";
	stats->cursor_insert.desc = "cursor insert calls";
	stats->cursor_insert_bulk.desc = "bulk-loaded cursor-insert calls";
	stats->cursor_insert_bytes.desc =
	    "cursor-insert key and value bytes inserted";
	stats->cursor_next.desc = "cursor next calls";
	stats->cursor_prev.desc = "cursor prev calls";
	stats->cursor_remove.desc = "cursor remove calls";
	stats->cursor_remove_bytes.desc = "cursor-remove key bytes removed";
	stats->cursor_reset.desc = "cursor reset calls";
	stats->cursor_search.desc = "cursor search calls";
	stats->cursor_search_near.desc = "cursor search near calls";
	stats->cursor_update.desc = "cursor update calls";
	stats->cursor_update_bytes.desc = "cursor-update value bytes updated";
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
	stats->rec_pages.desc = "page reconciliation calls";
	stats->rec_pages_eviction.desc =
	    "page reconciliation calls for eviction";
	stats->rec_skipped_update.desc =
	    "reconciliation failed because an update could not be included";
	stats->rec_split_intl.desc = "reconciliation internal pages split";
	stats->rec_split_leaf.desc = "reconciliation leaf pages split";
	stats->rec_split_max.desc =
	    "reconciliation maximum number of splits created for a page";
	stats->session_compact.desc = "object compaction";
	stats->session_cursor_open.desc = "open cursor count";
	stats->txn_update_conflict.desc = "update conflicts";
	stats->txn_write_conflict.desc = "write generation conflicts";
}

void
__wt_stat_clear_dsrc_stats(void *stats_arg)
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
	stats->btree_fixed_len.v = 0;
	stats->btree_maximum_depth.v = 0;
	stats->btree_maxintlitem.v = 0;
	stats->btree_maxintlpage.v = 0;
	stats->btree_maxleafitem.v = 0;
	stats->btree_maxleafpage.v = 0;
	stats->btree_overflow.v = 0;
	stats->btree_row_internal.v = 0;
	stats->btree_row_leaf.v = 0;
	stats->cache_bytes_read.v = 0;
	stats->cache_bytes_write.v = 0;
	stats->cache_eviction_checkpoint.v = 0;
	stats->cache_eviction_clean.v = 0;
	stats->cache_eviction_dirty.v = 0;
	stats->cache_eviction_fail.v = 0;
	stats->cache_eviction_hazard.v = 0;
	stats->cache_eviction_internal.v = 0;
	stats->cache_eviction_merge.v = 0;
	stats->cache_eviction_merge_fail.v = 0;
	stats->cache_eviction_merge_levels.v = 0;
	stats->cache_overflow_value.v = 0;
	stats->cache_read.v = 0;
	stats->cache_read_overflow.v = 0;
	stats->cache_write.v = 0;
	stats->compress_raw_fail.v = 0;
	stats->compress_raw_fail_temporary.v = 0;
	stats->compress_raw_ok.v = 0;
	stats->compress_read.v = 0;
	stats->compress_write.v = 0;
	stats->compress_write_fail.v = 0;
	stats->compress_write_too_small.v = 0;
	stats->cursor_create.v = 0;
	stats->cursor_insert.v = 0;
	stats->cursor_insert_bulk.v = 0;
	stats->cursor_insert_bytes.v = 0;
	stats->cursor_next.v = 0;
	stats->cursor_prev.v = 0;
	stats->cursor_remove.v = 0;
	stats->cursor_remove_bytes.v = 0;
	stats->cursor_reset.v = 0;
	stats->cursor_search.v = 0;
	stats->cursor_search_near.v = 0;
	stats->cursor_update.v = 0;
	stats->cursor_update_bytes.v = 0;
	stats->lsm_chunk_count.v = 0;
	stats->lsm_generation_max.v = 0;
	stats->lsm_lookup_no_bloom.v = 0;
	stats->rec_dictionary.v = 0;
	stats->rec_ovfl_key.v = 0;
	stats->rec_ovfl_value.v = 0;
	stats->rec_page_delete.v = 0;
	stats->rec_page_merge.v = 0;
	stats->rec_pages.v = 0;
	stats->rec_pages_eviction.v = 0;
	stats->rec_skipped_update.v = 0;
	stats->rec_split_intl.v = 0;
	stats->rec_split_leaf.v = 0;
	stats->rec_split_max.v = 0;
	stats->session_compact.v = 0;
	stats->txn_update_conflict.v = 0;
	stats->txn_write_conflict.v = 0;
}

void
__wt_stat_aggregate_dsrc_stats(void *child, void *parent)
{
	WT_DSRC_STATS *c, *p;

	c = (WT_DSRC_STATS *)child;
	p = (WT_DSRC_STATS *)parent;
	p->block_alloc.v += c->block_alloc.v;
	p->block_checkpoint_size.v += c->block_checkpoint_size.v;
	p->block_extension.v += c->block_extension.v;
	p->block_free.v += c->block_free.v;
	p->block_size.v += c->block_size.v;
	p->bloom_count.v += c->bloom_count.v;
	p->bloom_false_positive.v += c->bloom_false_positive.v;
	p->bloom_hit.v += c->bloom_hit.v;
	p->bloom_miss.v += c->bloom_miss.v;
	p->bloom_page_evict.v += c->bloom_page_evict.v;
	p->bloom_page_read.v += c->bloom_page_read.v;
	p->bloom_size.v += c->bloom_size.v;
	p->btree_column_deleted.v += c->btree_column_deleted.v;
	p->btree_column_fix.v += c->btree_column_fix.v;
	p->btree_column_internal.v += c->btree_column_internal.v;
	p->btree_column_variable.v += c->btree_column_variable.v;
	p->btree_compact_rewrite.v += c->btree_compact_rewrite.v;
	p->btree_entries.v += c->btree_entries.v;
	if (c->btree_maximum_depth.v > p->btree_maximum_depth.v)
	    p->btree_maximum_depth.v = c->btree_maximum_depth.v;
	p->btree_overflow.v += c->btree_overflow.v;
	p->btree_row_internal.v += c->btree_row_internal.v;
	p->btree_row_leaf.v += c->btree_row_leaf.v;
	p->cache_bytes_read.v += c->cache_bytes_read.v;
	p->cache_bytes_write.v += c->cache_bytes_write.v;
	p->cache_eviction_checkpoint.v += c->cache_eviction_checkpoint.v;
	p->cache_eviction_clean.v += c->cache_eviction_clean.v;
	p->cache_eviction_dirty.v += c->cache_eviction_dirty.v;
	p->cache_eviction_fail.v += c->cache_eviction_fail.v;
	p->cache_eviction_hazard.v += c->cache_eviction_hazard.v;
	p->cache_eviction_internal.v += c->cache_eviction_internal.v;
	p->cache_eviction_merge.v += c->cache_eviction_merge.v;
	p->cache_eviction_merge_fail.v += c->cache_eviction_merge_fail.v;
	p->cache_eviction_merge_levels.v += c->cache_eviction_merge_levels.v;
	p->cache_overflow_value.v += c->cache_overflow_value.v;
	p->cache_read.v += c->cache_read.v;
	p->cache_read_overflow.v += c->cache_read_overflow.v;
	p->cache_write.v += c->cache_write.v;
	p->compress_raw_fail.v += c->compress_raw_fail.v;
	p->compress_raw_fail_temporary.v += c->compress_raw_fail_temporary.v;
	p->compress_raw_ok.v += c->compress_raw_ok.v;
	p->compress_read.v += c->compress_read.v;
	p->compress_write.v += c->compress_write.v;
	p->compress_write_fail.v += c->compress_write_fail.v;
	p->compress_write_too_small.v += c->compress_write_too_small.v;
	p->cursor_create.v += c->cursor_create.v;
	p->cursor_insert.v += c->cursor_insert.v;
	p->cursor_insert_bulk.v += c->cursor_insert_bulk.v;
	p->cursor_insert_bytes.v += c->cursor_insert_bytes.v;
	p->cursor_next.v += c->cursor_next.v;
	p->cursor_prev.v += c->cursor_prev.v;
	p->cursor_remove.v += c->cursor_remove.v;
	p->cursor_remove_bytes.v += c->cursor_remove_bytes.v;
	p->cursor_reset.v += c->cursor_reset.v;
	p->cursor_search.v += c->cursor_search.v;
	p->cursor_search_near.v += c->cursor_search_near.v;
	p->cursor_update.v += c->cursor_update.v;
	p->cursor_update_bytes.v += c->cursor_update_bytes.v;
	if (c->lsm_generation_max.v > p->lsm_generation_max.v)
	    p->lsm_generation_max.v = c->lsm_generation_max.v;
	p->lsm_lookup_no_bloom.v += c->lsm_lookup_no_bloom.v;
	p->rec_dictionary.v += c->rec_dictionary.v;
	p->rec_ovfl_key.v += c->rec_ovfl_key.v;
	p->rec_ovfl_value.v += c->rec_ovfl_value.v;
	p->rec_page_delete.v += c->rec_page_delete.v;
	p->rec_page_merge.v += c->rec_page_merge.v;
	p->rec_pages.v += c->rec_pages.v;
	p->rec_pages_eviction.v += c->rec_pages_eviction.v;
	p->rec_skipped_update.v += c->rec_skipped_update.v;
	p->rec_split_intl.v += c->rec_split_intl.v;
	p->rec_split_leaf.v += c->rec_split_leaf.v;
	if (c->rec_split_max.v > p->rec_split_max.v)
	    p->rec_split_max.v = c->rec_split_max.v;
	p->session_compact.v += c->session_compact.v;
	p->session_cursor_open.v += c->session_cursor_open.v;
	p->txn_update_conflict.v += c->txn_update_conflict.v;
	p->txn_write_conflict.v += c->txn_write_conflict.v;
}

void
__wt_stat_init_connection_stats(WT_CONNECTION_STATS *stats)
{
	stats->block_byte_map_read.desc =
	    "mapped bytes read by the block manager";
	stats->block_byte_read.desc = "bytes read by the block manager";
	stats->block_byte_write.desc = "bytes written by the block manager";
	stats->block_map_read.desc = "mapped blocks read by the block manager";
	stats->block_preload.desc = "blocks pre-loaded by the block manager";
	stats->block_read.desc = "blocks read by the block manager";
	stats->block_write.desc = "blocks written by the block manager";
	stats->cache_bytes_dirty.desc =
	    "cache: tracked dirty bytes in the cache";
	stats->cache_bytes_inuse.desc = "cache: bytes currently in the cache";
	stats->cache_bytes_max.desc = "cache: maximum bytes configured";
	stats->cache_bytes_read.desc = "cache: bytes read into cache";
	stats->cache_bytes_write.desc = "cache: bytes written from cache";
	stats->cache_eviction_checkpoint.desc =
	    "cache: checkpoint blocked page eviction";
	stats->cache_eviction_clean.desc = "cache: unmodified pages evicted";
	stats->cache_eviction_dirty.desc = "cache: modified pages evicted";
	stats->cache_eviction_fail.desc =
	    "cache: pages selected for eviction unable to be evicted";
	stats->cache_eviction_hazard.desc =
	    "cache: hazard pointer blocked page eviction";
	stats->cache_eviction_internal.desc = "cache: internal pages evicted";
	stats->cache_eviction_merge.desc =
	    "cache: internal page merge operations completed";
	stats->cache_eviction_merge_fail.desc =
	    "cache: internal page merge attempts that could not complete";
	stats->cache_eviction_merge_levels.desc =
	    "cache: internal levels merged";
	stats->cache_eviction_slow.desc =
	    "cache: eviction server unable to reach eviction goal";
	stats->cache_eviction_walk.desc = "cache: pages walked for eviction";
	stats->cache_pages_dirty.desc =
	    "cache: tracked dirty pages in the cache";
	stats->cache_pages_inuse.desc =
	    "cache: pages currently held in the cache";
	stats->cache_read.desc = "cache: pages read into cache";
	stats->cache_write.desc = "cache: pages written from cache";
	stats->cond_wait.desc = "pthread mutex condition wait calls";
	stats->cursor_create.desc = "cursor creation";
	stats->cursor_insert.desc = "Btree cursor insert calls";
	stats->cursor_next.desc = "Btree cursor next calls";
	stats->cursor_prev.desc = "Btree cursor prev calls";
	stats->cursor_remove.desc = "Btree cursor remove calls";
	stats->cursor_reset.desc = "Btree cursor reset calls";
	stats->cursor_search.desc = "Btree cursor search calls";
	stats->cursor_search_near.desc = "Btree cursor search near calls";
	stats->cursor_update.desc = "Btree cursor update calls";
	stats->file_open.desc = "files currently open";
	stats->log_bytes_user.desc =
	    "log: total user provided log bytes written";
	stats->log_bytes_written.desc = "log: total log bytes written";
	stats->log_max_filesize.desc = "log: maximum log file size";
	stats->log_reads.desc = "log: total log read operations";
	stats->log_scan_records.desc =
	    "log: total records processed by log scan";
	stats->log_scan_rereads.desc =
	    "log: log scan records requiring two reads";
	stats->log_scans.desc = "log: total log scan operations";
	stats->log_slot_closes.desc = "log: total consolidated slot closures";
	stats->log_slot_consolidated.desc =
	    "log: total logging bytes consolidated";
	stats->log_slot_joins.desc = "log: total consolidated slot joins";
	stats->log_slot_races.desc = "log: total consolidated slot join races";
	stats->log_slot_toobig.desc = "log: record size exceeded maximum";
	stats->log_slot_transitions.desc =
	    "log: total consolidated slot join transitions";
	stats->log_sync.desc = "log: total log sync operations";
	stats->log_writes.desc = "log: total log write operations";
	stats->lsm_rows_merged.desc = "rows merged in an LSM tree";
	stats->memory_allocation.desc = "total heap memory allocations";
	stats->memory_free.desc = "total heap memory frees";
	stats->memory_grow.desc = "total heap memory re-allocations";
	stats->read_io.desc = "total read I/Os";
	stats->rec_pages.desc = "page reconciliation calls";
	stats->rec_pages_eviction.desc =
	    "page reconciliation calls for eviction";
	stats->rec_skipped_update.desc =
	    "reconciliation failed because an update could not be included";
	stats->rwlock_read.desc = "pthread mutex shared lock read-lock calls";
	stats->rwlock_write.desc =
	    "pthread mutex shared lock write-lock calls";
	stats->session_cursor_open.desc = "open cursor count";
	stats->txn_begin.desc = "transactions";
	stats->txn_checkpoint.desc = "transaction checkpoints";
	stats->txn_commit.desc = "transactions committed";
	stats->txn_fail_cache.desc =
	    "transaction failures due to cache overflow";
	stats->txn_rollback.desc = "transactions rolled-back";
	stats->write_io.desc = "total write I/Os";
}

void
__wt_stat_clear_connection_stats(void *stats_arg)
{
	WT_CONNECTION_STATS *stats;

	stats = (WT_CONNECTION_STATS *)stats_arg;
	stats->block_byte_map_read.v = 0;
	stats->block_byte_read.v = 0;
	stats->block_byte_write.v = 0;
	stats->block_map_read.v = 0;
	stats->block_preload.v = 0;
	stats->block_read.v = 0;
	stats->block_write.v = 0;
	stats->cache_bytes_dirty.v = 0;
	stats->cache_bytes_read.v = 0;
	stats->cache_bytes_write.v = 0;
	stats->cache_eviction_checkpoint.v = 0;
	stats->cache_eviction_clean.v = 0;
	stats->cache_eviction_dirty.v = 0;
	stats->cache_eviction_fail.v = 0;
	stats->cache_eviction_hazard.v = 0;
	stats->cache_eviction_internal.v = 0;
	stats->cache_eviction_merge.v = 0;
	stats->cache_eviction_merge_fail.v = 0;
	stats->cache_eviction_merge_levels.v = 0;
	stats->cache_eviction_slow.v = 0;
	stats->cache_eviction_walk.v = 0;
	stats->cache_pages_dirty.v = 0;
	stats->cache_read.v = 0;
	stats->cache_write.v = 0;
	stats->cond_wait.v = 0;
	stats->cursor_create.v = 0;
	stats->cursor_insert.v = 0;
	stats->cursor_next.v = 0;
	stats->cursor_prev.v = 0;
	stats->cursor_remove.v = 0;
	stats->cursor_reset.v = 0;
	stats->cursor_search.v = 0;
	stats->cursor_search_near.v = 0;
	stats->cursor_update.v = 0;
	stats->log_bytes_user.v = 0;
	stats->log_bytes_written.v = 0;
	stats->log_reads.v = 0;
	stats->log_scan_records.v = 0;
	stats->log_scan_rereads.v = 0;
	stats->log_scans.v = 0;
	stats->log_slot_closes.v = 0;
	stats->log_slot_consolidated.v = 0;
	stats->log_slot_joins.v = 0;
	stats->log_slot_races.v = 0;
	stats->log_slot_toobig.v = 0;
	stats->log_slot_transitions.v = 0;
	stats->log_sync.v = 0;
	stats->log_writes.v = 0;
	stats->lsm_rows_merged.v = 0;
	stats->memory_allocation.v = 0;
	stats->memory_free.v = 0;
	stats->memory_grow.v = 0;
	stats->read_io.v = 0;
	stats->rec_pages.v = 0;
	stats->rec_pages_eviction.v = 0;
	stats->rec_skipped_update.v = 0;
	stats->rwlock_read.v = 0;
	stats->rwlock_write.v = 0;
	stats->txn_begin.v = 0;
	stats->txn_checkpoint.v = 0;
	stats->txn_commit.v = 0;
	stats->txn_fail_cache.v = 0;
	stats->txn_rollback.v = 0;
	stats->write_io.v = 0;
}
