/* DO NOT EDIT: automatically built by dist/stat.py. */

#include "wt_internal.h"

void
__wt_stat_init_dsrc_stats(WT_DSRC_STATS *stats)
{
	/* Clear, so can also be called for reinitialization. */
	memset(stats, 0, sizeof(*stats));

	stats->allocation_size.desc =
	    "block manager: file allocation unit size";
	stats->block_alloc.desc = "block manager: blocks allocated";
	stats->block_checkpoint_size.desc = "block manager: checkpoint size";
	stats->block_extension.desc =
	    "block manager: allocations requiring file extension";
	stats->block_free.desc = "block manager: blocks freed";
	stats->block_magic.desc = "block manager: file magic number";
	stats->block_major.desc = "block manager: file major version number";
	stats->block_minor.desc = "block manager: minor version number";
	stats->block_reuse_bytes.desc =
	    "block manager: file bytes available for reuse";
	stats->block_size.desc = "block manager: file size in bytes";
	stats->bloom_count.desc = "LSM: bloom filters in the LSM tree";
	stats->bloom_false_positive.desc = "LSM: bloom filter false positives";
	stats->bloom_hit.desc = "LSM: bloom filter hits";
	stats->bloom_miss.desc = "LSM: bloom filter misses";
	stats->bloom_page_evict.desc =
	    "LSM: bloom filter pages evicted from cache";
	stats->bloom_page_read.desc =
	    "LSM: bloom filter pages read into cache";
	stats->bloom_size.desc = "LSM: total size of bloom filters";
	stats->btree_column_deleted.desc =
	    "btree: column-store variable-size deleted values";
	stats->btree_column_fix.desc =
	    "btree: column-store fixed-size leaf pages";
	stats->btree_column_internal.desc =
	    "btree: column-store internal pages";
	stats->btree_column_variable.desc =
	    "btree: column-store variable-size leaf pages";
	stats->btree_compact_rewrite.desc =
	    "btree: pages rewritten by compaction";
	stats->btree_entries.desc = "btree: number of key/value pairs";
	stats->btree_fixed_len.desc = "btree: fixed-record size";
	stats->btree_maximum_depth.desc = "btree: maximum tree depth";
	stats->btree_maxintlitem.desc =
	    "btree: maximum internal page item size";
	stats->btree_maxintlpage.desc = "btree: maximum internal page size";
	stats->btree_maxleafitem.desc = "btree: maximum leaf page item size";
	stats->btree_maxleafpage.desc = "btree: maximum leaf page size";
	stats->btree_overflow.desc = "btree: overflow pages";
	stats->btree_row_internal.desc = "btree: row-store internal pages";
	stats->btree_row_leaf.desc = "btree: row-store leaf pages";
	stats->cache_bytes_read.desc = "cache: bytes read into cache";
	stats->cache_bytes_write.desc = "cache: bytes written from cache";
	stats->cache_eviction_checkpoint.desc =
	    "cache: checkpoint blocked page eviction";
	stats->cache_eviction_clean.desc = "cache: unmodified pages evicted";
	stats->cache_eviction_dirty.desc = "cache: modified pages evicted";
	stats->cache_eviction_fail.desc =
	    "cache: data source pages selected for eviction unable to be evicted";
	stats->cache_eviction_hazard.desc =
	    "cache: hazard pointer blocked page eviction";
	stats->cache_eviction_internal.desc = "cache: internal pages evicted";
	stats->cache_overflow_value.desc =
	    "cache: overflow values cached in memory";
	stats->cache_read.desc = "cache: pages read into cache";
	stats->cache_read_overflow.desc =
	    "cache: overflow pages read into cache";
	stats->cache_write.desc = "cache: pages written from cache";
	stats->compress_raw_fail.desc =
	    "compression: raw compression call failed, no additional data available";
	stats->compress_raw_fail_temporary.desc =
	    "compression: raw compression call failed, additional data available";
	stats->compress_raw_ok.desc =
	    "compression: raw compression call succeeded";
	stats->compress_read.desc = "compression: compressed pages read";
	stats->compress_write.desc = "compression: compressed pages written";
	stats->compress_write_fail.desc =
	    "compression: page written failed to compress";
	stats->compress_write_too_small.desc =
	    "compression: page written was too small to compress";
	stats->cursor_create.desc = "cursor: create calls";
	stats->cursor_insert.desc = "cursor: insert calls";
	stats->cursor_insert_bulk.desc =
	    "cursor: bulk-loaded cursor-insert calls";
	stats->cursor_insert_bytes.desc =
	    "cursor: cursor-insert key and value bytes inserted";
	stats->cursor_next.desc = "cursor: next calls";
	stats->cursor_prev.desc = "cursor: prev calls";
	stats->cursor_remove.desc = "cursor: remove calls";
	stats->cursor_remove_bytes.desc =
	    "cursor: cursor-remove key bytes removed";
	stats->cursor_reset.desc = "cursor: reset calls";
	stats->cursor_search.desc = "cursor: search calls";
	stats->cursor_search_near.desc = "cursor: search near calls";
	stats->cursor_update.desc = "cursor: update calls";
	stats->cursor_update_bytes.desc =
	    "cursor: cursor-update value bytes updated";
	stats->lsm_checkpoint_throttle.desc =
	    "LSM: sleep for LSM checkpoint throttle";
	stats->lsm_chunk_count.desc = "LSM: chunks in the LSM tree";
	stats->lsm_generation_max.desc =
	    "LSM: highest merge generation in the LSM tree";
	stats->lsm_lookup_no_bloom.desc =
	    "LSM: queries that could have benefited from a Bloom filter that did not exist";
	stats->lsm_merge_throttle.desc = "LSM: sleep for LSM merge throttle";
	stats->rec_dictionary.desc = "reconciliation: dictionary matches";
	stats->rec_multiblock_internal.desc =
	    "reconciliation: internal page multi-block writes";
	stats->rec_multiblock_leaf.desc =
	    "reconciliation: leaf page multi-block writes";
	stats->rec_multiblock_max.desc =
	    "reconciliation: maximum blocks required for a page";
	stats->rec_overflow_key_internal.desc =
	    "reconciliation: internal-page overflow keys";
	stats->rec_overflow_key_leaf.desc =
	    "reconciliation: leaf-page overflow keys";
	stats->rec_overflow_value.desc =
	    "reconciliation: overflow values written";
	stats->rec_page_delete.desc = "reconciliation: pages deleted";
	stats->rec_page_match.desc = "reconciliation: page checksum matches";
	stats->rec_pages.desc = "reconciliation: page reconciliation calls";
	stats->rec_pages_eviction.desc =
	    "reconciliation: page reconciliation calls for eviction";
	stats->rec_prefix_compression.desc =
	    "reconciliation: leaf page key bytes discarded using prefix compression";
	stats->rec_suffix_compression.desc =
	    "reconciliation: internal page key bytes discarded using suffix compression";
	stats->session_compact.desc = "session: object compaction";
	stats->session_cursor_open.desc = "session: open cursor count";
	stats->txn_update_conflict.desc = "txn: update conflicts";
}

