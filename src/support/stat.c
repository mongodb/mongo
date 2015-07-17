/* DO NOT EDIT: automatically built by dist/stat.py. */

#include "wt_internal.h"

void
__wt_stat_init_dsrc_stats(WT_DSRC_STATS *stats)
{
	/* Clear, so can also be called for reinitialization. */
	memset(stats, 0, sizeof(*stats));

	stats->block_extension.desc =
	    "block-manager: allocations requiring file extension";
	stats->block_alloc.desc = "block-manager: blocks allocated";
	stats->block_free.desc = "block-manager: blocks freed";
	stats->block_checkpoint_size.desc = "block-manager: checkpoint size";
	stats->allocation_size.desc =
	    "block-manager: file allocation unit size";
	stats->block_reuse_bytes.desc =
	    "block-manager: file bytes available for reuse";
	stats->block_magic.desc = "block-manager: file magic number";
	stats->block_major.desc = "block-manager: file major version number";
	stats->block_size.desc = "block-manager: file size in bytes";
	stats->block_minor.desc = "block-manager: minor version number";
	stats->btree_checkpoint_generation.desc =
	    "btree: btree checkpoint generation";
	stats->btree_column_fix.desc =
	    "btree: column-store fixed-size leaf pages";
	stats->btree_column_internal.desc =
	    "btree: column-store internal pages";
	stats->btree_column_deleted.desc =
	    "btree: column-store variable-size deleted values";
	stats->btree_column_variable.desc =
	    "btree: column-store variable-size leaf pages";
	stats->btree_fixed_len.desc = "btree: fixed-record size";
	stats->btree_maxintlkey.desc = "btree: maximum internal page key size";
	stats->btree_maxintlpage.desc = "btree: maximum internal page size";
	stats->btree_maxleafkey.desc = "btree: maximum leaf page key size";
	stats->btree_maxleafpage.desc = "btree: maximum leaf page size";
	stats->btree_maxleafvalue.desc = "btree: maximum leaf page value size";
	stats->btree_maximum_depth.desc = "btree: maximum tree depth";
	stats->btree_entries.desc = "btree: number of key/value pairs";
	stats->btree_overflow.desc = "btree: overflow pages";
	stats->btree_compact_rewrite.desc =
	    "btree: pages rewritten by compaction";
	stats->btree_row_internal.desc = "btree: row-store internal pages";
	stats->btree_row_leaf.desc = "btree: row-store leaf pages";
	stats->cache_bytes_read.desc = "cache: bytes read into cache";
	stats->cache_bytes_write.desc = "cache: bytes written from cache";
	stats->cache_eviction_checkpoint.desc =
	    "cache: checkpoint blocked page eviction";
	stats->cache_eviction_fail.desc =
	    "cache: data source pages selected for eviction unable to be evicted";
	stats->cache_eviction_hazard.desc =
	    "cache: hazard pointer blocked page eviction";
	stats->cache_inmem_split.desc = "cache: in-memory page splits";
	stats->cache_eviction_internal.desc = "cache: internal pages evicted";
	stats->cache_eviction_dirty.desc = "cache: modified pages evicted";
	stats->cache_read_overflow.desc =
	    "cache: overflow pages read into cache";
	stats->cache_overflow_value.desc =
	    "cache: overflow values cached in memory";
	stats->cache_eviction_deepen.desc =
	    "cache: page split during eviction deepened the tree";
	stats->cache_read.desc = "cache: pages read into cache";
	stats->cache_eviction_split.desc =
	    "cache: pages split during eviction";
	stats->cache_write.desc = "cache: pages written from cache";
	stats->cache_eviction_clean.desc = "cache: unmodified pages evicted";
	stats->compress_read.desc = "compression: compressed pages read";
	stats->compress_write.desc = "compression: compressed pages written";
	stats->compress_write_fail.desc =
	    "compression: page written failed to compress";
	stats->compress_write_too_small.desc =
	    "compression: page written was too small to compress";
	stats->compress_raw_fail_temporary.desc =
	    "compression: raw compression call failed, additional data available";
	stats->compress_raw_fail.desc =
	    "compression: raw compression call failed, no additional data available";
	stats->compress_raw_ok.desc =
	    "compression: raw compression call succeeded";
	stats->cursor_insert_bulk.desc =
	    "cursor: bulk-loaded cursor-insert calls";
	stats->cursor_create.desc = "cursor: create calls";
	stats->cursor_insert_bytes.desc =
	    "cursor: cursor-insert key and value bytes inserted";
	stats->cursor_remove_bytes.desc =
	    "cursor: cursor-remove key bytes removed";
	stats->cursor_update_bytes.desc =
	    "cursor: cursor-update value bytes updated";
	stats->cursor_insert.desc = "cursor: insert calls";
	stats->cursor_next.desc = "cursor: next calls";
	stats->cursor_prev.desc = "cursor: prev calls";
	stats->cursor_remove.desc = "cursor: remove calls";
	stats->cursor_reset.desc = "cursor: reset calls";
	stats->cursor_search.desc = "cursor: search calls";
	stats->cursor_search_near.desc = "cursor: search near calls";
	stats->cursor_update.desc = "cursor: update calls";
	stats->bloom_false_positive.desc = "LSM: bloom filter false positives";
	stats->bloom_hit.desc = "LSM: bloom filter hits";
	stats->bloom_miss.desc = "LSM: bloom filter misses";
	stats->bloom_page_evict.desc =
	    "LSM: bloom filter pages evicted from cache";
	stats->bloom_page_read.desc =
	    "LSM: bloom filter pages read into cache";
	stats->bloom_count.desc = "LSM: bloom filters in the LSM tree";
	stats->lsm_chunk_count.desc = "LSM: chunks in the LSM tree";
	stats->lsm_generation_max.desc =
	    "LSM: highest merge generation in the LSM tree";
	stats->lsm_lookup_no_bloom.desc =
	    "LSM: queries that could have benefited from a Bloom filter that did not exist";
	stats->lsm_checkpoint_throttle.desc =
	    "LSM: sleep for LSM checkpoint throttle";
	stats->lsm_merge_throttle.desc = "LSM: sleep for LSM merge throttle";
	stats->bloom_size.desc = "LSM: total size of bloom filters";
	stats->rec_dictionary.desc = "reconciliation: dictionary matches";
	stats->rec_suffix_compression.desc =
	    "reconciliation: internal page key bytes discarded using suffix compression";
	stats->rec_multiblock_internal.desc =
	    "reconciliation: internal page multi-block writes";
	stats->rec_overflow_key_internal.desc =
	    "reconciliation: internal-page overflow keys";
	stats->rec_prefix_compression.desc =
	    "reconciliation: leaf page key bytes discarded using prefix compression";
	stats->rec_multiblock_leaf.desc =
	    "reconciliation: leaf page multi-block writes";
	stats->rec_overflow_key_leaf.desc =
	    "reconciliation: leaf-page overflow keys";
	stats->rec_multiblock_max.desc =
	    "reconciliation: maximum blocks required for a page";
	stats->rec_overflow_value.desc =
	    "reconciliation: overflow values written";
	stats->rec_page_match.desc = "reconciliation: page checksum matches";
	stats->rec_pages.desc = "reconciliation: page reconciliation calls";
	stats->rec_pages_eviction.desc =
	    "reconciliation: page reconciliation calls for eviction";
	stats->rec_page_delete.desc = "reconciliation: pages deleted";
	stats->session_compact.desc = "session: object compaction";
	stats->session_cursor_open.desc = "session: open cursor count";
	stats->txn_update_conflict.desc = "transaction: update conflicts";
}

