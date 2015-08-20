/* DO NOT EDIT: automatically built by dist/stat.py. */

#include "wt_internal.h"

static const char * const __stats_dsrc_desc[] = {
	"block-manager: file allocation unit size",
	"block-manager: blocks allocated",
	"block-manager: checkpoint size",
	"block-manager: allocations requiring file extension",
	"block-manager: blocks freed",
	"block-manager: file magic number",
	"block-manager: file major version number",
	"block-manager: minor version number",
	"block-manager: file bytes available for reuse",
	"block-manager: file size in bytes",
	"LSM: bloom filters in the LSM tree",
	"LSM: bloom filter false positives",
	"LSM: bloom filter hits",
	"LSM: bloom filter misses",
	"LSM: bloom filter pages evicted from cache",
	"LSM: bloom filter pages read into cache",
	"LSM: total size of bloom filters",
	"btree: btree checkpoint generation",
	"btree: column-store variable-size deleted values",
	"btree: column-store fixed-size leaf pages",
	"btree: column-store internal pages",
	"btree: column-store variable-size leaf pages",
	"btree: pages rewritten by compaction",
	"btree: number of key/value pairs",
	"btree: fixed-record size",
	"btree: maximum tree depth",
	"btree: maximum internal page key size",
	"btree: maximum internal page size",
	"btree: maximum leaf page key size",
	"btree: maximum leaf page size",
	"btree: maximum leaf page value size",
	"btree: overflow pages",
	"btree: row-store internal pages",
	"btree: row-store leaf pages",
	"cache: bytes read into cache",
	"cache: bytes written from cache",
	"cache: checkpoint blocked page eviction",
	"cache: unmodified pages evicted",
	"cache: page split during eviction deepened the tree",
	"cache: modified pages evicted",
	"cache: data source pages selected for eviction unable to be evicted",
	"cache: hazard pointer blocked page eviction",
	"cache: internal pages evicted",
	"cache: pages split during eviction",
	"cache: in-memory page splits",
	"cache: overflow values cached in memory",
	"cache: pages read into cache",
	"cache: pages read into cache requiring lookaside entries",
	"cache: overflow pages read into cache",
	"cache: pages written from cache",
	"compression: raw compression call failed, no additional data available",
	"compression: raw compression call failed, additional data available",
	"compression: raw compression call succeeded",
	"compression: compressed pages read",
	"compression: compressed pages written",
	"compression: page written failed to compress",
	"compression: page written was too small to compress",
	"cursor: create calls",
	"cursor: insert calls",
	"cursor: bulk-loaded cursor-insert calls",
	"cursor: cursor-insert key and value bytes inserted",
	"cursor: next calls",
	"cursor: prev calls",
	"cursor: remove calls",
	"cursor: cursor-remove key bytes removed",
	"cursor: reset calls",
	"cursor: restarted searches",
	"cursor: search calls",
	"cursor: search near calls",
	"cursor: update calls",
	"cursor: cursor-update value bytes updated",
	"LSM: sleep for LSM checkpoint throttle",
	"LSM: chunks in the LSM tree",
	"LSM: highest merge generation in the LSM tree",
	"LSM: queries that could have benefited from a Bloom filter that did not exist",
	"LSM: sleep for LSM merge throttle",
	"reconciliation: dictionary matches",
	"reconciliation: internal page multi-block writes",
	"reconciliation: leaf page multi-block writes",
	"reconciliation: maximum blocks required for a page",
	"reconciliation: internal-page overflow keys",
	"reconciliation: leaf-page overflow keys",
	"reconciliation: overflow values written",
	"reconciliation: pages deleted",
	"reconciliation: page checksum matches",
	"reconciliation: page reconciliation calls",
	"reconciliation: page reconciliation calls for eviction",
	"reconciliation: page reconciliation block requires lookaside records",
	"reconciliation: leaf page key bytes discarded using prefix compression",
	"reconciliation: internal page key bytes discarded using suffix compression",
	"session: object compaction",
	"session: open cursor count",
	"transaction: update conflicts",
};

const char *
__wt_stat_dsrc_desc(int slot)
{
	return (__stats_dsrc_desc[slot]);
}

void
__wt_stat_dsrc_init_single(WT_DSRC_STATS *stats)
{
	memset(stats, 0, sizeof(*stats));
}

void
__wt_stat_dsrc_init(WT_DATA_HANDLE *handle)
{
	int i;

	for (i = 0; i < WT_COUNTER_SLOTS; ++i) {
		handle->stats[i] = &handle->stat_array[i];
		__wt_stat_dsrc_init_single(handle->stats[i]);
	}
}

void
__wt_stat_dsrc_clear_single(WT_DSRC_STATS *stats)
{
	stats->block_extension = 0;
	stats->block_alloc = 0;
	stats->block_free = 0;
	stats->block_checkpoint_size = 0;
	stats->allocation_size = 0;
	stats->block_reuse_bytes = 0;
	stats->block_magic = 0;
	stats->block_major = 0;
	stats->block_size = 0;
	stats->block_minor = 0;
		/* not clearing btree_checkpoint_generation */
	stats->btree_column_fix = 0;
	stats->btree_column_internal = 0;
	stats->btree_column_deleted = 0;
	stats->btree_column_variable = 0;
	stats->btree_fixed_len = 0;
	stats->btree_maxintlkey = 0;
	stats->btree_maxintlpage = 0;
	stats->btree_maxleafkey = 0;
	stats->btree_maxleafpage = 0;
	stats->btree_maxleafvalue = 0;
	stats->btree_maximum_depth = 0;
	stats->btree_entries = 0;
	stats->btree_overflow = 0;
	stats->btree_compact_rewrite = 0;
	stats->btree_row_internal = 0;
	stats->btree_row_leaf = 0;
	stats->cache_bytes_read = 0;
	stats->cache_bytes_write = 0;
	stats->cache_eviction_checkpoint = 0;
	stats->cache_eviction_fail = 0;
	stats->cache_eviction_hazard = 0;
	stats->cache_inmem_split = 0;
	stats->cache_eviction_internal = 0;
	stats->cache_eviction_dirty = 0;
	stats->cache_read_overflow = 0;
	stats->cache_overflow_value = 0;
	stats->cache_eviction_deepen = 0;
	stats->cache_read = 0;
	stats->cache_read_lookaside = 0;
	stats->cache_eviction_split = 0;
	stats->cache_write = 0;
	stats->cache_eviction_clean = 0;
	stats->compress_read = 0;
	stats->compress_write = 0;
	stats->compress_write_fail = 0;
	stats->compress_write_too_small = 0;
	stats->compress_raw_fail_temporary = 0;
	stats->compress_raw_fail = 0;
	stats->compress_raw_ok = 0;
	stats->cursor_insert_bulk = 0;
	stats->cursor_create = 0;
	stats->cursor_insert_bytes = 0;
	stats->cursor_remove_bytes = 0;
	stats->cursor_update_bytes = 0;
	stats->cursor_insert = 0;
	stats->cursor_next = 0;
	stats->cursor_prev = 0;
	stats->cursor_remove = 0;
	stats->cursor_reset = 0;
	stats->cursor_restart = 0;
	stats->cursor_search = 0;
	stats->cursor_search_near = 0;
	stats->cursor_update = 0;
	stats->bloom_false_positive = 0;
	stats->bloom_hit = 0;
	stats->bloom_miss = 0;
	stats->bloom_page_evict = 0;
	stats->bloom_page_read = 0;
	stats->bloom_count = 0;
	stats->lsm_chunk_count = 0;
	stats->lsm_generation_max = 0;
	stats->lsm_lookup_no_bloom = 0;
	stats->lsm_checkpoint_throttle = 0;
	stats->lsm_merge_throttle = 0;
	stats->bloom_size = 0;
	stats->rec_dictionary = 0;
	stats->rec_suffix_compression = 0;
	stats->rec_multiblock_internal = 0;
	stats->rec_overflow_key_internal = 0;
	stats->rec_prefix_compression = 0;
	stats->rec_multiblock_leaf = 0;
	stats->rec_overflow_key_leaf = 0;
	stats->rec_multiblock_max = 0;
	stats->rec_overflow_value = 0;
	stats->rec_page_match = 0;
	stats->rec_pages_lookaside = 0;
	stats->rec_pages = 0;
	stats->rec_pages_eviction = 0;
	stats->rec_page_delete = 0;
	stats->session_compact = 0;
		/* not clearing session_cursor_open */
	stats->txn_update_conflict = 0;
}