void
__wt_stat_refresh_dsrc_stats(void *stats_arg)
{
	WT_DSRC_STATS *stats;

	stats = (WT_DSRC_STATS *)stats_arg;
	stats->allocation_size.v = 0;
	stats->block_alloc.v = 0;
	stats->block_checkpoint_size.v = 0;
	stats->block_extension.v = 0;
	stats->block_free.v = 0;
	stats->block_magic.v = 0;
	stats->block_major.v = 0;
	stats->block_minor.v = 0;
	stats->block_reuse_bytes.v = 0;
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
	stats->lsm_checkpoint_throttle.v = 0;
	stats->lsm_chunk_count.v = 0;
	stats->lsm_generation_max.v = 0;
	stats->lsm_lookup_no_bloom.v = 0;
	stats->lsm_merge_throttle.v = 0;
	stats->rec_dictionary.v = 0;
	stats->rec_multiblock_internal.v = 0;
	stats->rec_multiblock_leaf.v = 0;
	stats->rec_multiblock_max.v = 0;
	stats->rec_overflow_key_internal.v = 0;
	stats->rec_overflow_key_leaf.v = 0;
	stats->rec_overflow_value.v = 0;
	stats->rec_page_delete.v = 0;
	stats->rec_page_match.v = 0;
	stats->rec_pages.v = 0;
	stats->rec_pages_eviction.v = 0;
	stats->rec_prefix_compression.v = 0;
	stats->rec_suffix_compression.v = 0;
	stats->session_compact.v = 0;
	stats->txn_update_conflict.v = 0;
}