void
__wt_stat_refresh_dsrc_stats(void *stats_arg)
{
	WT_DSRC_STATS *stats;

	stats = (WT_DSRC_STATS *)stats_arg;
	stats->block_extension.v = 0;
	stats->block_alloc.v = 0;
	stats->block_free.v = 0;
	stats->block_checkpoint_size.v = 0;
	stats->allocation_size.v = 0;
	stats->block_reuse_bytes.v = 0;
	stats->block_magic.v = 0;
	stats->block_major.v = 0;
	stats->block_size.v = 0;
	stats->block_minor.v = 0;
	stats->btree_column_fix.v = 0;
	stats->btree_column_internal.v = 0;
	stats->btree_column_deleted.v = 0;
	stats->btree_column_variable.v = 0;
	stats->btree_fixed_len.v = 0;
	stats->btree_maxintlkey.v = 0;
	stats->btree_maxintlpage.v = 0;
	stats->btree_maxleafkey.v = 0;
	stats->btree_maxleafpage.v = 0;
	stats->btree_maxleafvalue.v = 0;
	stats->btree_maximum_depth.v = 0;
	stats->btree_entries.v = 0;
	stats->btree_overflow.v = 0;
	stats->btree_compact_rewrite.v = 0;
	stats->btree_row_internal.v = 0;
	stats->btree_row_leaf.v = 0;
	stats->cache_bytes_read.v = 0;
	stats->cache_bytes_write.v = 0;
	stats->cache_eviction_checkpoint.v = 0;
	stats->cache_eviction_fail.v = 0;
	stats->cache_eviction_hazard.v = 0;
	stats->cache_inmem_split.v = 0;
	stats->cache_eviction_internal.v = 0;
	stats->cache_eviction_dirty.v = 0;
	stats->cache_read_overflow.v = 0;
	stats->cache_overflow_value.v = 0;
	stats->cache_eviction_deepen.v = 0;
	stats->cache_read.v = 0;
	stats->cache_eviction_split.v = 0;
	stats->cache_write.v = 0;
	stats->cache_eviction_clean.v = 0;
	stats->compress_read.v = 0;
	stats->compress_write.v = 0;
	stats->compress_write_fail.v = 0;
	stats->compress_write_too_small.v = 0;
	stats->compress_raw_fail_temporary.v = 0;
	stats->compress_raw_fail.v = 0;
	stats->compress_raw_ok.v = 0;
	stats->cursor_insert_bulk.v = 0;
	stats->cursor_create.v = 0;
	stats->cursor_insert_bytes.v = 0;
	stats->cursor_remove_bytes.v = 0;
	stats->cursor_update_bytes.v = 0;
	stats->cursor_insert.v = 0;
	stats->cursor_next.v = 0;
	stats->cursor_prev.v = 0;
	stats->cursor_remove.v = 0;
	stats->cursor_reset.v = 0;
	stats->cursor_search.v = 0;
	stats->cursor_search_near.v = 0;
	stats->cursor_update.v = 0;
	stats->bloom_false_positive.v = 0;
	stats->bloom_hit.v = 0;
	stats->bloom_miss.v = 0;
	stats->bloom_page_evict.v = 0;
	stats->bloom_page_read.v = 0;
	stats->bloom_count.v = 0;
	stats->lsm_chunk_count.v = 0;
	stats->lsm_generation_max.v = 0;
	stats->lsm_lookup_no_bloom.v = 0;
	stats->lsm_checkpoint_throttle.v = 0;
	stats->lsm_merge_throttle.v = 0;
	stats->bloom_size.v = 0;
	stats->rec_dictionary.v = 0;
	stats->rec_suffix_compression.v = 0;
	stats->rec_multiblock_internal.v = 0;
	stats->rec_overflow_key_internal.v = 0;
	stats->rec_prefix_compression.v = 0;
	stats->rec_multiblock_leaf.v = 0;
	stats->rec_overflow_key_leaf.v = 0;
	stats->rec_multiblock_max.v = 0;
	stats->rec_overflow_value.v = 0;
	stats->rec_page_match.v = 0;
	stats->rec_pages.v = 0;
	stats->rec_pages_eviction.v = 0;
	stats->rec_page_delete.v = 0;
	stats->session_compact.v = 0;
	stats->txn_update_conflict.v = 0;
}