void
__wt_stat_dsrc_clear_all(WT_DSRC_STATS **stats)
{
	u_int i;

	for (i = 0; i < WT_COUNTER_SLOTS; ++i)
		__wt_stat_dsrc_clear_single(stats[i]);
}

void
__wt_stat_dsrc_aggregate_single(
    WT_DSRC_STATS *from, WT_DSRC_STATS *to)
{
	to->block_extension +=
	    from->block_extension;
	to->block_alloc +=
	    from->block_alloc;
	to->block_free +=
	    from->block_free;
	to->block_checkpoint_size +=
	    from->block_checkpoint_size;
		/* not aggregating allocation_size */
	to->block_reuse_bytes +=
	    from->block_reuse_bytes;
		/* not aggregating block_magic */
		/* not aggregating block_major */
	to->block_size +=
	    from->block_size;
		/* not aggregating block_minor */
	to->btree_checkpoint_generation +=
	    from->btree_checkpoint_generation;
	to->btree_column_fix +=
	    from->btree_column_fix;
	to->btree_column_internal +=
	    from->btree_column_internal;
	to->btree_column_deleted +=
	    from->btree_column_deleted;
	to->btree_column_variable +=
	    from->btree_column_variable;
		/* not aggregating btree_fixed_len */
	if (from->btree_maxintlkey > to->btree_maxintlkey)
		to->btree_maxintlkey = from->btree_maxintlkey;
	if (from->btree_maxintlpage > to->btree_maxintlpage)
		to->btree_maxintlpage = from->btree_maxintlpage;
	if (from->btree_maxleafkey > to->btree_maxleafkey)
		to->btree_maxleafkey = from->btree_maxleafkey;
	if (from->btree_maxleafpage > to->btree_maxleafpage)
		to->btree_maxleafpage = from->btree_maxleafpage;
	if (from->btree_maxleafvalue > to->btree_maxleafvalue)
		to->btree_maxleafvalue = from->btree_maxleafvalue;
	if (from->btree_maximum_depth > to->btree_maximum_depth)
		to->btree_maximum_depth = from->btree_maximum_depth;
	to->btree_entries +=
	    from->btree_entries;
	to->btree_overflow +=
	    from->btree_overflow;
	to->btree_compact_rewrite +=
	    from->btree_compact_rewrite;
	to->btree_row_internal +=
	    from->btree_row_internal;
	to->btree_row_leaf +=
	    from->btree_row_leaf;
	to->cache_bytes_read +=
	    from->cache_bytes_read;
	to->cache_bytes_write +=
	    from->cache_bytes_write;
	to->cache_eviction_checkpoint +=
	    from->cache_eviction_checkpoint;
	to->cache_eviction_fail +=
	    from->cache_eviction_fail;
	to->cache_eviction_hazard +=
	    from->cache_eviction_hazard;
	to->cache_inmem_split +=
	    from->cache_inmem_split;
	to->cache_eviction_internal +=
	    from->cache_eviction_internal;
	to->cache_eviction_dirty +=
	    from->cache_eviction_dirty;
	to->cache_read_overflow +=
	    from->cache_read_overflow;
	to->cache_overflow_value +=
	    from->cache_overflow_value;
	to->cache_eviction_deepen +=
	    from->cache_eviction_deepen;
	to->cache_read +=
	    from->cache_read;
	to->cache_read_lookaside +=
	    from->cache_read_lookaside;
	to->cache_eviction_split +=
	    from->cache_eviction_split;
	to->cache_write +=
	    from->cache_write;
	to->cache_eviction_clean +=
	    from->cache_eviction_clean;
	to->compress_read +=
	    from->compress_read;
	to->compress_write +=
	    from->compress_write;
	to->compress_write_fail +=
	    from->compress_write_fail;
	to->compress_write_too_small +=
	    from->compress_write_too_small;
	to->compress_raw_fail_temporary +=
	    from->compress_raw_fail_temporary;
	to->compress_raw_fail +=
	    from->compress_raw_fail;
	to->compress_raw_ok +=
	    from->compress_raw_ok;
	to->cursor_insert_bulk +=
	    from->cursor_insert_bulk;
	to->cursor_create +=
	    from->cursor_create;
	to->cursor_insert_bytes +=
	    from->cursor_insert_bytes;
	to->cursor_remove_bytes +=
	    from->cursor_remove_bytes;
	to->cursor_update_bytes +=
	    from->cursor_update_bytes;
	to->cursor_insert +=
	    from->cursor_insert;
	to->cursor_next +=
	    from->cursor_next;
	to->cursor_prev +=
	    from->cursor_prev;
	to->cursor_remove +=
	    from->cursor_remove;
	to->cursor_reset +=
	    from->cursor_reset;
	to->cursor_restart +=
	    from->cursor_restart;
	to->cursor_search +=
	    from->cursor_search;
	to->cursor_search_near +=
	    from->cursor_search_near;
	to->cursor_update +=
	    from->cursor_update;
	to->bloom_false_positive +=
	    from->bloom_false_positive;
	to->bloom_hit +=
	    from->bloom_hit;
	to->bloom_miss +=
	    from->bloom_miss;
	to->bloom_page_evict +=
	    from->bloom_page_evict;
	to->bloom_page_read +=
	    from->bloom_page_read;
	to->bloom_count +=
	    from->bloom_count;
	to->lsm_chunk_count +=
	    from->lsm_chunk_count;
	if (from->lsm_generation_max > to->lsm_generation_max)
		to->lsm_generation_max = from->lsm_generation_max;
	to->lsm_lookup_no_bloom +=
	    from->lsm_lookup_no_bloom;
	to->lsm_checkpoint_throttle +=
	    from->lsm_checkpoint_throttle;
	to->lsm_merge_throttle +=
	    from->lsm_merge_throttle;
	to->bloom_size +=
	    from->bloom_size;
	to->rec_dictionary +=
	    from->rec_dictionary;
	to->rec_suffix_compression +=
	    from->rec_suffix_compression;
	to->rec_multiblock_internal +=
	    from->rec_multiblock_internal;
	to->rec_overflow_key_internal +=
	    from->rec_overflow_key_internal;
	to->rec_prefix_compression +=
	    from->rec_prefix_compression;
	to->rec_multiblock_leaf +=
	    from->rec_multiblock_leaf;
	to->rec_overflow_key_leaf +=
	    from->rec_overflow_key_leaf;
	if (from->rec_multiblock_max > to->rec_multiblock_max)
		to->rec_multiblock_max = from->rec_multiblock_max;
	to->rec_overflow_value +=
	    from->rec_overflow_value;
	to->rec_page_match +=
	    from->rec_page_match;
	to->rec_pages_lookaside +=
	    from->rec_pages_lookaside;
	to->rec_pages +=
	    from->rec_pages;
	to->rec_pages_eviction +=
	    from->rec_pages_eviction;
	to->rec_page_delete +=
	    from->rec_page_delete;
	to->session_compact +=
	    from->session_compact;
	to->session_cursor_open +=
	    from->session_cursor_open;
	to->txn_update_conflict +=
	    from->txn_update_conflict;
}