void
__wt_stat_aggregate_dsrc_stats(const void *child, const void *parent)
{
	WT_DSRC_STATS *c, *p;

	c = (WT_DSRC_STATS *)child;
	p = (WT_DSRC_STATS *)parent;
	p->block_alloc.v += c->block_alloc.v;
	p->block_checkpoint_size.v += c->block_checkpoint_size.v;
	p->block_extension.v += c->block_extension.v;
	p->block_free.v += c->block_free.v;
	p->block_reuse_bytes.v += c->block_reuse_bytes.v;
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
	p->lsm_checkpoint_throttle.v += c->lsm_checkpoint_throttle.v;
	if (c->lsm_generation_max.v > p->lsm_generation_max.v)
	    p->lsm_generation_max.v = c->lsm_generation_max.v;
	p->lsm_lookup_no_bloom.v += c->lsm_lookup_no_bloom.v;
	p->lsm_merge_throttle.v += c->lsm_merge_throttle.v;
	p->rec_dictionary.v += c->rec_dictionary.v;
	p->rec_multiblock_internal.v += c->rec_multiblock_internal.v;
	p->rec_multiblock_leaf.v += c->rec_multiblock_leaf.v;
	if (c->rec_multiblock_max.v > p->rec_multiblock_max.v)
	    p->rec_multiblock_max.v = c->rec_multiblock_max.v;
	p->rec_overflow_key_internal.v += c->rec_overflow_key_internal.v;
	p->rec_overflow_key_leaf.v += c->rec_overflow_key_leaf.v;
	p->rec_overflow_value.v += c->rec_overflow_value.v;
	p->rec_page_delete.v += c->rec_page_delete.v;
	p->rec_page_match.v += c->rec_page_match.v;
	p->rec_pages.v += c->rec_pages.v;
	p->rec_pages_eviction.v += c->rec_pages_eviction.v;
	p->rec_prefix_compression.v += c->rec_prefix_compression.v;
	p->rec_suffix_compression.v += c->rec_suffix_compression.v;
	p->session_compact.v += c->session_compact.v;
	p->session_cursor_open.v += c->session_cursor_open.v;
	p->txn_update_conflict.v += c->txn_update_conflict.v;
}