void
__wt_stat_aggregate_dsrc_stats(const void *child, const void *parent)
{
	WT_DSRC_STATS *c, *p;

	c = (WT_DSRC_STATS *)child;
	p = (WT_DSRC_STATS *)parent;
	p->block_extension.v += c->block_extension.v;
	p->block_alloc.v += c->block_alloc.v;
	p->block_free.v += c->block_free.v;
	p->block_checkpoint_size.v += c->block_checkpoint_size.v;
	p->block_reuse_bytes.v += c->block_reuse_bytes.v;
	p->block_size.v += c->block_size.v;
	p->btree_checkpoint_generation.v += c->btree_checkpoint_generation.v;
	p->btree_column_fix.v += c->btree_column_fix.v;
	p->btree_column_internal.v += c->btree_column_internal.v;
	p->btree_column_deleted.v += c->btree_column_deleted.v;
	p->btree_column_variable.v += c->btree_column_variable.v;
	if (c->btree_maxintlkey.v > p->btree_maxintlkey.v)
	    p->btree_maxintlkey.v = c->btree_maxintlkey.v;
	if (c->btree_maxintlpage.v > p->btree_maxintlpage.v)
	    p->btree_maxintlpage.v = c->btree_maxintlpage.v;
	if (c->btree_maxleafkey.v > p->btree_maxleafkey.v)
	    p->btree_maxleafkey.v = c->btree_maxleafkey.v;
	if (c->btree_maxleafpage.v > p->btree_maxleafpage.v)
	    p->btree_maxleafpage.v = c->btree_maxleafpage.v;
	if (c->btree_maxleafvalue.v > p->btree_maxleafvalue.v)
	    p->btree_maxleafvalue.v = c->btree_maxleafvalue.v;
	if (c->btree_maximum_depth.v > p->btree_maximum_depth.v)
	    p->btree_maximum_depth.v = c->btree_maximum_depth.v;
	p->btree_entries.v += c->btree_entries.v;
	p->btree_overflow.v += c->btree_overflow.v;
	p->btree_compact_rewrite.v += c->btree_compact_rewrite.v;
	p->btree_row_internal.v += c->btree_row_internal.v;
	p->btree_row_leaf.v += c->btree_row_leaf.v;
	p->cache_bytes_read.v += c->cache_bytes_read.v;
	p->cache_bytes_write.v += c->cache_bytes_write.v;
	p->cache_eviction_checkpoint.v += c->cache_eviction_checkpoint.v;
	p->cache_eviction_fail.v += c->cache_eviction_fail.v;
	p->cache_eviction_hazard.v += c->cache_eviction_hazard.v;
	p->cache_inmem_split.v += c->cache_inmem_split.v;
	p->cache_eviction_internal.v += c->cache_eviction_internal.v;
	p->cache_eviction_dirty.v += c->cache_eviction_dirty.v;
	p->cache_read_overflow.v += c->cache_read_overflow.v;
	p->cache_overflow_value.v += c->cache_overflow_value.v;
	p->cache_eviction_deepen.v += c->cache_eviction_deepen.v;
	p->cache_read.v += c->cache_read.v;
	p->cache_eviction_split.v += c->cache_eviction_split.v;
	p->cache_write.v += c->cache_write.v;
	p->cache_eviction_clean.v += c->cache_eviction_clean.v;
	p->compress_read.v += c->compress_read.v;
	p->compress_write.v += c->compress_write.v;
	p->compress_write_fail.v += c->compress_write_fail.v;
	p->compress_write_too_small.v += c->compress_write_too_small.v;
	p->compress_raw_fail_temporary.v += c->compress_raw_fail_temporary.v;
	p->compress_raw_fail.v += c->compress_raw_fail.v;
	p->compress_raw_ok.v += c->compress_raw_ok.v;
	p->cursor_insert_bulk.v += c->cursor_insert_bulk.v;
	p->cursor_create.v += c->cursor_create.v;
	p->cursor_insert_bytes.v += c->cursor_insert_bytes.v;
	p->cursor_remove_bytes.v += c->cursor_remove_bytes.v;
	p->cursor_update_bytes.v += c->cursor_update_bytes.v;
	p->cursor_insert.v += c->cursor_insert.v;
	p->cursor_next.v += c->cursor_next.v;
	p->cursor_prev.v += c->cursor_prev.v;
	p->cursor_remove.v += c->cursor_remove.v;
	p->cursor_reset.v += c->cursor_reset.v;
	p->cursor_search.v += c->cursor_search.v;
	p->cursor_search_near.v += c->cursor_search_near.v;
	p->cursor_update.v += c->cursor_update.v;
	p->bloom_false_positive.v += c->bloom_false_positive.v;
	p->bloom_hit.v += c->bloom_hit.v;
	p->bloom_miss.v += c->bloom_miss.v;
	p->bloom_page_evict.v += c->bloom_page_evict.v;
	p->bloom_page_read.v += c->bloom_page_read.v;
	p->bloom_count.v += c->bloom_count.v;
	p->lsm_chunk_count.v += c->lsm_chunk_count.v;
	if (c->lsm_generation_max.v > p->lsm_generation_max.v)
	    p->lsm_generation_max.v = c->lsm_generation_max.v;
	p->lsm_lookup_no_bloom.v += c->lsm_lookup_no_bloom.v;
	p->lsm_checkpoint_throttle.v += c->lsm_checkpoint_throttle.v;
	p->lsm_merge_throttle.v += c->lsm_merge_throttle.v;
	p->bloom_size.v += c->bloom_size.v;
	p->rec_dictionary.v += c->rec_dictionary.v;
	p->rec_suffix_compression.v += c->rec_suffix_compression.v;
	p->rec_multiblock_internal.v += c->rec_multiblock_internal.v;
	p->rec_overflow_key_internal.v += c->rec_overflow_key_internal.v;
	p->rec_prefix_compression.v += c->rec_prefix_compression.v;
	p->rec_multiblock_leaf.v += c->rec_multiblock_leaf.v;
	p->rec_overflow_key_leaf.v += c->rec_overflow_key_leaf.v;
	if (c->rec_multiblock_max.v > p->rec_multiblock_max.v)
	    p->rec_multiblock_max.v = c->rec_multiblock_max.v;
	p->rec_overflow_value.v += c->rec_overflow_value.v;
	p->rec_page_match.v += c->rec_page_match.v;
	p->rec_pages.v += c->rec_pages.v;
	p->rec_pages_eviction.v += c->rec_pages_eviction.v;
	p->rec_page_delete.v += c->rec_page_delete.v;
	p->session_compact.v += c->session_compact.v;
	p->session_cursor_open.v += c->session_cursor_open.v;
	p->txn_update_conflict.v += c->txn_update_conflict.v;
}