void
__wt_stat_dsrc_aggregate(
    WT_DSRC_STATS **from, WT_DSRC_STATS *to)
{
	uint64_t v;

	to->block_extension +=
	    (int64_t)WT_STAT_READ(from, block_extension);
	to->block_alloc +=
	    (int64_t)WT_STAT_READ(from, block_alloc);
	to->block_free +=
	    (int64_t)WT_STAT_READ(from, block_free);
	to->block_checkpoint_size +=
	    (int64_t)WT_STAT_READ(from, block_checkpoint_size);
		/* not aggregating allocation_size */
	to->block_reuse_bytes +=
	    (int64_t)WT_STAT_READ(from, block_reuse_bytes);
		/* not aggregating block_magic */
		/* not aggregating block_major */
	to->block_size +=
	    (int64_t)WT_STAT_READ(from, block_size);
		/* not aggregating block_minor */
	to->btree_checkpoint_generation +=
	    (int64_t)WT_STAT_READ(from, btree_checkpoint_generation);
	to->btree_column_fix +=
	    (int64_t)WT_STAT_READ(from, btree_column_fix);
	to->btree_column_internal +=
	    (int64_t)WT_STAT_READ(from, btree_column_internal);
	to->btree_column_deleted +=
	    (int64_t)WT_STAT_READ(from, btree_column_deleted);
	to->btree_column_variable +=
	    (int64_t)WT_STAT_READ(from, btree_column_variable);
		/* not aggregating btree_fixed_len */
	if ((v = WT_STAT_READ(from, btree_maxintlkey)) >
	    (uint64_t)to->btree_maxintlkey)
		to->btree_maxintlkey = (int64_t)v;
	if ((v = WT_STAT_READ(from, btree_maxintlpage)) >
	    (uint64_t)to->btree_maxintlpage)
		to->btree_maxintlpage = (int64_t)v;
	if ((v = WT_STAT_READ(from, btree_maxleafkey)) >
	    (uint64_t)to->btree_maxleafkey)
		to->btree_maxleafkey = (int64_t)v;
	if ((v = WT_STAT_READ(from, btree_maxleafpage)) >
	    (uint64_t)to->btree_maxleafpage)
		to->btree_maxleafpage = (int64_t)v;
	if ((v = WT_STAT_READ(from, btree_maxleafvalue)) >
	    (uint64_t)to->btree_maxleafvalue)
		to->btree_maxleafvalue = (int64_t)v;
	if ((v = WT_STAT_READ(from, btree_maximum_depth)) >
	    (uint64_t)to->btree_maximum_depth)
		to->btree_maximum_depth = (int64_t)v;
	to->btree_entries +=
	    (int64_t)WT_STAT_READ(from, btree_entries);
	to->btree_overflow +=
	    (int64_t)WT_STAT_READ(from, btree_overflow);
	to->btree_compact_rewrite +=
	    (int64_t)WT_STAT_READ(from, btree_compact_rewrite);
	to->btree_row_internal +=
	    (int64_t)WT_STAT_READ(from, btree_row_internal);
	to->btree_row_leaf +=
	    (int64_t)WT_STAT_READ(from, btree_row_leaf);
	to->cache_bytes_read +=
	    (int64_t)WT_STAT_READ(from, cache_bytes_read);
	to->cache_bytes_write +=
	    (int64_t)WT_STAT_READ(from, cache_bytes_write);
	to->cache_eviction_checkpoint +=
	    (int64_t)WT_STAT_READ(from, cache_eviction_checkpoint);
	to->cache_eviction_fail +=
	    (int64_t)WT_STAT_READ(from, cache_eviction_fail);
	to->cache_eviction_hazard +=
	    (int64_t)WT_STAT_READ(from, cache_eviction_hazard);
	to->cache_inmem_split +=
	    (int64_t)WT_STAT_READ(from, cache_inmem_split);
	to->cache_eviction_internal +=
	    (int64_t)WT_STAT_READ(from, cache_eviction_internal);
	to->cache_eviction_dirty +=
	    (int64_t)WT_STAT_READ(from, cache_eviction_dirty);
	to->cache_read_overflow +=
	    (int64_t)WT_STAT_READ(from, cache_read_overflow);
	to->cache_overflow_value +=
	    (int64_t)WT_STAT_READ(from, cache_overflow_value);
	to->cache_eviction_deepen +=
	    (int64_t)WT_STAT_READ(from, cache_eviction_deepen);
	to->cache_read +=
	    (int64_t)WT_STAT_READ(from, cache_read);
	to->cache_read_lookaside +=
	    (int64_t)WT_STAT_READ(from, cache_read_lookaside);
	to->cache_eviction_split +=
	    (int64_t)WT_STAT_READ(from, cache_eviction_split);
	to->cache_write +=
	    (int64_t)WT_STAT_READ(from, cache_write);
	to->cache_eviction_clean +=
	    (int64_t)WT_STAT_READ(from, cache_eviction_clean);
	to->compress_read +=
	    (int64_t)WT_STAT_READ(from, compress_read);
	to->compress_write +=
	    (int64_t)WT_STAT_READ(from, compress_write);
	to->compress_write_fail +=
	    (int64_t)WT_STAT_READ(from, compress_write_fail);
	to->compress_write_too_small +=
	    (int64_t)WT_STAT_READ(from, compress_write_too_small);
	to->compress_raw_fail_temporary +=
	    (int64_t)WT_STAT_READ(from, compress_raw_fail_temporary);
	to->compress_raw_fail +=
	    (int64_t)WT_STAT_READ(from, compress_raw_fail);
	to->compress_raw_ok +=
	    (int64_t)WT_STAT_READ(from, compress_raw_ok);
	to->cursor_insert_bulk +=
	    (int64_t)WT_STAT_READ(from, cursor_insert_bulk);
	to->cursor_create +=
	    (int64_t)WT_STAT_READ(from, cursor_create);
	to->cursor_insert_bytes +=
	    (int64_t)WT_STAT_READ(from, cursor_insert_bytes);
	to->cursor_remove_bytes +=
	    (int64_t)WT_STAT_READ(from, cursor_remove_bytes);
	to->cursor_update_bytes +=
	    (int64_t)WT_STAT_READ(from, cursor_update_bytes);
	to->cursor_insert +=
	    (int64_t)WT_STAT_READ(from, cursor_insert);
	to->cursor_next +=
	    (int64_t)WT_STAT_READ(from, cursor_next);
	to->cursor_prev +=
	    (int64_t)WT_STAT_READ(from, cursor_prev);
	to->cursor_remove +=
	    (int64_t)WT_STAT_READ(from, cursor_remove);
	to->cursor_reset +=
	    (int64_t)WT_STAT_READ(from, cursor_reset);
	to->cursor_restart +=
	    (int64_t)WT_STAT_READ(from, cursor_restart);
	to->cursor_search +=
	    (int64_t)WT_STAT_READ(from, cursor_search);
	to->cursor_search_near +=
	    (int64_t)WT_STAT_READ(from, cursor_search_near);
	to->cursor_update +=
	    (int64_t)WT_STAT_READ(from, cursor_update);
	to->bloom_false_positive +=
	    (int64_t)WT_STAT_READ(from, bloom_false_positive);
	to->bloom_hit +=
	    (int64_t)WT_STAT_READ(from, bloom_hit);
	to->bloom_miss +=
	    (int64_t)WT_STAT_READ(from, bloom_miss);
	to->bloom_page_evict +=
	    (int64_t)WT_STAT_READ(from, bloom_page_evict);
	to->bloom_page_read +=
	    (int64_t)WT_STAT_READ(from, bloom_page_read);
	to->bloom_count +=
	    (int64_t)WT_STAT_READ(from, bloom_count);
	to->lsm_chunk_count +=
	    (int64_t)WT_STAT_READ(from, lsm_chunk_count);
	if ((v = WT_STAT_READ(from, lsm_generation_max)) >
	    (uint64_t)to->lsm_generation_max)
		to->lsm_generation_max = (int64_t)v;
	to->lsm_lookup_no_bloom +=
	    (int64_t)WT_STAT_READ(from, lsm_lookup_no_bloom);
	to->lsm_checkpoint_throttle +=
	    (int64_t)WT_STAT_READ(from, lsm_checkpoint_throttle);
	to->lsm_merge_throttle +=
	    (int64_t)WT_STAT_READ(from, lsm_merge_throttle);
	to->bloom_size +=
	    (int64_t)WT_STAT_READ(from, bloom_size);
	to->rec_dictionary +=
	    (int64_t)WT_STAT_READ(from, rec_dictionary);
	to->rec_suffix_compression +=
	    (int64_t)WT_STAT_READ(from, rec_suffix_compression);
	to->rec_multiblock_internal +=
	    (int64_t)WT_STAT_READ(from, rec_multiblock_internal);
	to->rec_overflow_key_internal +=
	    (int64_t)WT_STAT_READ(from, rec_overflow_key_internal);
	to->rec_prefix_compression +=
	    (int64_t)WT_STAT_READ(from, rec_prefix_compression);
	to->rec_multiblock_leaf +=
	    (int64_t)WT_STAT_READ(from, rec_multiblock_leaf);
	to->rec_overflow_key_leaf +=
	    (int64_t)WT_STAT_READ(from, rec_overflow_key_leaf);
	if ((v = WT_STAT_READ(from, rec_multiblock_max)) >
	    (uint64_t)to->rec_multiblock_max)
		to->rec_multiblock_max = (int64_t)v;
	to->rec_overflow_value +=
	    (int64_t)WT_STAT_READ(from, rec_overflow_value);
	to->rec_page_match +=
	    (int64_t)WT_STAT_READ(from, rec_page_match);
	to->rec_pages_lookaside +=
	    (int64_t)WT_STAT_READ(from, rec_pages_lookaside);
	to->rec_pages +=
	    (int64_t)WT_STAT_READ(from, rec_pages);
	to->rec_pages_eviction +=
	    (int64_t)WT_STAT_READ(from, rec_pages_eviction);
	to->rec_page_delete +=
	    (int64_t)WT_STAT_READ(from, rec_page_delete);
	to->session_compact +=
	    (int64_t)WT_STAT_READ(from, session_compact);
	to->session_cursor_open +=
	    (int64_t)WT_STAT_READ(from, session_cursor_open);
	to->txn_update_conflict +=
	    (int64_t)WT_STAT_READ(from, txn_update_conflict);
}