void
__wt_stat_init_connection_stats(WT_CONNECTION_STATS *stats)
{
	/* Clear, so can also be called for reinitialization. */
	memset(stats, 0, sizeof(*stats));

	stats->async_alloc_race.desc =
	    "async: number of allocation state races";
	stats->async_alloc_view.desc =
	    "async: number of op slots viewed for alloc";
	stats->async_cur_queue.desc = "async: current work queue length";
	stats->async_flush.desc = "async: number of async flush calls";
	stats->async_full.desc = "async: number of times op allocation failed";
	stats->async_max_queue.desc = "async: maximum work queue length";
	stats->async_nowork.desc =
	    "async: number of times worker found no work";
	stats->async_op_alloc.desc = "async: op allocations";
	stats->async_op_compact.desc = "async: op compact calls";
	stats->async_op_insert.desc = "async: op insert calls";
	stats->async_op_remove.desc = "async: op remove calls";
	stats->async_op_search.desc = "async: op search calls";
	stats->async_op_update.desc = "async: op update calls";
	stats->block_byte_map_read.desc = "block manager: mapped bytes read";
	stats->block_byte_read.desc = "block manager: bytes read";
	stats->block_byte_write.desc = "block manager: bytes written";
	stats->block_map_read.desc = "block manager: mapped blocks read";
	stats->block_preload.desc = "block manager: blocks pre-loaded";
	stats->block_read.desc = "block manager: blocks read";
	stats->block_write.desc = "block manager: blocks written";
	stats->cache_bytes_dirty.desc =
	    "cache: tracked dirty bytes in the cache";
	stats->cache_bytes_inuse.desc = "cache: bytes currently in the cache";
	stats->cache_bytes_max.desc = "cache: maximum bytes configured";
	stats->cache_bytes_read.desc = "cache: bytes read into cache";
	stats->cache_bytes_write.desc = "cache: bytes written from cache";
	stats->cache_eviction_checkpoint.desc =
	    "cache: checkpoint blocked page eviction";
	stats->cache_eviction_clean.desc = "cache: unmodified pages evicted";
	stats->cache_eviction_deepen.desc =
	    "cache: page split during eviction deepened the tree";
	stats->cache_eviction_dirty.desc = "cache: modified pages evicted";
	stats->cache_eviction_fail.desc =
	    "cache: pages selected for eviction unable to be evicted";
	stats->cache_eviction_force.desc =
	    "cache: pages evicted because they exceeded the in-memory maximum";
	stats->cache_eviction_force_fail.desc =
	    "cache: failed eviction of pages that exceeded the in-memory maximum";
	stats->cache_eviction_hazard.desc =
	    "cache: hazard pointer blocked page eviction";
	stats->cache_eviction_internal.desc = "cache: internal pages evicted";
	stats->cache_eviction_queue_empty.desc =
	    "cache: eviction server candidate queue empty when topping up";
	stats->cache_eviction_queue_not_empty.desc =
	    "cache: eviction server candidate queue not empty when topping up";
	stats->cache_eviction_server_evicting.desc =
	    "cache: eviction server evicting pages";
	stats->cache_eviction_server_not_evicting.desc =
	    "cache: eviction server populating queue, but not evicting pages";
	stats->cache_eviction_slow.desc =
	    "cache: eviction server unable to reach eviction goal";
	stats->cache_eviction_split.desc =
	    "cache: pages split during eviction";
	stats->cache_eviction_walk.desc = "cache: pages walked for eviction";
	stats->cache_pages_dirty.desc =
	    "cache: tracked dirty pages in the cache";
	stats->cache_pages_inuse.desc =
	    "cache: pages currently held in the cache";
	stats->cache_read.desc = "cache: pages read into cache";
	stats->cache_write.desc = "cache: pages written from cache";
	stats->cond_wait.desc = "conn: pthread mutex condition wait calls";
	stats->cursor_create.desc = "Btree: cursor create calls";
	stats->cursor_insert.desc = "Btree: cursor insert calls";
	stats->cursor_next.desc = "Btree: cursor next calls";
	stats->cursor_prev.desc = "Btree: cursor prev calls";
	stats->cursor_remove.desc = "Btree: cursor remove calls";
	stats->cursor_reset.desc = "Btree: cursor reset calls";
	stats->cursor_search.desc = "Btree: cursor search calls";
	stats->cursor_search_near.desc = "Btree: cursor search near calls";
	stats->cursor_update.desc = "Btree: cursor update calls";
	stats->dh_session_handles.desc = "dhandle: session dhandles swept";
	stats->dh_session_sweeps.desc = "dhandle: session sweep attempts";
	stats->file_open.desc = "conn: files currently open";
	stats->log_buffer_grow.desc = "log: log buffer size increases";
	stats->log_buffer_size.desc = "log: total log buffer size";
	stats->log_bytes_user.desc = "log: user provided log bytes written";
	stats->log_bytes_written.desc = "log: log bytes written";
	stats->log_close_yields.desc =
	    "log: yields waiting for previous log file close";
	stats->log_max_filesize.desc = "log: maximum log file size";
	stats->log_reads.desc = "log: log read operations";
	stats->log_scan_records.desc = "log: records processed by log scan";
	stats->log_scan_rereads.desc =
	    "log: log scan records requiring two reads";
	stats->log_scans.desc = "log: log scan operations";
	stats->log_slot_closes.desc = "log: consolidated slot closures";
	stats->log_slot_consolidated.desc = "log: logging bytes consolidated";
	stats->log_slot_joins.desc = "log: consolidated slot joins";
	stats->log_slot_races.desc = "log: consolidated slot join races";
	stats->log_slot_switch_fails.desc =
	    "log: slots selected for switching that were unavailable";
	stats->log_slot_toobig.desc = "log: record size exceeded maximum";
	stats->log_slot_toosmall.desc =
	    "log: failed to find a slot large enough for record";
	stats->log_slot_transitions.desc =
	    "log: consolidated slot join transitions";
	stats->log_sync.desc = "log: log sync operations";
	stats->log_writes.desc = "log: log write operations";
	stats->lsm_checkpoint_throttle.desc =
	    "LSM: sleep for LSM checkpoint throttle";
	stats->lsm_merge_throttle.desc = "LSM: sleep for LSM merge throttle";
	stats->lsm_rows_merged.desc = "LSM: rows merged in an LSM tree";
	stats->lsm_work_queue_app.desc =
	    "LSM: App work units currently queued";
	stats->lsm_work_queue_manager.desc =
	    "LSM: Merge work units currently queued";
	stats->lsm_work_queue_max.desc = "LSM: tree queue hit maximum";
	stats->lsm_work_queue_switch.desc =
	    "LSM: Switch work units currently queued";
	stats->lsm_work_units_created.desc =
	    "LSM: tree maintenance operations scheduled";
	stats->lsm_work_units_discarded.desc =
	    "LSM: tree maintenance operations discarded";
	stats->lsm_work_units_done.desc =
	    "LSM: tree maintenance operations executed";
	stats->memory_allocation.desc = "conn: memory allocations";
	stats->memory_free.desc = "conn: memory frees";
	stats->memory_grow.desc = "conn: memory re-allocations";
	stats->read_io.desc = "conn: total read I/Os";
	stats->rec_pages.desc = "reconciliation: page reconciliation calls";
	stats->rec_pages_eviction.desc =
	    "reconciliation: page reconciliation calls for eviction";
	stats->rec_split_stashed_bytes.desc =
	    "reconciliation: split bytes currently awaiting free";
	stats->rec_split_stashed_objects.desc =
	    "reconciliation: split objects currently awaiting free";
	stats->rwlock_read.desc =
	    "conn: pthread mutex shared lock read-lock calls";
	stats->rwlock_write.desc =
	    "conn: pthread mutex shared lock write-lock calls";
	stats->session_cursor_open.desc = "session: open cursor count";
	stats->session_open.desc = "session: open session count";
	stats->txn_begin.desc = "txn: transaction begins";
	stats->txn_checkpoint.desc = "txn: transaction checkpoints";
	stats->txn_checkpoint_running.desc =
	    "txn: transaction checkpoint currently running";
	stats->txn_commit.desc = "txn: transactions committed";
	stats->txn_fail_cache.desc =
	    "txn: transaction failures due to cache overflow";
	stats->txn_pinned_range.desc =
	    "txn: transaction range of IDs currently pinned";
	stats->txn_rollback.desc = "txn: transactions rolled back";
	stats->write_io.desc = "conn: total write I/Os";
}