void
__wt_stat_init_connection_stats(WT_CONNECTION_STATS *stats)
{
	/* Clear, so can also be called for reinitialization. */
	memset(stats, 0, sizeof(*stats));

	stats->async_cur_queue.desc = "async: current work queue length";
	stats->async_max_queue.desc = "async: maximum work queue length";
	stats->async_alloc_race.desc =
	    "async: number of allocation state races";
	stats->async_flush.desc = "async: number of flush calls";
	stats->async_alloc_view.desc =
	    "async: number of operation slots viewed for allocation";
	stats->async_full.desc =
	    "async: number of times operation allocation failed";
	stats->async_nowork.desc =
	    "async: number of times worker found no work";
	stats->async_op_alloc.desc = "async: total allocations";
	stats->async_op_compact.desc = "async: total compact calls";
	stats->async_op_insert.desc = "async: total insert calls";
	stats->async_op_remove.desc = "async: total remove calls";
	stats->async_op_search.desc = "async: total search calls";
	stats->async_op_update.desc = "async: total update calls";
	stats->block_preload.desc = "block-manager: blocks pre-loaded";
	stats->block_read.desc = "block-manager: blocks read";
	stats->block_write.desc = "block-manager: blocks written";
	stats->block_byte_read.desc = "block-manager: bytes read";
	stats->block_byte_write.desc = "block-manager: bytes written";
	stats->block_map_read.desc = "block-manager: mapped blocks read";
	stats->block_byte_map_read.desc = "block-manager: mapped bytes read";
	stats->cache_bytes_inuse.desc = "cache: bytes currently in the cache";
	stats->cache_bytes_read.desc = "cache: bytes read into cache";
	stats->cache_bytes_write.desc = "cache: bytes written from cache";
	stats->cache_eviction_checkpoint.desc =
	    "cache: checkpoint blocked page eviction";
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
	stats->cache_eviction_worker_evicting.desc =
	    "cache: eviction worker thread evicting pages";
	stats->cache_eviction_force_fail.desc =
	    "cache: failed eviction of pages that exceeded the in-memory maximum";
	stats->cache_eviction_hazard.desc =
	    "cache: hazard pointer blocked page eviction";
	stats->cache_inmem_split.desc = "cache: in-memory page splits";
	stats->cache_eviction_internal.desc = "cache: internal pages evicted";
	stats->cache_bytes_max.desc = "cache: maximum bytes configured";
	stats->cache_eviction_maximum_page_size.desc =
	    "cache: maximum page size at eviction";
	stats->cache_eviction_dirty.desc = "cache: modified pages evicted";
	stats->cache_eviction_deepen.desc =
	    "cache: page split during eviction deepened the tree";
	stats->cache_pages_inuse.desc =
	    "cache: pages currently held in the cache";
	stats->cache_eviction_force.desc =
	    "cache: pages evicted because they exceeded the in-memory maximum";
	stats->cache_eviction_force_delete.desc =
	    "cache: pages evicted because they had chains of deleted items";
	stats->cache_eviction_app.desc =
	    "cache: pages evicted by application threads";
	stats->cache_read.desc = "cache: pages read into cache";
	stats->cache_eviction_fail.desc =
	    "cache: pages selected for eviction unable to be evicted";
	stats->cache_eviction_split.desc =
	    "cache: pages split during eviction";
	stats->cache_eviction_walk.desc = "cache: pages walked for eviction";
	stats->cache_write.desc = "cache: pages written from cache";
	stats->cache_overhead.desc = "cache: percentage overhead";
	stats->cache_bytes_internal.desc =
	    "cache: tracked bytes belonging to internal pages in the cache";
	stats->cache_bytes_leaf.desc =
	    "cache: tracked bytes belonging to leaf pages in the cache";
	stats->cache_bytes_overflow.desc =
	    "cache: tracked bytes belonging to overflow pages in the cache";
	stats->cache_bytes_dirty.desc =
	    "cache: tracked dirty bytes in the cache";
	stats->cache_pages_dirty.desc =
	    "cache: tracked dirty pages in the cache";
	stats->cache_eviction_clean.desc = "cache: unmodified pages evicted";
	stats->file_open.desc = "connection: files currently open";
	stats->memory_allocation.desc = "connection: memory allocations";
	stats->memory_free.desc = "connection: memory frees";
	stats->memory_grow.desc = "connection: memory re-allocations";
	stats->cond_wait.desc =
	    "connection: pthread mutex condition wait calls";
	stats->rwlock_read.desc =
	    "connection: pthread mutex shared lock read-lock calls";
	stats->rwlock_write.desc =
	    "connection: pthread mutex shared lock write-lock calls";
	stats->read_io.desc = "connection: total read I/Os";
	stats->write_io.desc = "connection: total write I/Os";
	stats->cursor_create.desc = "cursor: cursor create calls";
	stats->cursor_insert.desc = "cursor: cursor insert calls";
	stats->cursor_next.desc = "cursor: cursor next calls";
	stats->cursor_prev.desc = "cursor: cursor prev calls";
	stats->cursor_remove.desc = "cursor: cursor remove calls";
	stats->cursor_reset.desc = "cursor: cursor reset calls";
	stats->cursor_search.desc = "cursor: cursor search calls";
	stats->cursor_search_near.desc = "cursor: cursor search near calls";
	stats->cursor_update.desc = "cursor: cursor update calls";
	stats->dh_conn_ref.desc =
	    "data-handle: connection candidate referenced";
	stats->dh_conn_handles.desc = "data-handle: connection dhandles swept";
	stats->dh_conn_sweeps.desc = "data-handle: connection sweeps";
	stats->dh_conn_tod.desc = "data-handle: connection time-of-death sets";
	stats->dh_session_handles.desc = "data-handle: session dhandles swept";
	stats->dh_session_sweeps.desc = "data-handle: session sweep attempts";
	stats->log_slot_closes.desc = "log: consolidated slot closures";
	stats->log_slot_races.desc = "log: consolidated slot join races";
	stats->log_slot_transitions.desc =
	    "log: consolidated slot join transitions";
	stats->log_slot_joins.desc = "log: consolidated slot joins";
	stats->log_slot_toosmall.desc =
	    "log: failed to find a slot large enough for record";
	stats->log_bytes_payload.desc = "log: log bytes of payload data";
	stats->log_bytes_written.desc = "log: log bytes written";
	stats->log_compress_writes.desc = "log: log records compressed";
	stats->log_compress_write_fails.desc =
	    "log: log records not compressed";
	stats->log_compress_small.desc =
	    "log: log records too small to compress";
	stats->log_release_write_lsn.desc =
	    "log: log release advances write LSN";
	stats->log_scans.desc = "log: log scan operations";
	stats->log_scan_rereads.desc =
	    "log: log scan records requiring two reads";
	stats->log_write_lsn.desc =
	    "log: log server thread advances write LSN";
	stats->log_sync.desc = "log: log sync operations";
	stats->log_sync_dir.desc = "log: log sync_dir operations";
	stats->log_writes.desc = "log: log write operations";
	stats->log_slot_consolidated.desc = "log: logging bytes consolidated";
	stats->log_max_filesize.desc = "log: maximum log file size";
	stats->log_prealloc_max.desc =
	    "log: number of pre-allocated log files to create";
	stats->log_prealloc_files.desc =
	    "log: pre-allocated log files prepared";
	stats->log_prealloc_used.desc = "log: pre-allocated log files used";
	stats->log_slot_toobig.desc = "log: record size exceeded maximum";
	stats->log_scan_records.desc = "log: records processed by log scan";
	stats->log_slot_switch_fails.desc =
	    "log: slots selected for switching that were unavailable";
	stats->log_compress_mem.desc =
	    "log: total in-memory size of compressed records";
	stats->log_buffer_size.desc = "log: total log buffer size";
	stats->log_compress_len.desc = "log: total size of compressed records";
	stats->log_close_yields.desc =
	    "log: yields waiting for previous log file close";
	stats->lsm_work_queue_app.desc =
	    "LSM: application work units currently queued";
	stats->lsm_work_queue_manager.desc =
	    "LSM: merge work units currently queued";
	stats->lsm_rows_merged.desc = "LSM: rows merged in an LSM tree";
	stats->lsm_checkpoint_throttle.desc =
	    "LSM: sleep for LSM checkpoint throttle";
	stats->lsm_merge_throttle.desc = "LSM: sleep for LSM merge throttle";
	stats->lsm_work_queue_switch.desc =
	    "LSM: switch work units currently queued";
	stats->lsm_work_units_discarded.desc =
	    "LSM: tree maintenance operations discarded";
	stats->lsm_work_units_done.desc =
	    "LSM: tree maintenance operations executed";
	stats->lsm_work_units_created.desc =
	    "LSM: tree maintenance operations scheduled";
	stats->lsm_work_queue_max.desc = "LSM: tree queue hit maximum";
	stats->rec_pages.desc = "reconciliation: page reconciliation calls";
	stats->rec_pages_eviction.desc =
	    "reconciliation: page reconciliation calls for eviction";
	stats->rec_split_stashed_bytes.desc =
	    "reconciliation: split bytes currently awaiting free";
	stats->rec_split_stashed_objects.desc =
	    "reconciliation: split objects currently awaiting free";
	stats->session_cursor_open.desc = "session: open cursor count";
	stats->session_open.desc = "session: open session count";
	stats->page_busy_blocked.desc =
	    "thread-yield: page acquire busy blocked";
	stats->page_forcible_evict_blocked.desc =
	    "thread-yield: page acquire eviction blocked";
	stats->page_locked_blocked.desc =
	    "thread-yield: page acquire locked blocked";
	stats->page_read_blocked.desc =
	    "thread-yield: page acquire read blocked";
	stats->page_sleep.desc =
	    "thread-yield: page acquire time sleeping (usecs)";
	stats->txn_begin.desc = "transaction: transaction begins";
	stats->txn_checkpoint_running.desc =
	    "transaction: transaction checkpoint currently running";
	stats->txn_checkpoint_generation.desc =
	    "transaction: transaction checkpoint generation";
	stats->txn_checkpoint_time_max.desc =
	    "transaction: transaction checkpoint max time (msecs)";
	stats->txn_checkpoint_time_min.desc =
	    "transaction: transaction checkpoint min time (msecs)";
	stats->txn_checkpoint_time_recent.desc =
	    "transaction: transaction checkpoint most recent time (msecs)";
	stats->txn_checkpoint_time_total.desc =
	    "transaction: transaction checkpoint total time (msecs)";
	stats->txn_checkpoint.desc = "transaction: transaction checkpoints";
	stats->txn_fail_cache.desc =
	    "transaction: transaction failures due to cache overflow";
	stats->txn_pinned_range.desc =
	    "transaction: transaction range of IDs currently pinned";
	stats->txn_pinned_checkpoint_range.desc =
	    "transaction: transaction range of IDs currently pinned by a checkpoint";
	stats->txn_sync.desc = "transaction: transaction sync calls";
	stats->txn_commit.desc = "transaction: transactions committed";
	stats->txn_rollback.desc = "transaction: transactions rolled back";
}