static const char * const __stats_connection_desc[] = {
	"async: number of allocation state races",
	"async: number of operation slots viewed for allocation",
	"async: current work queue length",
	"async: number of flush calls",
	"async: number of times operation allocation failed",
	"async: maximum work queue length",
	"async: number of times worker found no work",
	"async: total allocations",
	"async: total compact calls",
	"async: total insert calls",
	"async: total remove calls",
	"async: total search calls",
	"async: total update calls",
	"block-manager: mapped bytes read",
	"block-manager: bytes read",
	"block-manager: bytes written",
	"block-manager: mapped blocks read",
	"block-manager: blocks pre-loaded",
	"block-manager: blocks read",
	"block-manager: blocks written",
	"cache: tracked dirty bytes in the cache",
	"cache: tracked bytes belonging to internal pages in the cache",
	"cache: bytes currently in the cache",
	"cache: tracked bytes belonging to leaf pages in the cache",
	"cache: maximum bytes configured",
	"cache: tracked bytes belonging to overflow pages in the cache",
	"cache: bytes read into cache",
	"cache: bytes written from cache",
	"cache: pages evicted by application threads",
	"cache: checkpoint blocked page eviction",
	"cache: unmodified pages evicted",
	"cache: page split during eviction deepened the tree",
	"cache: modified pages evicted",
	"cache: pages selected for eviction unable to be evicted",
	"cache: pages evicted because they exceeded the in-memory maximum",
	"cache: pages evicted because they had chains of deleted items",
	"cache: failed eviction of pages that exceeded the in-memory maximum",
	"cache: hazard pointer blocked page eviction",
	"cache: internal pages evicted",
	"cache: maximum page size at eviction",
	"cache: eviction server candidate queue empty when topping up",
	"cache: eviction server candidate queue not empty when topping up",
	"cache: eviction server evicting pages",
	"cache: eviction server populating queue, but not evicting pages",
	"cache: eviction server unable to reach eviction goal",
	"cache: pages split during eviction",
	"cache: pages walked for eviction",
	"cache: eviction worker thread evicting pages",
	"cache: in-memory page splits",
	"cache: percentage overhead",
	"cache: tracked dirty pages in the cache",
	"cache: pages currently held in the cache",
	"cache: pages read into cache",
	"cache: pages read into cache requiring lookaside entries",
	"cache: pages written from cache",
	"connection: pthread mutex condition wait calls",
	"cursor: cursor create calls",
	"cursor: cursor insert calls",
	"cursor: cursor next calls",
	"cursor: cursor prev calls",
	"cursor: cursor remove calls",
	"cursor: cursor reset calls",
	"cursor: cursor restarted searches",
	"cursor: cursor search calls",
	"cursor: cursor search near calls",
	"cursor: cursor update calls",
	"data-handle: connection data handles currently active",
	"data-handle: session dhandles swept",
	"data-handle: session sweep attempts",
	"data-handle: connection sweep dhandles closed",
	"data-handle: connection sweep candidate became referenced",
	"data-handle: connection sweep dhandles removed from hash list",
	"data-handle: connection sweep time-of-death sets",
	"data-handle: connection sweeps",
	"connection: files currently open",
	"log: total log buffer size",
	"log: log bytes of payload data",
	"log: log bytes written",
	"log: yields waiting for previous log file close",
	"log: total size of compressed records",
	"log: total in-memory size of compressed records",
	"log: log records too small to compress",
	"log: log records not compressed",
	"log: log records compressed",
	"log: maximum log file size",
	"log: pre-allocated log files prepared",
	"log: number of pre-allocated log files to create",
	"log: pre-allocated log files used",
	"log: log release advances write LSN",
	"log: records processed by log scan",
	"log: log scan records requiring two reads",
	"log: log scan operations",
	"log: consolidated slot closures",
	"log: written slots coalesced",
	"log: logging bytes consolidated",
	"log: consolidated slot joins",
	"log: consolidated slot join races",
	"log: record size exceeded maximum",
	"log: failed to find a slot large enough for record",
	"log: consolidated slot join transitions",
	"log: log sync operations",
	"log: log sync_dir operations",
	"log: log server thread advances write LSN",
	"log: log write operations",
	"lookaside: lookaside table insert calls",
	"lookaside: lookaside table cursor-insert key and value bytes inserted",
	"lookaside: lookaside table remove calls",
	"LSM: sleep for LSM checkpoint throttle",
	"LSM: sleep for LSM merge throttle",
	"LSM: rows merged in an LSM tree",
	"LSM: application work units currently queued",
	"LSM: merge work units currently queued",
	"LSM: tree queue hit maximum",
	"LSM: switch work units currently queued",
	"LSM: tree maintenance operations scheduled",
	"LSM: tree maintenance operations discarded",
	"LSM: tree maintenance operations executed",
	"connection: memory allocations",
	"connection: memory frees",
	"connection: memory re-allocations",
	"thread-yield: page acquire busy blocked",
	"thread-yield: page acquire eviction blocked",
	"thread-yield: page acquire locked blocked",
	"thread-yield: page acquire read blocked",
	"thread-yield: page acquire time sleeping (usecs)",
	"connection: total read I/Os",
	"reconciliation: page reconciliation calls",
	"reconciliation: page reconciliation calls for eviction",
	"reconciliation: page reconciliation block requires lookaside records",
	"reconciliation: page reconciliation block requires in-memory restoration",
	"reconciliation: split bytes currently awaiting free",
	"reconciliation: split objects currently awaiting free",
	"connection: pthread mutex shared lock read-lock calls",
	"connection: pthread mutex shared lock write-lock calls",
	"session: open cursor count",
	"session: open session count",
	"transaction: transaction begins",
	"transaction: transaction checkpoints",
	"transaction: transaction checkpoint generation",
	"transaction: transaction checkpoint currently running",
	"transaction: transaction checkpoint max time (msecs)",
	"transaction: transaction checkpoint min time (msecs)",
	"transaction: transaction checkpoint most recent time (msecs)",
	"transaction: transaction checkpoint total time (msecs)",
	"transaction: transactions committed",
	"transaction: transaction failures due to cache overflow",
	"transaction: transaction range of IDs currently pinned by a checkpoint",
	"transaction: transaction range of IDs currently pinned",
	"transaction: transactions rolled back",
	"transaction: transaction sync calls",
	"connection: total write I/Os",
};