void
__wt_stat_refresh_connection_stats(void *stats_arg)
{
	WT_CONNECTION_STATS *stats;

	stats = (WT_CONNECTION_STATS *)stats_arg;
	stats->async_alloc_race.v = 0;
	stats->async_alloc_view.v = 0;
	stats->async_cur_queue.v = 0;
	stats->async_flush.v = 0;
	stats->async_full.v = 0;
	stats->async_max_queue.v = 0;
	stats->async_nowork.v = 0;
	stats->async_op_alloc.v = 0;
	stats->async_op_compact.v = 0;
	stats->async_op_insert.v = 0;
	stats->async_op_remove.v = 0;
	stats->async_op_search.v = 0;
	stats->async_op_update.v = 0;
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
	stats->cache_eviction_deepen.v = 0;
	stats->cache_eviction_dirty.v = 0;
	stats->cache_eviction_fail.v = 0;
	stats->cache_eviction_force.v = 0;
	stats->cache_eviction_force_fail.v = 0;
	stats->cache_eviction_hazard.v = 0;
	stats->cache_eviction_internal.v = 0;
	stats->cache_eviction_queue_empty.v = 0;
	stats->cache_eviction_queue_not_empty.v = 0;
	stats->cache_eviction_server_evicting.v = 0;
	stats->cache_eviction_server_not_evicting.v = 0;
	stats->cache_eviction_slow.v = 0;
	stats->cache_eviction_split.v = 0;
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
	stats->dh_session_handles.v = 0;
	stats->dh_session_sweeps.v = 0;
	stats->log_buffer_grow.v = 0;
	stats->log_bytes_user.v = 0;
	stats->log_bytes_written.v = 0;
	stats->log_close_yields.v = 0;
	stats->log_reads.v = 0;
	stats->log_scan_records.v = 0;
	stats->log_scan_rereads.v = 0;
	stats->log_scans.v = 0;
	stats->log_slot_closes.v = 0;
	stats->log_slot_consolidated.v = 0;
	stats->log_slot_joins.v = 0;
	stats->log_slot_races.v = 0;
	stats->log_slot_switch_fails.v = 0;
	stats->log_slot_toobig.v = 0;
	stats->log_slot_toosmall.v = 0;
	stats->log_slot_transitions.v = 0;
	stats->log_sync.v = 0;
	stats->log_writes.v = 0;
	stats->lsm_checkpoint_throttle.v = 0;
	stats->lsm_merge_throttle.v = 0;
	stats->lsm_rows_merged.v = 0;
	stats->lsm_work_queue_max.v = 0;
	stats->lsm_work_units_created.v = 0;
	stats->lsm_work_units_discarded.v = 0;
	stats->lsm_work_units_done.v = 0;
	stats->memory_allocation.v = 0;
	stats->memory_free.v = 0;
	stats->memory_grow.v = 0;
	stats->read_io.v = 0;
	stats->rec_pages.v = 0;
	stats->rec_pages_eviction.v = 0;
	stats->rwlock_read.v = 0;
	stats->rwlock_write.v = 0;
	stats->txn_begin.v = 0;
	stats->txn_checkpoint.v = 0;
	stats->txn_commit.v = 0;
	stats->txn_fail_cache.v = 0;
	stats->txn_rollback.v = 0;
	stats->write_io.v = 0;
}