void
__wt_stat_refresh_connection_stats(void *stats_arg)
{
	WT_CONNECTION_STATS *stats;

	stats = (WT_CONNECTION_STATS *)stats_arg;
	stats->async_cur_queue.v = 0;
	stats->async_alloc_race.v = 0;
	stats->async_flush.v = 0;
	stats->async_alloc_view.v = 0;
	stats->async_full.v = 0;
	stats->async_nowork.v = 0;
	stats->async_op_alloc.v = 0;
	stats->async_op_compact.v = 0;
	stats->async_op_insert.v = 0;
	stats->async_op_remove.v = 0;
	stats->async_op_search.v = 0;
	stats->async_op_update.v = 0;
	stats->block_preload.v = 0;
	stats->block_read.v = 0;
	stats->block_write.v = 0;
	stats->block_byte_read.v = 0;
	stats->block_byte_write.v = 0;
	stats->block_map_read.v = 0;
	stats->block_byte_map_read.v = 0;
	stats->cache_bytes_read.v = 0;
	stats->cache_bytes_write.v = 0;
	stats->cache_eviction_checkpoint.v = 0;
	stats->cache_eviction_queue_empty.v = 0;
	stats->cache_eviction_queue_not_empty.v = 0;
	stats->cache_eviction_server_evicting.v = 0;
	stats->cache_eviction_server_not_evicting.v = 0;
	stats->cache_eviction_slow.v = 0;
	stats->cache_eviction_worker_evicting.v = 0;
	stats->cache_eviction_force_fail.v = 0;
	stats->cache_eviction_hazard.v = 0;
	stats->cache_inmem_split.v = 0;
	stats->cache_eviction_internal.v = 0;
	stats->cache_eviction_dirty.v = 0;
	stats->cache_eviction_deepen.v = 0;
	stats->cache_eviction_force.v = 0;
	stats->cache_eviction_force_delete.v = 0;
	stats->cache_eviction_app.v = 0;
	stats->cache_read.v = 0;
	stats->cache_eviction_fail.v = 0;
	stats->cache_eviction_split.v = 0;
	stats->cache_eviction_walk.v = 0;
	stats->cache_write.v = 0;
	stats->cache_eviction_clean.v = 0;
	stats->memory_allocation.v = 0;
	stats->memory_free.v = 0;
	stats->memory_grow.v = 0;
	stats->cond_wait.v = 0;
	stats->rwlock_read.v = 0;
	stats->rwlock_write.v = 0;
	stats->read_io.v = 0;
	stats->write_io.v = 0;
	stats->cursor_create.v = 0;
	stats->cursor_insert.v = 0;
	stats->cursor_next.v = 0;
	stats->cursor_prev.v = 0;
	stats->cursor_remove.v = 0;
	stats->cursor_reset.v = 0;
	stats->cursor_search.v = 0;
	stats->cursor_search_near.v = 0;
	stats->cursor_update.v = 0;
	stats->dh_conn_ref.v = 0;
	stats->dh_conn_handles.v = 0;
	stats->dh_conn_sweeps.v = 0;
	stats->dh_conn_tod.v = 0;
	stats->dh_session_handles.v = 0;
	stats->dh_session_sweeps.v = 0;
	stats->log_slot_closes.v = 0;
	stats->log_slot_races.v = 0;
	stats->log_slot_transitions.v = 0;
	stats->log_slot_joins.v = 0;
	stats->log_slot_toosmall.v = 0;
	stats->log_bytes_payload.v = 0;
	stats->log_bytes_written.v = 0;
	stats->log_compress_writes.v = 0;
	stats->log_compress_write_fails.v = 0;
	stats->log_compress_small.v = 0;
	stats->log_release_write_lsn.v = 0;
	stats->log_scans.v = 0;
	stats->log_scan_rereads.v = 0;
	stats->log_write_lsn.v = 0;
	stats->log_sync.v = 0;
	stats->log_sync_dir.v = 0;
	stats->log_writes.v = 0;
	stats->log_slot_consolidated.v = 0;
	stats->log_prealloc_files.v = 0;
	stats->log_prealloc_used.v = 0;
	stats->log_slot_toobig.v = 0;
	stats->log_scan_records.v = 0;
	stats->log_slot_switch_fails.v = 0;
	stats->log_compress_mem.v = 0;
	stats->log_compress_len.v = 0;
	stats->log_close_yields.v = 0;
	stats->lsm_rows_merged.v = 0;
	stats->lsm_checkpoint_throttle.v = 0;
	stats->lsm_merge_throttle.v = 0;
	stats->lsm_work_units_discarded.v = 0;
	stats->lsm_work_units_done.v = 0;
	stats->lsm_work_units_created.v = 0;
	stats->lsm_work_queue_max.v = 0;
	stats->rec_pages.v = 0;
	stats->rec_pages_eviction.v = 0;
	stats->page_busy_blocked.v = 0;
	stats->page_forcible_evict_blocked.v = 0;
	stats->page_locked_blocked.v = 0;
	stats->page_read_blocked.v = 0;
	stats->page_sleep.v = 0;
	stats->txn_begin.v = 0;
	stats->txn_checkpoint.v = 0;
	stats->txn_fail_cache.v = 0;
	stats->txn_sync.v = 0;
	stats->txn_commit.v = 0;
	stats->txn_rollback.v = 0;
}