const char *
__wt_stat_connection_desc(int slot)
{
	return (__stats_connection_desc[slot]);
}

void
__wt_stat_connection_init_single(WT_CONNECTION_STATS *stats)
{
	memset(stats, 0, sizeof(*stats));
}

void
__wt_stat_connection_init(WT_CONNECTION_IMPL *handle)
{
	int i;

	for (i = 0; i < WT_COUNTER_SLOTS; ++i) {
		handle->stats[i] = &handle->stat_array[i];
		__wt_stat_connection_init_single(handle->stats[i]);
	}
}

void
__wt_stat_connection_clear_single(WT_CONNECTION_STATS *stats)
{
	stats->async_cur_queue = 0;
		/* not clearing async_max_queue */
	stats->async_alloc_race = 0;
	stats->async_flush = 0;
	stats->async_alloc_view = 0;
	stats->async_full = 0;
	stats->async_nowork = 0;
	stats->async_op_alloc = 0;
	stats->async_op_compact = 0;
	stats->async_op_insert = 0;
	stats->async_op_remove = 0;
	stats->async_op_search = 0;
	stats->async_op_update = 0;
	stats->block_preload = 0;
	stats->block_read = 0;
	stats->block_write = 0;
	stats->block_byte_read = 0;
	stats->block_byte_write = 0;
	stats->block_map_read = 0;
	stats->block_byte_map_read = 0;
		/* not clearing cache_bytes_inuse */
	stats->cache_bytes_read = 0;
	stats->cache_bytes_write = 0;
	stats->cache_eviction_checkpoint = 0;
	stats->cache_eviction_queue_empty = 0;
	stats->cache_eviction_queue_not_empty = 0;
	stats->cache_eviction_server_evicting = 0;
	stats->cache_eviction_server_not_evicting = 0;
	stats->cache_eviction_slow = 0;
	stats->cache_eviction_worker_evicting = 0;
	stats->cache_eviction_force_fail = 0;
	stats->cache_eviction_hazard = 0;
	stats->cache_inmem_split = 0;
	stats->cache_eviction_internal = 0;
		/* not clearing cache_bytes_max */
		/* not clearing cache_eviction_maximum_page_size */
	stats->cache_eviction_dirty = 0;
	stats->cache_eviction_deepen = 0;
		/* not clearing cache_pages_inuse */
	stats->cache_eviction_force = 0;
	stats->cache_eviction_force_delete = 0;
	stats->cache_eviction_app = 0;
	stats->cache_read = 0;
	stats->cache_read_lookaside = 0;
	stats->cache_eviction_fail = 0;
	stats->cache_eviction_split = 0;
	stats->cache_eviction_walk = 0;
	stats->cache_write = 0;
		/* not clearing cache_overhead */
		/* not clearing cache_bytes_internal */
		/* not clearing cache_bytes_leaf */
		/* not clearing cache_bytes_overflow */
		/* not clearing cache_bytes_dirty */
		/* not clearing cache_pages_dirty */
	stats->cache_eviction_clean = 0;
		/* not clearing file_open */
	stats->memory_allocation = 0;
	stats->memory_free = 0;
	stats->memory_grow = 0;
	stats->cond_wait = 0;
	stats->rwlock_read = 0;
	stats->rwlock_write = 0;
	stats->read_io = 0;
	stats->write_io = 0;
	stats->cursor_create = 0;
	stats->cursor_insert = 0;
	stats->cursor_next = 0;
	stats->cursor_prev = 0;
	stats->cursor_remove = 0;
	stats->cursor_reset = 0;
	stats->cursor_restart = 0;
	stats->cursor_search = 0;
	stats->cursor_search_near = 0;
	stats->cursor_update = 0;
		/* not clearing dh_conn_handle_count */
	stats->dh_sweep_ref = 0;
	stats->dh_sweep_close = 0;
	stats->dh_sweep_remove = 0;
	stats->dh_sweep_tod = 0;
	stats->dh_sweeps = 0;
	stats->dh_session_handles = 0;
	stats->dh_session_sweeps = 0;
	stats->log_slot_closes = 0;
	stats->log_slot_races = 0;
	stats->log_slot_transitions = 0;
	stats->log_slot_joins = 0;
	stats->log_slot_toosmall = 0;
	stats->log_bytes_payload = 0;
	stats->log_bytes_written = 0;
	stats->log_compress_writes = 0;
	stats->log_compress_write_fails = 0;
	stats->log_compress_small = 0;
	stats->log_release_write_lsn = 0;
	stats->log_scans = 0;
	stats->log_scan_rereads = 0;
	stats->log_write_lsn = 0;
	stats->log_sync = 0;
	stats->log_sync_dir = 0;
	stats->log_writes = 0;
	stats->log_slot_consolidated = 0;
		/* not clearing log_max_filesize */
		/* not clearing log_prealloc_max */
	stats->log_prealloc_files = 0;
	stats->log_prealloc_used = 0;
	stats->log_slot_toobig = 0;
	stats->log_scan_records = 0;
	stats->log_compress_mem = 0;
		/* not clearing log_buffer_size */
	stats->log_compress_len = 0;
	stats->log_slot_coalesced = 0;
	stats->log_close_yields = 0;
	stats->lookaside_cursor_insert_bytes = 0;
	stats->lookaside_cursor_insert = 0;
	stats->lookaside_cursor_remove = 0;
		/* not clearing lsm_work_queue_app */
		/* not clearing lsm_work_queue_manager */
	stats->lsm_rows_merged = 0;
	stats->lsm_checkpoint_throttle = 0;
	stats->lsm_merge_throttle = 0;
		/* not clearing lsm_work_queue_switch */
	stats->lsm_work_units_discarded = 0;
	stats->lsm_work_units_done = 0;
	stats->lsm_work_units_created = 0;
	stats->lsm_work_queue_max = 0;
	stats->rec_pages_restore = 0;
	stats->rec_pages_lookaside = 0;
	stats->rec_pages = 0;
	stats->rec_pages_eviction = 0;
		/* not clearing rec_split_stashed_bytes */
		/* not clearing rec_split_stashed_objects */
		/* not clearing session_cursor_open */
		/* not clearing session_open */
	stats->page_busy_blocked = 0;
	stats->page_forcible_evict_blocked = 0;
	stats->page_locked_blocked = 0;
	stats->page_read_blocked = 0;
	stats->page_sleep = 0;
	stats->txn_begin = 0;
		/* not clearing txn_checkpoint_running */
		/* not clearing txn_checkpoint_generation */
		/* not clearing txn_checkpoint_time_max */
		/* not clearing txn_checkpoint_time_min */
		/* not clearing txn_checkpoint_time_recent */
		/* not clearing txn_checkpoint_time_total */
	stats->txn_checkpoint = 0;
	stats->txn_fail_cache = 0;
		/* not clearing txn_pinned_range */
		/* not clearing txn_pinned_checkpoint_range */
	stats->txn_sync = 0;
	stats->txn_commit = 0;
	stats->txn_rollback = 0;
}

void
__wt_stat_connection_clear_all(WT_CONNECTION_STATS **stats)
{
	u_int i;

	for (i = 0; i < WT_COUNTER_SLOTS; ++i)
		__wt_stat_connection_clear_single(stats[i]);
}

void
__wt_stat_connection_aggregate(
    WT_CONNECTION_STATS **from, WT_CONNECTION_STATS *to)
{
	to->async_cur_queue +=
	    (int64_t)WT_STAT_READ(from, async_cur_queue);
	to->async_max_queue +=
	    (int64_t)WT_STAT_READ(from, async_max_queue);
	to->async_alloc_race +=
	    (int64_t)WT_STAT_READ(from, async_alloc_race);
	to->async_flush +=
	    (int64_t)WT_STAT_READ(from, async_flush);
	to->async_alloc_view +=
	    (int64_t)WT_STAT_READ(from, async_alloc_view);
	to->async_full +=
	    (int64_t)WT_STAT_READ(from, async_full);
	to->async_nowork +=
	    (int64_t)WT_STAT_READ(from, async_nowork);
	to->async_op_alloc +=
	    (int64_t)WT_STAT_READ(from, async_op_alloc);
	to->async_op_compact +=
	    (int64_t)WT_STAT_READ(from, async_op_compact);
	to->async_op_insert +=
	    (int64_t)WT_STAT_READ(from, async_op_insert);
	to->async_op_remove +=
	    (int64_t)WT_STAT_READ(from, async_op_remove);
	to->async_op_search +=
	    (int64_t)WT_STAT_READ(from, async_op_search);
	to->async_op_update +=
	    (int64_t)WT_STAT_READ(from, async_op_update);
	to->block_preload +=
	    (int64_t)WT_STAT_READ(from, block_preload);
	to->block_read +=
	    (int64_t)WT_STAT_READ(from, block_read);
	to->block_write +=
	    (int64_t)WT_STAT_READ(from, block_write);
	to->block_byte_read +=
	    (int64_t)WT_STAT_READ(from, block_byte_read);
	to->block_byte_write +=
	    (int64_t)WT_STAT_READ(from, block_byte_write);
	to->block_map_read +=
	    (int64_t)WT_STAT_READ(from, block_map_read);
	to->block_byte_map_read +=
	    (int64_t)WT_STAT_READ(from, block_byte_map_read);
	to->cache_bytes_inuse +=
	    (int64_t)WT_STAT_READ(from, cache_bytes_inuse);
	to->cache_bytes_read +=
	    (int64_t)WT_STAT_READ(from, cache_bytes_read);
	to->cache_bytes_write +=
	    (int64_t)WT_STAT_READ(from, cache_bytes_write);
	to->cache_eviction_checkpoint +=
	    (int64_t)WT_STAT_READ(from, cache_eviction_checkpoint);
	to->cache_eviction_queue_empty +=
	    (int64_t)WT_STAT_READ(from, cache_eviction_queue_empty);
	to->cache_eviction_queue_not_empty +=
	    (int64_t)WT_STAT_READ(from, cache_eviction_queue_not_empty);
	to->cache_eviction_server_evicting +=
	    (int64_t)WT_STAT_READ(from, cache_eviction_server_evicting);
	to->cache_eviction_server_not_evicting +=
	    (int64_t)WT_STAT_READ(from, cache_eviction_server_not_evicting);
	to->cache_eviction_slow +=
	    (int64_t)WT_STAT_READ(from, cache_eviction_slow);
	to->cache_eviction_worker_evicting +=
	    (int64_t)WT_STAT_READ(from, cache_eviction_worker_evicting);
	to->cache_eviction_force_fail +=
	    (int64_t)WT_STAT_READ(from, cache_eviction_force_fail);
	to->cache_eviction_hazard +=
	    (int64_t)WT_STAT_READ(from, cache_eviction_hazard);
	to->cache_inmem_split +=
	    (int64_t)WT_STAT_READ(from, cache_inmem_split);
	to->cache_eviction_internal +=
	    (int64_t)WT_STAT_READ(from, cache_eviction_internal);
	to->cache_bytes_max +=
	    (int64_t)WT_STAT_READ(from, cache_bytes_max);
	to->cache_eviction_maximum_page_size +=
	    (int64_t)WT_STAT_READ(from, cache_eviction_maximum_page_size);
	to->cache_eviction_dirty +=
	    (int64_t)WT_STAT_READ(from, cache_eviction_dirty);
	to->cache_eviction_deepen +=
	    (int64_t)WT_STAT_READ(from, cache_eviction_deepen);
	to->cache_pages_inuse +=
	    (int64_t)WT_STAT_READ(from, cache_pages_inuse);
	to->cache_eviction_force +=
	    (int64_t)WT_STAT_READ(from, cache_eviction_force);
	to->cache_eviction_force_delete +=
	    (int64_t)WT_STAT_READ(from, cache_eviction_force_delete);
	to->cache_eviction_app +=
	    (int64_t)WT_STAT_READ(from, cache_eviction_app);
	to->cache_read +=
	    (int64_t)WT_STAT_READ(from, cache_read);
	to->cache_read_lookaside +=
	    (int64_t)WT_STAT_READ(from, cache_read_lookaside);
	to->cache_eviction_fail +=
	    (int64_t)WT_STAT_READ(from, cache_eviction_fail);
	to->cache_eviction_split +=
	    (int64_t)WT_STAT_READ(from, cache_eviction_split);
	to->cache_eviction_walk +=
	    (int64_t)WT_STAT_READ(from, cache_eviction_walk);
	to->cache_write +=
	    (int64_t)WT_STAT_READ(from, cache_write);
	to->cache_overhead +=
	    (int64_t)WT_STAT_READ(from, cache_overhead);
	to->cache_bytes_internal +=
	    (int64_t)WT_STAT_READ(from, cache_bytes_internal);
	to->cache_bytes_leaf +=
	    (int64_t)WT_STAT_READ(from, cache_bytes_leaf);
	to->cache_bytes_overflow +=
	    (int64_t)WT_STAT_READ(from, cache_bytes_overflow);
	to->cache_bytes_dirty +=
	    (int64_t)WT_STAT_READ(from, cache_bytes_dirty);
	to->cache_pages_dirty +=
	    (int64_t)WT_STAT_READ(from, cache_pages_dirty);
	to->cache_eviction_clean +=
	    (int64_t)WT_STAT_READ(from, cache_eviction_clean);
	to->file_open +=
	    (int64_t)WT_STAT_READ(from, file_open);
	to->memory_allocation +=
	    (int64_t)WT_STAT_READ(from, memory_allocation);
	to->memory_free +=
	    (int64_t)WT_STAT_READ(from, memory_free);
	to->memory_grow +=
	    (int64_t)WT_STAT_READ(from, memory_grow);
	to->cond_wait +=
	    (int64_t)WT_STAT_READ(from, cond_wait);
	to->rwlock_read +=
	    (int64_t)WT_STAT_READ(from, rwlock_read);
	to->rwlock_write +=
	    (int64_t)WT_STAT_READ(from, rwlock_write);
	to->read_io +=
	    (int64_t)WT_STAT_READ(from, read_io);
	to->write_io +=
	    (int64_t)WT_STAT_READ(from, write_io);
	to->cursor_create +=
	    (int64_t)WT_STAT_READ(from, cursor_create);
	to->cursor_insert +=
	    (int64_t)WT_STAT_READ(from, cursor_insert);
	to->cursor_next +=
	    (int64_t)WT_STAT_READ(from, cursor_next);
	to->cursor_prev +=
	    (int64_t)WT_STAT_READ(from, cursor_prev);
	to->cursor_remove +=
	    (int64_t)WT_STAT_READ(from, cursor_remove);
	to->cursor_reset +=
	    (int64_t)WT_STAT_READ(from, cursor_reset);
	to->cursor_restart +=
	    (int64_t)WT_STAT_READ(from, cursor_restart);
	to->cursor_search +=
	    (int64_t)WT_STAT_READ(from, cursor_search);
	to->cursor_search_near +=
	    (int64_t)WT_STAT_READ(from, cursor_search_near);
	to->cursor_update +=
	    (int64_t)WT_STAT_READ(from, cursor_update);
	to->dh_conn_handle_count +=
	    (int64_t)WT_STAT_READ(from, dh_conn_handle_count);
	to->dh_sweep_ref +=
	    (int64_t)WT_STAT_READ(from, dh_sweep_ref);
	to->dh_sweep_close +=
	    (int64_t)WT_STAT_READ(from, dh_sweep_close);
	to->dh_sweep_remove +=
	    (int64_t)WT_STAT_READ(from, dh_sweep_remove);
	to->dh_sweep_tod +=
	    (int64_t)WT_STAT_READ(from, dh_sweep_tod);
	to->dh_sweeps +=
	    (int64_t)WT_STAT_READ(from, dh_sweeps);
	to->dh_session_handles +=
	    (int64_t)WT_STAT_READ(from, dh_session_handles);
	to->dh_session_sweeps +=
	    (int64_t)WT_STAT_READ(from, dh_session_sweeps);
	to->log_slot_closes +=
	    (int64_t)WT_STAT_READ(from, log_slot_closes);
	to->log_slot_races +=
	    (int64_t)WT_STAT_READ(from, log_slot_races);
	to->log_slot_transitions +=
	    (int64_t)WT_STAT_READ(from, log_slot_transitions);
	to->log_slot_joins +=
	    (int64_t)WT_STAT_READ(from, log_slot_joins);
	to->log_slot_toosmall +=
	    (int64_t)WT_STAT_READ(from, log_slot_toosmall);
	to->log_bytes_payload +=
	    (int64_t)WT_STAT_READ(from, log_bytes_payload);
	to->log_bytes_written +=
	    (int64_t)WT_STAT_READ(from, log_bytes_written);
	to->log_compress_writes +=
	    (int64_t)WT_STAT_READ(from, log_compress_writes);
	to->log_compress_write_fails +=
	    (int64_t)WT_STAT_READ(from, log_compress_write_fails);
	to->log_compress_small +=
	    (int64_t)WT_STAT_READ(from, log_compress_small);
	to->log_release_write_lsn +=
	    (int64_t)WT_STAT_READ(from, log_release_write_lsn);
	to->log_scans +=
	    (int64_t)WT_STAT_READ(from, log_scans);
	to->log_scan_rereads +=
	    (int64_t)WT_STAT_READ(from, log_scan_rereads);
	to->log_write_lsn +=
	    (int64_t)WT_STAT_READ(from, log_write_lsn);
	to->log_sync +=
	    (int64_t)WT_STAT_READ(from, log_sync);
	to->log_sync_dir +=
	    (int64_t)WT_STAT_READ(from, log_sync_dir);
	to->log_writes +=
	    (int64_t)WT_STAT_READ(from, log_writes);
	to->log_slot_consolidated +=
	    (int64_t)WT_STAT_READ(from, log_slot_consolidated);
	to->log_max_filesize +=
	    (int64_t)WT_STAT_READ(from, log_max_filesize);
	to->log_prealloc_max +=
	    (int64_t)WT_STAT_READ(from, log_prealloc_max);
	to->log_prealloc_files +=
	    (int64_t)WT_STAT_READ(from, log_prealloc_files);
	to->log_prealloc_used +=
	    (int64_t)WT_STAT_READ(from, log_prealloc_used);
	to->log_slot_toobig +=
	    (int64_t)WT_STAT_READ(from, log_slot_toobig);
	to->log_scan_records +=
	    (int64_t)WT_STAT_READ(from, log_scan_records);
	to->log_compress_mem +=
	    (int64_t)WT_STAT_READ(from, log_compress_mem);
	to->log_buffer_size +=
	    (int64_t)WT_STAT_READ(from, log_buffer_size);
	to->log_compress_len +=
	    (int64_t)WT_STAT_READ(from, log_compress_len);
	to->log_slot_coalesced +=
	    (int64_t)WT_STAT_READ(from, log_slot_coalesced);
	to->log_close_yields +=
	    (int64_t)WT_STAT_READ(from, log_close_yields);
	to->lookaside_cursor_insert_bytes +=
	    (int64_t)WT_STAT_READ(from, lookaside_cursor_insert_bytes);
	to->lookaside_cursor_insert +=
	    (int64_t)WT_STAT_READ(from, lookaside_cursor_insert);
	to->lookaside_cursor_remove +=
	    (int64_t)WT_STAT_READ(from, lookaside_cursor_remove);
	to->lsm_work_queue_app +=
	    (int64_t)WT_STAT_READ(from, lsm_work_queue_app);
	to->lsm_work_queue_manager +=
	    (int64_t)WT_STAT_READ(from, lsm_work_queue_manager);
	to->lsm_rows_merged +=
	    (int64_t)WT_STAT_READ(from, lsm_rows_merged);
	to->lsm_checkpoint_throttle +=
	    (int64_t)WT_STAT_READ(from, lsm_checkpoint_throttle);
	to->lsm_merge_throttle +=
	    (int64_t)WT_STAT_READ(from, lsm_merge_throttle);
	to->lsm_work_queue_switch +=
	    (int64_t)WT_STAT_READ(from, lsm_work_queue_switch);
	to->lsm_work_units_discarded +=
	    (int64_t)WT_STAT_READ(from, lsm_work_units_discarded);
	to->lsm_work_units_done +=
	    (int64_t)WT_STAT_READ(from, lsm_work_units_done);
	to->lsm_work_units_created +=
	    (int64_t)WT_STAT_READ(from, lsm_work_units_created);
	to->lsm_work_queue_max +=
	    (int64_t)WT_STAT_READ(from, lsm_work_queue_max);
	to->rec_pages_restore +=
	    (int64_t)WT_STAT_READ(from, rec_pages_restore);
	to->rec_pages_lookaside +=
	    (int64_t)WT_STAT_READ(from, rec_pages_lookaside);
	to->rec_pages +=
	    (int64_t)WT_STAT_READ(from, rec_pages);
	to->rec_pages_eviction +=
	    (int64_t)WT_STAT_READ(from, rec_pages_eviction);
	to->rec_split_stashed_bytes +=
	    (int64_t)WT_STAT_READ(from, rec_split_stashed_bytes);
	to->rec_split_stashed_objects +=
	    (int64_t)WT_STAT_READ(from, rec_split_stashed_objects);
	to->session_cursor_open +=
	    (int64_t)WT_STAT_READ(from, session_cursor_open);
	to->session_open +=
	    (int64_t)WT_STAT_READ(from, session_open);
	to->page_busy_blocked +=
	    (int64_t)WT_STAT_READ(from, page_busy_blocked);
	to->page_forcible_evict_blocked +=
	    (int64_t)WT_STAT_READ(from, page_forcible_evict_blocked);
	to->page_locked_blocked +=
	    (int64_t)WT_STAT_READ(from, page_locked_blocked);
	to->page_read_blocked +=
	    (int64_t)WT_STAT_READ(from, page_read_blocked);
	to->page_sleep +=
	    (int64_t)WT_STAT_READ(from, page_sleep);
	to->txn_begin +=
	    (int64_t)WT_STAT_READ(from, txn_begin);
	to->txn_checkpoint_running +=
	    (int64_t)WT_STAT_READ(from, txn_checkpoint_running);
	to->txn_checkpoint_generation +=
	    (int64_t)WT_STAT_READ(from, txn_checkpoint_generation);
	to->txn_checkpoint_time_max +=
	    (int64_t)WT_STAT_READ(from, txn_checkpoint_time_max);
	to->txn_checkpoint_time_min +=
	    (int64_t)WT_STAT_READ(from, txn_checkpoint_time_min);
	to->txn_checkpoint_time_recent +=
	    (int64_t)WT_STAT_READ(from, txn_checkpoint_time_recent);
	to->txn_checkpoint_time_total +=
	    (int64_t)WT_STAT_READ(from, txn_checkpoint_time_total);
	to->txn_checkpoint +=
	    (int64_t)WT_STAT_READ(from, txn_checkpoint);
	to->txn_fail_cache +=
	    (int64_t)WT_STAT_READ(from, txn_fail_cache);
	to->txn_pinned_range +=
	    (int64_t)WT_STAT_READ(from, txn_pinned_range);
	to->txn_pinned_checkpoint_range +=
	    (int64_t)WT_STAT_READ(from, txn_pinned_checkpoint_range);
	to->txn_sync +=
	    (int64_t)WT_STAT_READ(from, txn_sync);
	to->txn_commit +=
	    (int64_t)WT_STAT_READ(from, txn_commit);
	to->txn_rollback +=
	    (int64_t)WT_STAT_READ(from, txn_rollback);
}
