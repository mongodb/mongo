/* DO NOT EDIT: automatically built by dist/stat.py. */

#include "wt_internal.h"

static const char * const __stats_dsrc_desc[] = {
	"LSM: bloom filter false positives",
	"LSM: bloom filter hits",
	"LSM: bloom filter misses",
	"LSM: bloom filter pages evicted from cache",
	"LSM: bloom filter pages read into cache",
	"LSM: bloom filters in the LSM tree",
	"LSM: chunks in the LSM tree",
	"LSM: highest merge generation in the LSM tree",
	"LSM: queries that could have benefited from a Bloom filter that did not exist",
	"LSM: sleep for LSM checkpoint throttle",
	"LSM: sleep for LSM merge throttle",
	"LSM: total size of bloom filters",
	"block-manager: allocations requiring file extension",
	"block-manager: blocks allocated",
	"block-manager: blocks freed",
	"block-manager: checkpoint size",
	"block-manager: file allocation unit size",
	"block-manager: file bytes available for reuse",
	"block-manager: file magic number",
	"block-manager: file major version number",
	"block-manager: file size in bytes",
	"block-manager: minor version number",
	"btree: btree checkpoint generation",
	"btree: column-store fixed-size leaf pages",
	"btree: column-store internal pages",
	"btree: column-store variable-size RLE encoded values",
	"btree: column-store variable-size deleted values",
	"btree: column-store variable-size leaf pages",
	"btree: fixed-record size",
	"btree: maximum internal page key size",
	"btree: maximum internal page size",
	"btree: maximum leaf page key size",
	"btree: maximum leaf page size",
	"btree: maximum leaf page value size",
	"btree: maximum tree depth",
	"btree: number of key/value pairs",
	"btree: overflow pages",
	"btree: pages rewritten by compaction",
	"btree: row-store internal pages",
	"btree: row-store leaf pages",
	"cache: bytes currently in the cache",
	"cache: bytes read into cache",
	"cache: bytes written from cache",
	"cache: checkpoint blocked page eviction",
	"cache: data source pages selected for eviction unable to be evicted",
	"cache: eviction walk passes of a file",
	"cache: eviction walk target pages histogram - 0-9",
	"cache: eviction walk target pages histogram - 10-31",
	"cache: eviction walk target pages histogram - 128 and higher",
	"cache: eviction walk target pages histogram - 32-63",
	"cache: eviction walk target pages histogram - 64-128",
	"cache: eviction walks abandoned",
	"cache: eviction walks gave up because they restarted their walk twice",
	"cache: eviction walks gave up because they saw too many pages and found no candidates",
	"cache: eviction walks gave up because they saw too many pages and found too few candidates",
	"cache: eviction walks reached end of tree",
	"cache: eviction walks started from root of tree",
	"cache: eviction walks started from saved location in tree",
	"cache: hazard pointer blocked page eviction",
	"cache: in-memory page passed criteria to be split",
	"cache: in-memory page splits",
	"cache: internal pages evicted",
	"cache: internal pages split during eviction",
	"cache: leaf pages split during eviction",
	"cache: modified pages evicted",
	"cache: overflow pages read into cache",
	"cache: page split during eviction deepened the tree",
	"cache: page written requiring cache overflow records",
	"cache: pages read into cache",
	"cache: pages read into cache after truncate",
	"cache: pages read into cache after truncate in prepare state",
	"cache: pages read into cache requiring cache overflow entries",
	"cache: pages requested from the cache",
	"cache: pages seen by eviction walk",
	"cache: pages written from cache",
	"cache: pages written requiring in-memory restoration",
	"cache: tracked dirty bytes in the cache",
	"cache: unmodified pages evicted",
	"cache_walk: Average difference between current eviction generation when the page was last considered",
	"cache_walk: Average on-disk page image size seen",
	"cache_walk: Average time in cache for pages that have been visited by the eviction server",
	"cache_walk: Average time in cache for pages that have not been visited by the eviction server",
	"cache_walk: Clean pages currently in cache",
	"cache_walk: Current eviction generation",
	"cache_walk: Dirty pages currently in cache",
	"cache_walk: Entries in the root page",
	"cache_walk: Internal pages currently in cache",
	"cache_walk: Leaf pages currently in cache",
	"cache_walk: Maximum difference between current eviction generation when the page was last considered",
	"cache_walk: Maximum page size seen",
	"cache_walk: Minimum on-disk page image size seen",
	"cache_walk: Number of pages never visited by eviction server",
	"cache_walk: On-disk page image sizes smaller than a single allocation unit",
	"cache_walk: Pages created in memory and never written",
	"cache_walk: Pages currently queued for eviction",
	"cache_walk: Pages that could not be queued for eviction",
	"cache_walk: Refs skipped during cache traversal",
	"cache_walk: Size of the root page",
	"cache_walk: Total number of pages currently in cache",
	"compression: compressed pages read",
	"compression: compressed pages written",
	"compression: page written failed to compress",
	"compression: page written was too small to compress",
	"compression: raw compression call failed, additional data available",
	"compression: raw compression call failed, no additional data available",
	"compression: raw compression call succeeded",
	"cursor: bulk-loaded cursor-insert calls",
	"cursor: create calls",
	"cursor: cursor operation restarted",
	"cursor: cursor-insert key and value bytes inserted",
	"cursor: cursor-remove key bytes removed",
	"cursor: cursor-update value bytes updated",
	"cursor: cursors cached on close",
	"cursor: cursors reused from cache",
	"cursor: insert calls",
	"cursor: modify calls",
	"cursor: next calls",
	"cursor: prev calls",
	"cursor: remove calls",
	"cursor: reserve calls",
	"cursor: reset calls",
	"cursor: search calls",
	"cursor: search near calls",
	"cursor: truncate calls",
	"cursor: update calls",
	"reconciliation: dictionary matches",
	"reconciliation: fast-path pages deleted",
	"reconciliation: internal page key bytes discarded using suffix compression",
	"reconciliation: internal page multi-block writes",
	"reconciliation: internal-page overflow keys",
	"reconciliation: leaf page key bytes discarded using prefix compression",
	"reconciliation: leaf page multi-block writes",
	"reconciliation: leaf-page overflow keys",
	"reconciliation: maximum blocks required for a page",
	"reconciliation: overflow values written",
	"reconciliation: page checksum matches",
	"reconciliation: page reconciliation calls",
	"reconciliation: page reconciliation calls for eviction",
	"reconciliation: pages deleted",
	"session: cached cursor count",
	"session: object compaction",
	"session: open cursor count",
	"transaction: update conflicts",
};

int
__wt_stat_dsrc_desc(WT_CURSOR_STAT *cst, int slot, const char **p)
{
	WT_UNUSED(cst);
	*p = __stats_dsrc_desc[slot];
	return (0);
}

void
__wt_stat_dsrc_init_single(WT_DSRC_STATS *stats)
{
	memset(stats, 0, sizeof(*stats));
}

int
__wt_stat_dsrc_init(
    WT_SESSION_IMPL *session, WT_DATA_HANDLE *handle)
{
	int i;

	WT_RET(__wt_calloc(session, (size_t)WT_COUNTER_SLOTS,
	    sizeof(*handle->stat_array), &handle->stat_array));

	for (i = 0; i < WT_COUNTER_SLOTS; ++i) {
		handle->stats[i] = &handle->stat_array[i];
		__wt_stat_dsrc_init_single(handle->stats[i]);
	}
	return (0);
}

void
__wt_stat_dsrc_discard(
    WT_SESSION_IMPL *session, WT_DATA_HANDLE *handle)
{
	__wt_free(session, handle->stat_array);
}

void
__wt_stat_dsrc_clear_single(WT_DSRC_STATS *stats)
{
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
	stats->btree_column_rle = 0;
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
		/* not clearing cache_bytes_inuse */
	stats->cache_bytes_read = 0;
	stats->cache_bytes_write = 0;
	stats->cache_eviction_checkpoint = 0;
	stats->cache_eviction_fail = 0;
	stats->cache_eviction_walk_passes = 0;
	stats->cache_eviction_target_page_lt10 = 0;
	stats->cache_eviction_target_page_lt32 = 0;
	stats->cache_eviction_target_page_ge128 = 0;
	stats->cache_eviction_target_page_lt64 = 0;
	stats->cache_eviction_target_page_lt128 = 0;
	stats->cache_eviction_walks_abandoned = 0;
	stats->cache_eviction_walks_stopped = 0;
	stats->cache_eviction_walks_gave_up_no_targets = 0;
	stats->cache_eviction_walks_gave_up_ratio = 0;
	stats->cache_eviction_walks_ended = 0;
	stats->cache_eviction_walk_from_root = 0;
	stats->cache_eviction_walk_saved_pos = 0;
	stats->cache_eviction_hazard = 0;
	stats->cache_inmem_splittable = 0;
	stats->cache_inmem_split = 0;
	stats->cache_eviction_internal = 0;
	stats->cache_eviction_split_internal = 0;
	stats->cache_eviction_split_leaf = 0;
	stats->cache_eviction_dirty = 0;
	stats->cache_read_overflow = 0;
	stats->cache_eviction_deepen = 0;
	stats->cache_write_lookaside = 0;
	stats->cache_read = 0;
	stats->cache_read_deleted = 0;
	stats->cache_read_deleted_prepared = 0;
	stats->cache_read_lookaside = 0;
	stats->cache_pages_requested = 0;
	stats->cache_eviction_pages_seen = 0;
	stats->cache_write = 0;
	stats->cache_write_restore = 0;
		/* not clearing cache_bytes_dirty */
	stats->cache_eviction_clean = 0;
		/* not clearing cache_state_gen_avg_gap */
		/* not clearing cache_state_avg_written_size */
		/* not clearing cache_state_avg_visited_age */
		/* not clearing cache_state_avg_unvisited_age */
		/* not clearing cache_state_pages_clean */
		/* not clearing cache_state_gen_current */
		/* not clearing cache_state_pages_dirty */
		/* not clearing cache_state_root_entries */
		/* not clearing cache_state_pages_internal */
		/* not clearing cache_state_pages_leaf */
		/* not clearing cache_state_gen_max_gap */
		/* not clearing cache_state_max_pagesize */
		/* not clearing cache_state_min_written_size */
		/* not clearing cache_state_unvisited_count */
		/* not clearing cache_state_smaller_alloc_size */
		/* not clearing cache_state_memory */
		/* not clearing cache_state_queued */
		/* not clearing cache_state_not_queueable */
		/* not clearing cache_state_refs_skipped */
		/* not clearing cache_state_root_size */
		/* not clearing cache_state_pages */
	stats->compress_read = 0;
	stats->compress_write = 0;
	stats->compress_write_fail = 0;
	stats->compress_write_too_small = 0;
	stats->compress_raw_fail_temporary = 0;
	stats->compress_raw_fail = 0;
	stats->compress_raw_ok = 0;
	stats->cursor_insert_bulk = 0;
	stats->cursor_create = 0;
	stats->cursor_restart = 0;
	stats->cursor_insert_bytes = 0;
	stats->cursor_remove_bytes = 0;
	stats->cursor_update_bytes = 0;
	stats->cursor_cache = 0;
	stats->cursor_reopen = 0;
	stats->cursor_insert = 0;
	stats->cursor_modify = 0;
	stats->cursor_next = 0;
	stats->cursor_prev = 0;
	stats->cursor_remove = 0;
	stats->cursor_reserve = 0;
	stats->cursor_reset = 0;
	stats->cursor_search = 0;
	stats->cursor_search_near = 0;
	stats->cursor_truncate = 0;
	stats->cursor_update = 0;
	stats->rec_dictionary = 0;
	stats->rec_page_delete_fast = 0;
	stats->rec_suffix_compression = 0;
	stats->rec_multiblock_internal = 0;
	stats->rec_overflow_key_internal = 0;
	stats->rec_prefix_compression = 0;
	stats->rec_multiblock_leaf = 0;
	stats->rec_overflow_key_leaf = 0;
	stats->rec_multiblock_max = 0;
	stats->rec_overflow_value = 0;
	stats->rec_page_match = 0;
	stats->rec_pages = 0;
	stats->rec_pages_eviction = 0;
	stats->rec_page_delete = 0;
		/* not clearing session_cursor_cached */
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
	to->bloom_false_positive += from->bloom_false_positive;
	to->bloom_hit += from->bloom_hit;
	to->bloom_miss += from->bloom_miss;
	to->bloom_page_evict += from->bloom_page_evict;
	to->bloom_page_read += from->bloom_page_read;
	to->bloom_count += from->bloom_count;
	to->lsm_chunk_count += from->lsm_chunk_count;
	if (from->lsm_generation_max > to->lsm_generation_max)
		to->lsm_generation_max = from->lsm_generation_max;
	to->lsm_lookup_no_bloom += from->lsm_lookup_no_bloom;
	to->lsm_checkpoint_throttle += from->lsm_checkpoint_throttle;
	to->lsm_merge_throttle += from->lsm_merge_throttle;
	to->bloom_size += from->bloom_size;
	to->block_extension += from->block_extension;
	to->block_alloc += from->block_alloc;
	to->block_free += from->block_free;
	to->block_checkpoint_size += from->block_checkpoint_size;
	if (from->allocation_size > to->allocation_size)
		to->allocation_size = from->allocation_size;
	to->block_reuse_bytes += from->block_reuse_bytes;
	if (from->block_magic > to->block_magic)
		to->block_magic = from->block_magic;
	if (from->block_major > to->block_major)
		to->block_major = from->block_major;
	to->block_size += from->block_size;
	if (from->block_minor > to->block_minor)
		to->block_minor = from->block_minor;
	to->btree_checkpoint_generation += from->btree_checkpoint_generation;
	to->btree_column_fix += from->btree_column_fix;
	to->btree_column_internal += from->btree_column_internal;
	to->btree_column_rle += from->btree_column_rle;
	to->btree_column_deleted += from->btree_column_deleted;
	to->btree_column_variable += from->btree_column_variable;
	if (from->btree_fixed_len > to->btree_fixed_len)
		to->btree_fixed_len = from->btree_fixed_len;
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
	to->btree_entries += from->btree_entries;
	to->btree_overflow += from->btree_overflow;
	to->btree_compact_rewrite += from->btree_compact_rewrite;
	to->btree_row_internal += from->btree_row_internal;
	to->btree_row_leaf += from->btree_row_leaf;
	to->cache_bytes_inuse += from->cache_bytes_inuse;
	to->cache_bytes_read += from->cache_bytes_read;
	to->cache_bytes_write += from->cache_bytes_write;
	to->cache_eviction_checkpoint += from->cache_eviction_checkpoint;
	to->cache_eviction_fail += from->cache_eviction_fail;
	to->cache_eviction_walk_passes += from->cache_eviction_walk_passes;
	to->cache_eviction_target_page_lt10 +=
	    from->cache_eviction_target_page_lt10;
	to->cache_eviction_target_page_lt32 +=
	    from->cache_eviction_target_page_lt32;
	to->cache_eviction_target_page_ge128 +=
	    from->cache_eviction_target_page_ge128;
	to->cache_eviction_target_page_lt64 +=
	    from->cache_eviction_target_page_lt64;
	to->cache_eviction_target_page_lt128 +=
	    from->cache_eviction_target_page_lt128;
	to->cache_eviction_walks_abandoned +=
	    from->cache_eviction_walks_abandoned;
	to->cache_eviction_walks_stopped +=
	    from->cache_eviction_walks_stopped;
	to->cache_eviction_walks_gave_up_no_targets +=
	    from->cache_eviction_walks_gave_up_no_targets;
	to->cache_eviction_walks_gave_up_ratio +=
	    from->cache_eviction_walks_gave_up_ratio;
	to->cache_eviction_walks_ended += from->cache_eviction_walks_ended;
	to->cache_eviction_walk_from_root +=
	    from->cache_eviction_walk_from_root;
	to->cache_eviction_walk_saved_pos +=
	    from->cache_eviction_walk_saved_pos;
	to->cache_eviction_hazard += from->cache_eviction_hazard;
	to->cache_inmem_splittable += from->cache_inmem_splittable;
	to->cache_inmem_split += from->cache_inmem_split;
	to->cache_eviction_internal += from->cache_eviction_internal;
	to->cache_eviction_split_internal +=
	    from->cache_eviction_split_internal;
	to->cache_eviction_split_leaf += from->cache_eviction_split_leaf;
	to->cache_eviction_dirty += from->cache_eviction_dirty;
	to->cache_read_overflow += from->cache_read_overflow;
	to->cache_eviction_deepen += from->cache_eviction_deepen;
	to->cache_write_lookaside += from->cache_write_lookaside;
	to->cache_read += from->cache_read;
	to->cache_read_deleted += from->cache_read_deleted;
	to->cache_read_deleted_prepared += from->cache_read_deleted_prepared;
	to->cache_read_lookaside += from->cache_read_lookaside;
	to->cache_pages_requested += from->cache_pages_requested;
	to->cache_eviction_pages_seen += from->cache_eviction_pages_seen;
	to->cache_write += from->cache_write;
	to->cache_write_restore += from->cache_write_restore;
	to->cache_bytes_dirty += from->cache_bytes_dirty;
	to->cache_eviction_clean += from->cache_eviction_clean;
	to->cache_state_gen_avg_gap += from->cache_state_gen_avg_gap;
	to->cache_state_avg_written_size +=
	    from->cache_state_avg_written_size;
	to->cache_state_avg_visited_age += from->cache_state_avg_visited_age;
	to->cache_state_avg_unvisited_age +=
	    from->cache_state_avg_unvisited_age;
	to->cache_state_pages_clean += from->cache_state_pages_clean;
	to->cache_state_gen_current += from->cache_state_gen_current;
	to->cache_state_pages_dirty += from->cache_state_pages_dirty;
	to->cache_state_root_entries += from->cache_state_root_entries;
	to->cache_state_pages_internal += from->cache_state_pages_internal;
	to->cache_state_pages_leaf += from->cache_state_pages_leaf;
	to->cache_state_gen_max_gap += from->cache_state_gen_max_gap;
	to->cache_state_max_pagesize += from->cache_state_max_pagesize;
	to->cache_state_min_written_size +=
	    from->cache_state_min_written_size;
	to->cache_state_unvisited_count += from->cache_state_unvisited_count;
	to->cache_state_smaller_alloc_size +=
	    from->cache_state_smaller_alloc_size;
	to->cache_state_memory += from->cache_state_memory;
	to->cache_state_queued += from->cache_state_queued;
	to->cache_state_not_queueable += from->cache_state_not_queueable;
	to->cache_state_refs_skipped += from->cache_state_refs_skipped;
	to->cache_state_root_size += from->cache_state_root_size;
	to->cache_state_pages += from->cache_state_pages;
	to->compress_read += from->compress_read;
	to->compress_write += from->compress_write;
	to->compress_write_fail += from->compress_write_fail;
	to->compress_write_too_small += from->compress_write_too_small;
	to->compress_raw_fail_temporary += from->compress_raw_fail_temporary;
	to->compress_raw_fail += from->compress_raw_fail;
	to->compress_raw_ok += from->compress_raw_ok;
	to->cursor_insert_bulk += from->cursor_insert_bulk;
	to->cursor_create += from->cursor_create;
	to->cursor_restart += from->cursor_restart;
	to->cursor_insert_bytes += from->cursor_insert_bytes;
	to->cursor_remove_bytes += from->cursor_remove_bytes;
	to->cursor_update_bytes += from->cursor_update_bytes;
	to->cursor_cache += from->cursor_cache;
	to->cursor_reopen += from->cursor_reopen;
	to->cursor_insert += from->cursor_insert;
	to->cursor_modify += from->cursor_modify;
	to->cursor_next += from->cursor_next;
	to->cursor_prev += from->cursor_prev;
	to->cursor_remove += from->cursor_remove;
	to->cursor_reserve += from->cursor_reserve;
	to->cursor_reset += from->cursor_reset;
	to->cursor_search += from->cursor_search;
	to->cursor_search_near += from->cursor_search_near;
	to->cursor_truncate += from->cursor_truncate;
	to->cursor_update += from->cursor_update;
	to->rec_dictionary += from->rec_dictionary;
	to->rec_page_delete_fast += from->rec_page_delete_fast;
	to->rec_suffix_compression += from->rec_suffix_compression;
	to->rec_multiblock_internal += from->rec_multiblock_internal;
	to->rec_overflow_key_internal += from->rec_overflow_key_internal;
	to->rec_prefix_compression += from->rec_prefix_compression;
	to->rec_multiblock_leaf += from->rec_multiblock_leaf;
	to->rec_overflow_key_leaf += from->rec_overflow_key_leaf;
	if (from->rec_multiblock_max > to->rec_multiblock_max)
		to->rec_multiblock_max = from->rec_multiblock_max;
	to->rec_overflow_value += from->rec_overflow_value;
	to->rec_page_match += from->rec_page_match;
	to->rec_pages += from->rec_pages;
	to->rec_pages_eviction += from->rec_pages_eviction;
	to->rec_page_delete += from->rec_page_delete;
	to->session_cursor_cached += from->session_cursor_cached;
	to->session_compact += from->session_compact;
	to->session_cursor_open += from->session_cursor_open;
	to->txn_update_conflict += from->txn_update_conflict;
}

void
__wt_stat_dsrc_aggregate(
    WT_DSRC_STATS **from, WT_DSRC_STATS *to)
{
	int64_t v;

	to->bloom_false_positive += WT_STAT_READ(from, bloom_false_positive);
	to->bloom_hit += WT_STAT_READ(from, bloom_hit);
	to->bloom_miss += WT_STAT_READ(from, bloom_miss);
	to->bloom_page_evict += WT_STAT_READ(from, bloom_page_evict);
	to->bloom_page_read += WT_STAT_READ(from, bloom_page_read);
	to->bloom_count += WT_STAT_READ(from, bloom_count);
	to->lsm_chunk_count += WT_STAT_READ(from, lsm_chunk_count);
	if ((v = WT_STAT_READ(from, lsm_generation_max)) >
	    to->lsm_generation_max)
		to->lsm_generation_max = v;
	to->lsm_lookup_no_bloom += WT_STAT_READ(from, lsm_lookup_no_bloom);
	to->lsm_checkpoint_throttle +=
	    WT_STAT_READ(from, lsm_checkpoint_throttle);
	to->lsm_merge_throttle += WT_STAT_READ(from, lsm_merge_throttle);
	to->bloom_size += WT_STAT_READ(from, bloom_size);
	to->block_extension += WT_STAT_READ(from, block_extension);
	to->block_alloc += WT_STAT_READ(from, block_alloc);
	to->block_free += WT_STAT_READ(from, block_free);
	to->block_checkpoint_size +=
	    WT_STAT_READ(from, block_checkpoint_size);
	if ((v = WT_STAT_READ(from, allocation_size)) > to->allocation_size)
		to->allocation_size = v;
	to->block_reuse_bytes += WT_STAT_READ(from, block_reuse_bytes);
	if ((v = WT_STAT_READ(from, block_magic)) > to->block_magic)
		to->block_magic = v;
	if ((v = WT_STAT_READ(from, block_major)) > to->block_major)
		to->block_major = v;
	to->block_size += WT_STAT_READ(from, block_size);
	if ((v = WT_STAT_READ(from, block_minor)) > to->block_minor)
		to->block_minor = v;
	to->btree_checkpoint_generation +=
	    WT_STAT_READ(from, btree_checkpoint_generation);
	to->btree_column_fix += WT_STAT_READ(from, btree_column_fix);
	to->btree_column_internal +=
	    WT_STAT_READ(from, btree_column_internal);
	to->btree_column_rle += WT_STAT_READ(from, btree_column_rle);
	to->btree_column_deleted += WT_STAT_READ(from, btree_column_deleted);
	to->btree_column_variable +=
	    WT_STAT_READ(from, btree_column_variable);
	if ((v = WT_STAT_READ(from, btree_fixed_len)) > to->btree_fixed_len)
		to->btree_fixed_len = v;
	if ((v = WT_STAT_READ(from, btree_maxintlkey)) > to->btree_maxintlkey)
		to->btree_maxintlkey = v;
	if ((v = WT_STAT_READ(from, btree_maxintlpage)) >
	    to->btree_maxintlpage)
		to->btree_maxintlpage = v;
	if ((v = WT_STAT_READ(from, btree_maxleafkey)) > to->btree_maxleafkey)
		to->btree_maxleafkey = v;
	if ((v = WT_STAT_READ(from, btree_maxleafpage)) >
	    to->btree_maxleafpage)
		to->btree_maxleafpage = v;
	if ((v = WT_STAT_READ(from, btree_maxleafvalue)) >
	    to->btree_maxleafvalue)
		to->btree_maxleafvalue = v;
	if ((v = WT_STAT_READ(from, btree_maximum_depth)) >
	    to->btree_maximum_depth)
		to->btree_maximum_depth = v;
	to->btree_entries += WT_STAT_READ(from, btree_entries);
	to->btree_overflow += WT_STAT_READ(from, btree_overflow);
	to->btree_compact_rewrite +=
	    WT_STAT_READ(from, btree_compact_rewrite);
	to->btree_row_internal += WT_STAT_READ(from, btree_row_internal);
	to->btree_row_leaf += WT_STAT_READ(from, btree_row_leaf);
	to->cache_bytes_inuse += WT_STAT_READ(from, cache_bytes_inuse);
	to->cache_bytes_read += WT_STAT_READ(from, cache_bytes_read);
	to->cache_bytes_write += WT_STAT_READ(from, cache_bytes_write);
	to->cache_eviction_checkpoint +=
	    WT_STAT_READ(from, cache_eviction_checkpoint);
	to->cache_eviction_fail += WT_STAT_READ(from, cache_eviction_fail);
	to->cache_eviction_walk_passes +=
	    WT_STAT_READ(from, cache_eviction_walk_passes);
	to->cache_eviction_target_page_lt10 +=
	    WT_STAT_READ(from, cache_eviction_target_page_lt10);
	to->cache_eviction_target_page_lt32 +=
	    WT_STAT_READ(from, cache_eviction_target_page_lt32);
	to->cache_eviction_target_page_ge128 +=
	    WT_STAT_READ(from, cache_eviction_target_page_ge128);
	to->cache_eviction_target_page_lt64 +=
	    WT_STAT_READ(from, cache_eviction_target_page_lt64);
	to->cache_eviction_target_page_lt128 +=
	    WT_STAT_READ(from, cache_eviction_target_page_lt128);
	to->cache_eviction_walks_abandoned +=
	    WT_STAT_READ(from, cache_eviction_walks_abandoned);
	to->cache_eviction_walks_stopped +=
	    WT_STAT_READ(from, cache_eviction_walks_stopped);
	to->cache_eviction_walks_gave_up_no_targets +=
	    WT_STAT_READ(from, cache_eviction_walks_gave_up_no_targets);
	to->cache_eviction_walks_gave_up_ratio +=
	    WT_STAT_READ(from, cache_eviction_walks_gave_up_ratio);
	to->cache_eviction_walks_ended +=
	    WT_STAT_READ(from, cache_eviction_walks_ended);
	to->cache_eviction_walk_from_root +=
	    WT_STAT_READ(from, cache_eviction_walk_from_root);
	to->cache_eviction_walk_saved_pos +=
	    WT_STAT_READ(from, cache_eviction_walk_saved_pos);
	to->cache_eviction_hazard +=
	    WT_STAT_READ(from, cache_eviction_hazard);
	to->cache_inmem_splittable +=
	    WT_STAT_READ(from, cache_inmem_splittable);
	to->cache_inmem_split += WT_STAT_READ(from, cache_inmem_split);
	to->cache_eviction_internal +=
	    WT_STAT_READ(from, cache_eviction_internal);
	to->cache_eviction_split_internal +=
	    WT_STAT_READ(from, cache_eviction_split_internal);
	to->cache_eviction_split_leaf +=
	    WT_STAT_READ(from, cache_eviction_split_leaf);
	to->cache_eviction_dirty += WT_STAT_READ(from, cache_eviction_dirty);
	to->cache_read_overflow += WT_STAT_READ(from, cache_read_overflow);
	to->cache_eviction_deepen +=
	    WT_STAT_READ(from, cache_eviction_deepen);
	to->cache_write_lookaside +=
	    WT_STAT_READ(from, cache_write_lookaside);
	to->cache_read += WT_STAT_READ(from, cache_read);
	to->cache_read_deleted += WT_STAT_READ(from, cache_read_deleted);
	to->cache_read_deleted_prepared +=
	    WT_STAT_READ(from, cache_read_deleted_prepared);
	to->cache_read_lookaside += WT_STAT_READ(from, cache_read_lookaside);
	to->cache_pages_requested +=
	    WT_STAT_READ(from, cache_pages_requested);
	to->cache_eviction_pages_seen +=
	    WT_STAT_READ(from, cache_eviction_pages_seen);
	to->cache_write += WT_STAT_READ(from, cache_write);
	to->cache_write_restore += WT_STAT_READ(from, cache_write_restore);
	to->cache_bytes_dirty += WT_STAT_READ(from, cache_bytes_dirty);
	to->cache_eviction_clean += WT_STAT_READ(from, cache_eviction_clean);
	to->cache_state_gen_avg_gap +=
	    WT_STAT_READ(from, cache_state_gen_avg_gap);
	to->cache_state_avg_written_size +=
	    WT_STAT_READ(from, cache_state_avg_written_size);
	to->cache_state_avg_visited_age +=
	    WT_STAT_READ(from, cache_state_avg_visited_age);
	to->cache_state_avg_unvisited_age +=
	    WT_STAT_READ(from, cache_state_avg_unvisited_age);
	to->cache_state_pages_clean +=
	    WT_STAT_READ(from, cache_state_pages_clean);
	to->cache_state_gen_current +=
	    WT_STAT_READ(from, cache_state_gen_current);
	to->cache_state_pages_dirty +=
	    WT_STAT_READ(from, cache_state_pages_dirty);
	to->cache_state_root_entries +=
	    WT_STAT_READ(from, cache_state_root_entries);
	to->cache_state_pages_internal +=
	    WT_STAT_READ(from, cache_state_pages_internal);
	to->cache_state_pages_leaf +=
	    WT_STAT_READ(from, cache_state_pages_leaf);
	to->cache_state_gen_max_gap +=
	    WT_STAT_READ(from, cache_state_gen_max_gap);
	to->cache_state_max_pagesize +=
	    WT_STAT_READ(from, cache_state_max_pagesize);
	to->cache_state_min_written_size +=
	    WT_STAT_READ(from, cache_state_min_written_size);
	to->cache_state_unvisited_count +=
	    WT_STAT_READ(from, cache_state_unvisited_count);
	to->cache_state_smaller_alloc_size +=
	    WT_STAT_READ(from, cache_state_smaller_alloc_size);
	to->cache_state_memory += WT_STAT_READ(from, cache_state_memory);
	to->cache_state_queued += WT_STAT_READ(from, cache_state_queued);
	to->cache_state_not_queueable +=
	    WT_STAT_READ(from, cache_state_not_queueable);
	to->cache_state_refs_skipped +=
	    WT_STAT_READ(from, cache_state_refs_skipped);
	to->cache_state_root_size +=
	    WT_STAT_READ(from, cache_state_root_size);
	to->cache_state_pages += WT_STAT_READ(from, cache_state_pages);
	to->compress_read += WT_STAT_READ(from, compress_read);
	to->compress_write += WT_STAT_READ(from, compress_write);
	to->compress_write_fail += WT_STAT_READ(from, compress_write_fail);
	to->compress_write_too_small +=
	    WT_STAT_READ(from, compress_write_too_small);
	to->compress_raw_fail_temporary +=
	    WT_STAT_READ(from, compress_raw_fail_temporary);
	to->compress_raw_fail += WT_STAT_READ(from, compress_raw_fail);
	to->compress_raw_ok += WT_STAT_READ(from, compress_raw_ok);
	to->cursor_insert_bulk += WT_STAT_READ(from, cursor_insert_bulk);
	to->cursor_create += WT_STAT_READ(from, cursor_create);
	to->cursor_restart += WT_STAT_READ(from, cursor_restart);
	to->cursor_insert_bytes += WT_STAT_READ(from, cursor_insert_bytes);
	to->cursor_remove_bytes += WT_STAT_READ(from, cursor_remove_bytes);
	to->cursor_update_bytes += WT_STAT_READ(from, cursor_update_bytes);
	to->cursor_cache += WT_STAT_READ(from, cursor_cache);
	to->cursor_reopen += WT_STAT_READ(from, cursor_reopen);
	to->cursor_insert += WT_STAT_READ(from, cursor_insert);
	to->cursor_modify += WT_STAT_READ(from, cursor_modify);
	to->cursor_next += WT_STAT_READ(from, cursor_next);
	to->cursor_prev += WT_STAT_READ(from, cursor_prev);
	to->cursor_remove += WT_STAT_READ(from, cursor_remove);
	to->cursor_reserve += WT_STAT_READ(from, cursor_reserve);
	to->cursor_reset += WT_STAT_READ(from, cursor_reset);
	to->cursor_search += WT_STAT_READ(from, cursor_search);
	to->cursor_search_near += WT_STAT_READ(from, cursor_search_near);
	to->cursor_truncate += WT_STAT_READ(from, cursor_truncate);
	to->cursor_update += WT_STAT_READ(from, cursor_update);
	to->rec_dictionary += WT_STAT_READ(from, rec_dictionary);
	to->rec_page_delete_fast += WT_STAT_READ(from, rec_page_delete_fast);
	to->rec_suffix_compression +=
	    WT_STAT_READ(from, rec_suffix_compression);
	to->rec_multiblock_internal +=
	    WT_STAT_READ(from, rec_multiblock_internal);
	to->rec_overflow_key_internal +=
	    WT_STAT_READ(from, rec_overflow_key_internal);
	to->rec_prefix_compression +=
	    WT_STAT_READ(from, rec_prefix_compression);
	to->rec_multiblock_leaf += WT_STAT_READ(from, rec_multiblock_leaf);
	to->rec_overflow_key_leaf +=
	    WT_STAT_READ(from, rec_overflow_key_leaf);
	if ((v = WT_STAT_READ(from, rec_multiblock_max)) >
	    to->rec_multiblock_max)
		to->rec_multiblock_max = v;
	to->rec_overflow_value += WT_STAT_READ(from, rec_overflow_value);
	to->rec_page_match += WT_STAT_READ(from, rec_page_match);
	to->rec_pages += WT_STAT_READ(from, rec_pages);
	to->rec_pages_eviction += WT_STAT_READ(from, rec_pages_eviction);
	to->rec_page_delete += WT_STAT_READ(from, rec_page_delete);
	to->session_cursor_cached +=
	    WT_STAT_READ(from, session_cursor_cached);
	to->session_compact += WT_STAT_READ(from, session_compact);
	to->session_cursor_open += WT_STAT_READ(from, session_cursor_open);
	to->txn_update_conflict += WT_STAT_READ(from, txn_update_conflict);
}

static const char * const __stats_connection_desc[] = {
	"LSM: application work units currently queued",
	"LSM: merge work units currently queued",
	"LSM: rows merged in an LSM tree",
	"LSM: sleep for LSM checkpoint throttle",
	"LSM: sleep for LSM merge throttle",
	"LSM: switch work units currently queued",
	"LSM: tree maintenance operations discarded",
	"LSM: tree maintenance operations executed",
	"LSM: tree maintenance operations scheduled",
	"LSM: tree queue hit maximum",
	"async: current work queue length",
	"async: maximum work queue length",
	"async: number of allocation state races",
	"async: number of flush calls",
	"async: number of operation slots viewed for allocation",
	"async: number of times operation allocation failed",
	"async: number of times worker found no work",
	"async: total allocations",
	"async: total compact calls",
	"async: total insert calls",
	"async: total remove calls",
	"async: total search calls",
	"async: total update calls",
	"block-manager: blocks pre-loaded",
	"block-manager: blocks read",
	"block-manager: blocks written",
	"block-manager: bytes read",
	"block-manager: bytes written",
	"block-manager: bytes written for checkpoint",
	"block-manager: mapped blocks read",
	"block-manager: mapped bytes read",
	"cache: application threads page read from disk to cache count",
	"cache: application threads page read from disk to cache time (usecs)",
	"cache: application threads page write from cache to disk count",
	"cache: application threads page write from cache to disk time (usecs)",
	"cache: bytes belonging to page images in the cache",
	"cache: bytes belonging to the cache overflow table in the cache",
	"cache: bytes currently in the cache",
	"cache: bytes not belonging to page images in the cache",
	"cache: bytes read into cache",
	"cache: bytes written from cache",
	"cache: cache overflow score",
	"cache: cache overflow table entries",
	"cache: cache overflow table insert calls",
	"cache: cache overflow table remove calls",
	"cache: checkpoint blocked page eviction",
	"cache: eviction calls to get a page",
	"cache: eviction calls to get a page found queue empty",
	"cache: eviction calls to get a page found queue empty after locking",
	"cache: eviction currently operating in aggressive mode",
	"cache: eviction empty score",
	"cache: eviction passes of a file",
	"cache: eviction server candidate queue empty when topping up",
	"cache: eviction server candidate queue not empty when topping up",
	"cache: eviction server evicting pages",
	"cache: eviction server slept, because we did not make progress with eviction",
	"cache: eviction server unable to reach eviction goal",
	"cache: eviction state",
	"cache: eviction walk target pages histogram - 0-9",
	"cache: eviction walk target pages histogram - 10-31",
	"cache: eviction walk target pages histogram - 128 and higher",
	"cache: eviction walk target pages histogram - 32-63",
	"cache: eviction walk target pages histogram - 64-128",
	"cache: eviction walks abandoned",
	"cache: eviction walks gave up because they restarted their walk twice",
	"cache: eviction walks gave up because they saw too many pages and found no candidates",
	"cache: eviction walks gave up because they saw too many pages and found too few candidates",
	"cache: eviction walks reached end of tree",
	"cache: eviction walks started from root of tree",
	"cache: eviction walks started from saved location in tree",
	"cache: eviction worker thread active",
	"cache: eviction worker thread created",
	"cache: eviction worker thread evicting pages",
	"cache: eviction worker thread removed",
	"cache: eviction worker thread stable number",
	"cache: failed eviction of pages that exceeded the in-memory maximum count",
	"cache: failed eviction of pages that exceeded the in-memory maximum time (usecs)",
	"cache: files with active eviction walks",
	"cache: files with new eviction walks started",
	"cache: force re-tuning of eviction workers once in a while",
	"cache: hazard pointer blocked page eviction",
	"cache: hazard pointer check calls",
	"cache: hazard pointer check entries walked",
	"cache: hazard pointer maximum array length",
	"cache: in-memory page passed criteria to be split",
	"cache: in-memory page splits",
	"cache: internal pages evicted",
	"cache: internal pages split during eviction",
	"cache: leaf pages split during eviction",
	"cache: maximum bytes configured",
	"cache: maximum page size at eviction",
	"cache: modified pages evicted",
	"cache: modified pages evicted by application threads",
	"cache: operations timed out waiting for space in cache",
	"cache: overflow pages read into cache",
	"cache: page split during eviction deepened the tree",
	"cache: page written requiring cache overflow records",
	"cache: pages currently held in the cache",
	"cache: pages evicted because they exceeded the in-memory maximum count",
	"cache: pages evicted because they exceeded the in-memory maximum time (usecs)",
	"cache: pages evicted because they had chains of deleted items count",
	"cache: pages evicted because they had chains of deleted items time (usecs)",
	"cache: pages evicted by application threads",
	"cache: pages queued for eviction",
	"cache: pages queued for urgent eviction",
	"cache: pages queued for urgent eviction during walk",
	"cache: pages read into cache",
	"cache: pages read into cache after truncate",
	"cache: pages read into cache after truncate in prepare state",
	"cache: pages read into cache requiring cache overflow entries",
	"cache: pages read into cache requiring cache overflow for checkpoint",
	"cache: pages read into cache skipping older cache overflow entries",
	"cache: pages read into cache with skipped cache overflow entries needed later",
	"cache: pages read into cache with skipped cache overflow entries needed later by checkpoint",
	"cache: pages requested from the cache",
	"cache: pages seen by eviction walk",
	"cache: pages selected for eviction unable to be evicted",
	"cache: pages walked for eviction",
	"cache: pages written from cache",
	"cache: pages written requiring in-memory restoration",
	"cache: percentage overhead",
	"cache: tracked bytes belonging to internal pages in the cache",
	"cache: tracked bytes belonging to leaf pages in the cache",
	"cache: tracked dirty bytes in the cache",
	"cache: tracked dirty pages in the cache",
	"cache: unmodified pages evicted",
	"connection: auto adjusting condition resets",
	"connection: auto adjusting condition wait calls",
	"connection: detected system time went backwards",
	"connection: files currently open",
	"connection: memory allocations",
	"connection: memory frees",
	"connection: memory re-allocations",
	"connection: pthread mutex condition wait calls",
	"connection: pthread mutex shared lock read-lock calls",
	"connection: pthread mutex shared lock write-lock calls",
	"connection: total fsync I/Os",
	"connection: total read I/Os",
	"connection: total write I/Os",
	"cursor: cursor create calls",
	"cursor: cursor insert calls",
	"cursor: cursor modify calls",
	"cursor: cursor next calls",
	"cursor: cursor operation restarted",
	"cursor: cursor prev calls",
	"cursor: cursor remove calls",
	"cursor: cursor reserve calls",
	"cursor: cursor reset calls",
	"cursor: cursor search calls",
	"cursor: cursor search near calls",
	"cursor: cursor sweep buckets",
	"cursor: cursor sweep cursors closed",
	"cursor: cursor sweep cursors examined",
	"cursor: cursor sweeps",
	"cursor: cursor update calls",
	"cursor: cursors cached on close",
	"cursor: cursors reused from cache",
	"cursor: truncate calls",
	"data-handle: connection data handles currently active",
	"data-handle: connection sweep candidate became referenced",
	"data-handle: connection sweep dhandles closed",
	"data-handle: connection sweep dhandles removed from hash list",
	"data-handle: connection sweep time-of-death sets",
	"data-handle: connection sweeps",
	"data-handle: session dhandles swept",
	"data-handle: session sweep attempts",
	"lock: checkpoint lock acquisitions",
	"lock: checkpoint lock application thread wait time (usecs)",
	"lock: checkpoint lock internal thread wait time (usecs)",
	"lock: commit timestamp queue lock application thread time waiting (usecs)",
	"lock: commit timestamp queue lock internal thread time waiting (usecs)",
	"lock: commit timestamp queue read lock acquisitions",
	"lock: commit timestamp queue write lock acquisitions",
	"lock: dhandle lock application thread time waiting (usecs)",
	"lock: dhandle lock internal thread time waiting (usecs)",
	"lock: dhandle read lock acquisitions",
	"lock: dhandle write lock acquisitions",
	"lock: metadata lock acquisitions",
	"lock: metadata lock application thread wait time (usecs)",
	"lock: metadata lock internal thread wait time (usecs)",
	"lock: read timestamp queue lock application thread time waiting (usecs)",
	"lock: read timestamp queue lock internal thread time waiting (usecs)",
	"lock: read timestamp queue read lock acquisitions",
	"lock: read timestamp queue write lock acquisitions",
	"lock: schema lock acquisitions",
	"lock: schema lock application thread wait time (usecs)",
	"lock: schema lock internal thread wait time (usecs)",
	"lock: table lock application thread time waiting for the table lock (usecs)",
	"lock: table lock internal thread time waiting for the table lock (usecs)",
	"lock: table read lock acquisitions",
	"lock: table write lock acquisitions",
	"lock: txn global lock application thread time waiting (usecs)",
	"lock: txn global lock internal thread time waiting (usecs)",
	"lock: txn global read lock acquisitions",
	"lock: txn global write lock acquisitions",
	"log: busy returns attempting to switch slots",
	"log: force archive time sleeping (usecs)",
	"log: log bytes of payload data",
	"log: log bytes written",
	"log: log files manually zero-filled",
	"log: log flush operations",
	"log: log force write operations",
	"log: log force write operations skipped",
	"log: log records compressed",
	"log: log records not compressed",
	"log: log records too small to compress",
	"log: log release advances write LSN",
	"log: log scan operations",
	"log: log scan records requiring two reads",
	"log: log server thread advances write LSN",
	"log: log server thread write LSN walk skipped",
	"log: log sync operations",
	"log: log sync time duration (usecs)",
	"log: log sync_dir operations",
	"log: log sync_dir time duration (usecs)",
	"log: log write operations",
	"log: logging bytes consolidated",
	"log: maximum log file size",
	"log: number of pre-allocated log files to create",
	"log: pre-allocated log files not ready and missed",
	"log: pre-allocated log files prepared",
	"log: pre-allocated log files used",
	"log: records processed by log scan",
	"log: slot close lost race",
	"log: slot close unbuffered waits",
	"log: slot closures",
	"log: slot join atomic update races",
	"log: slot join calls atomic updates raced",
	"log: slot join calls did not yield",
	"log: slot join calls found active slot closed",
	"log: slot join calls slept",
	"log: slot join calls yielded",
	"log: slot join found active slot closed",
	"log: slot joins yield time (usecs)",
	"log: slot transitions unable to find free slot",
	"log: slot unbuffered writes",
	"log: total in-memory size of compressed records",
	"log: total log buffer size",
	"log: total size of compressed records",
	"log: written slots coalesced",
	"log: yields waiting for previous log file close",
	"perf: file system read latency histogram (bucket 1) - 10-49ms",
	"perf: file system read latency histogram (bucket 2) - 50-99ms",
	"perf: file system read latency histogram (bucket 3) - 100-249ms",
	"perf: file system read latency histogram (bucket 4) - 250-499ms",
	"perf: file system read latency histogram (bucket 5) - 500-999ms",
	"perf: file system read latency histogram (bucket 6) - 1000ms+",
	"perf: file system write latency histogram (bucket 1) - 10-49ms",
	"perf: file system write latency histogram (bucket 2) - 50-99ms",
	"perf: file system write latency histogram (bucket 3) - 100-249ms",
	"perf: file system write latency histogram (bucket 4) - 250-499ms",
	"perf: file system write latency histogram (bucket 5) - 500-999ms",
	"perf: file system write latency histogram (bucket 6) - 1000ms+",
	"perf: operation read latency histogram (bucket 1) - 100-249us",
	"perf: operation read latency histogram (bucket 2) - 250-499us",
	"perf: operation read latency histogram (bucket 3) - 500-999us",
	"perf: operation read latency histogram (bucket 4) - 1000-9999us",
	"perf: operation read latency histogram (bucket 5) - 10000us+",
	"perf: operation write latency histogram (bucket 1) - 100-249us",
	"perf: operation write latency histogram (bucket 2) - 250-499us",
	"perf: operation write latency histogram (bucket 3) - 500-999us",
	"perf: operation write latency histogram (bucket 4) - 1000-9999us",
	"perf: operation write latency histogram (bucket 5) - 10000us+",
	"reconciliation: fast-path pages deleted",
	"reconciliation: page reconciliation calls",
	"reconciliation: page reconciliation calls for eviction",
	"reconciliation: pages deleted",
	"reconciliation: split bytes currently awaiting free",
	"reconciliation: split objects currently awaiting free",
	"session: open cursor count",
	"session: open session count",
	"session: session query timestamp calls",
	"session: table alter failed calls",
	"session: table alter successful calls",
	"session: table alter unchanged and skipped",
	"session: table compact failed calls",
	"session: table compact successful calls",
	"session: table create failed calls",
	"session: table create successful calls",
	"session: table drop failed calls",
	"session: table drop successful calls",
	"session: table rebalance failed calls",
	"session: table rebalance successful calls",
	"session: table rename failed calls",
	"session: table rename successful calls",
	"session: table salvage failed calls",
	"session: table salvage successful calls",
	"session: table truncate failed calls",
	"session: table truncate successful calls",
	"session: table verify failed calls",
	"session: table verify successful calls",
	"thread-state: active filesystem fsync calls",
	"thread-state: active filesystem read calls",
	"thread-state: active filesystem write calls",
	"thread-yield: application thread time evicting (usecs)",
	"thread-yield: application thread time waiting for cache (usecs)",
	"thread-yield: connection close blocked waiting for transaction state stabilization",
	"thread-yield: connection close yielded for lsm manager shutdown",
	"thread-yield: data handle lock yielded",
	"thread-yield: get reference for page index and slot time sleeping (usecs)",
	"thread-yield: log server sync yielded for log write",
	"thread-yield: page access yielded due to prepare state change",
	"thread-yield: page acquire busy blocked",
	"thread-yield: page acquire eviction blocked",
	"thread-yield: page acquire locked blocked",
	"thread-yield: page acquire read blocked",
	"thread-yield: page acquire time sleeping (usecs)",
	"thread-yield: page delete rollback time sleeping for state change (usecs)",
	"thread-yield: page reconciliation yielded due to child modification",
	"transaction: commit timestamp queue entries walked",
	"transaction: commit timestamp queue insert to empty",
	"transaction: commit timestamp queue inserts to head",
	"transaction: commit timestamp queue inserts total",
	"transaction: commit timestamp queue length",
	"transaction: number of named snapshots created",
	"transaction: number of named snapshots dropped",
	"transaction: prepared transactions",
	"transaction: prepared transactions committed",
	"transaction: prepared transactions currently active",
	"transaction: prepared transactions rolled back",
	"transaction: query timestamp calls",
	"transaction: read timestamp queue entries walked",
	"transaction: read timestamp queue insert to empty",
	"transaction: read timestamp queue inserts to head",
	"transaction: read timestamp queue inserts total",
	"transaction: read timestamp queue length",
	"transaction: rollback to stable calls",
	"transaction: rollback to stable updates aborted",
	"transaction: rollback to stable updates removed from cache overflow",
	"transaction: set timestamp calls",
	"transaction: set timestamp commit calls",
	"transaction: set timestamp commit updates",
	"transaction: set timestamp oldest calls",
	"transaction: set timestamp oldest updates",
	"transaction: set timestamp stable calls",
	"transaction: set timestamp stable updates",
	"transaction: transaction begins",
	"transaction: transaction checkpoint currently running",
	"transaction: transaction checkpoint generation",
	"transaction: transaction checkpoint max time (msecs)",
	"transaction: transaction checkpoint min time (msecs)",
	"transaction: transaction checkpoint most recent time (msecs)",
	"transaction: transaction checkpoint scrub dirty target",
	"transaction: transaction checkpoint scrub time (msecs)",
	"transaction: transaction checkpoint total time (msecs)",
	"transaction: transaction checkpoints",
	"transaction: transaction checkpoints skipped because database was clean",
	"transaction: transaction failures due to cache overflow",
	"transaction: transaction fsync calls for checkpoint after allocating the transaction ID",
	"transaction: transaction fsync duration for checkpoint after allocating the transaction ID (usecs)",
	"transaction: transaction range of IDs currently pinned",
	"transaction: transaction range of IDs currently pinned by a checkpoint",
	"transaction: transaction range of IDs currently pinned by named snapshots",
	"transaction: transaction range of timestamps currently pinned",
	"transaction: transaction range of timestamps pinned by a checkpoint",
	"transaction: transaction range of timestamps pinned by the oldest timestamp",
	"transaction: transaction sync calls",
	"transaction: transactions committed",
	"transaction: transactions rolled back",
	"transaction: update conflicts",
};

int
__wt_stat_connection_desc(WT_CURSOR_STAT *cst, int slot, const char **p)
{
	WT_UNUSED(cst);
	*p = __stats_connection_desc[slot];
	return (0);
}

void
__wt_stat_connection_init_single(WT_CONNECTION_STATS *stats)
{
	memset(stats, 0, sizeof(*stats));
}

int
__wt_stat_connection_init(
    WT_SESSION_IMPL *session, WT_CONNECTION_IMPL *handle)
{
	int i;

	WT_RET(__wt_calloc(session, (size_t)WT_COUNTER_SLOTS,
	    sizeof(*handle->stat_array), &handle->stat_array));

	for (i = 0; i < WT_COUNTER_SLOTS; ++i) {
		handle->stats[i] = &handle->stat_array[i];
		__wt_stat_connection_init_single(handle->stats[i]);
	}
	return (0);
}

void
__wt_stat_connection_discard(
    WT_SESSION_IMPL *session, WT_CONNECTION_IMPL *handle)
{
	__wt_free(session, handle->stat_array);
}

void
__wt_stat_connection_clear_single(WT_CONNECTION_STATS *stats)
{
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
	stats->block_byte_write_checkpoint = 0;
	stats->block_map_read = 0;
	stats->block_byte_map_read = 0;
	stats->cache_read_app_count = 0;
	stats->cache_read_app_time = 0;
	stats->cache_write_app_count = 0;
	stats->cache_write_app_time = 0;
		/* not clearing cache_bytes_image */
		/* not clearing cache_bytes_lookaside */
		/* not clearing cache_bytes_inuse */
		/* not clearing cache_bytes_other */
	stats->cache_bytes_read = 0;
	stats->cache_bytes_write = 0;
		/* not clearing cache_lookaside_score */
		/* not clearing cache_lookaside_entries */
	stats->cache_lookaside_insert = 0;
	stats->cache_lookaside_remove = 0;
	stats->cache_eviction_checkpoint = 0;
	stats->cache_eviction_get_ref = 0;
	stats->cache_eviction_get_ref_empty = 0;
	stats->cache_eviction_get_ref_empty2 = 0;
		/* not clearing cache_eviction_aggressive_set */
		/* not clearing cache_eviction_empty_score */
	stats->cache_eviction_walk_passes = 0;
	stats->cache_eviction_queue_empty = 0;
	stats->cache_eviction_queue_not_empty = 0;
	stats->cache_eviction_server_evicting = 0;
	stats->cache_eviction_server_slept = 0;
	stats->cache_eviction_slow = 0;
		/* not clearing cache_eviction_state */
	stats->cache_eviction_target_page_lt10 = 0;
	stats->cache_eviction_target_page_lt32 = 0;
	stats->cache_eviction_target_page_ge128 = 0;
	stats->cache_eviction_target_page_lt64 = 0;
	stats->cache_eviction_target_page_lt128 = 0;
	stats->cache_eviction_walks_abandoned = 0;
	stats->cache_eviction_walks_stopped = 0;
	stats->cache_eviction_walks_gave_up_no_targets = 0;
	stats->cache_eviction_walks_gave_up_ratio = 0;
	stats->cache_eviction_walks_ended = 0;
	stats->cache_eviction_walk_from_root = 0;
	stats->cache_eviction_walk_saved_pos = 0;
		/* not clearing cache_eviction_active_workers */
	stats->cache_eviction_worker_created = 0;
	stats->cache_eviction_worker_evicting = 0;
	stats->cache_eviction_worker_removed = 0;
		/* not clearing cache_eviction_stable_state_workers */
	stats->cache_eviction_force_fail = 0;
	stats->cache_eviction_force_fail_time = 0;
		/* not clearing cache_eviction_walks_active */
	stats->cache_eviction_walks_started = 0;
	stats->cache_eviction_force_retune = 0;
	stats->cache_eviction_hazard = 0;
	stats->cache_hazard_checks = 0;
	stats->cache_hazard_walks = 0;
	stats->cache_hazard_max = 0;
	stats->cache_inmem_splittable = 0;
	stats->cache_inmem_split = 0;
	stats->cache_eviction_internal = 0;
	stats->cache_eviction_split_internal = 0;
	stats->cache_eviction_split_leaf = 0;
		/* not clearing cache_bytes_max */
		/* not clearing cache_eviction_maximum_page_size */
	stats->cache_eviction_dirty = 0;
	stats->cache_eviction_app_dirty = 0;
	stats->cache_timed_out_ops = 0;
	stats->cache_read_overflow = 0;
	stats->cache_eviction_deepen = 0;
	stats->cache_write_lookaside = 0;
		/* not clearing cache_pages_inuse */
	stats->cache_eviction_force = 0;
	stats->cache_eviction_force_time = 0;
	stats->cache_eviction_force_delete = 0;
	stats->cache_eviction_force_delete_time = 0;
	stats->cache_eviction_app = 0;
	stats->cache_eviction_pages_queued = 0;
	stats->cache_eviction_pages_queued_urgent = 0;
	stats->cache_eviction_pages_queued_oldest = 0;
	stats->cache_read = 0;
	stats->cache_read_deleted = 0;
	stats->cache_read_deleted_prepared = 0;
	stats->cache_read_lookaside = 0;
	stats->cache_read_lookaside_checkpoint = 0;
	stats->cache_read_lookaside_skipped = 0;
	stats->cache_read_lookaside_delay = 0;
	stats->cache_read_lookaside_delay_checkpoint = 0;
	stats->cache_pages_requested = 0;
	stats->cache_eviction_pages_seen = 0;
	stats->cache_eviction_fail = 0;
	stats->cache_eviction_walk = 0;
	stats->cache_write = 0;
	stats->cache_write_restore = 0;
		/* not clearing cache_overhead */
		/* not clearing cache_bytes_internal */
		/* not clearing cache_bytes_leaf */
		/* not clearing cache_bytes_dirty */
		/* not clearing cache_pages_dirty */
	stats->cache_eviction_clean = 0;
	stats->cond_auto_wait_reset = 0;
	stats->cond_auto_wait = 0;
	stats->time_travel = 0;
		/* not clearing file_open */
	stats->memory_allocation = 0;
	stats->memory_free = 0;
	stats->memory_grow = 0;
	stats->cond_wait = 0;
	stats->rwlock_read = 0;
	stats->rwlock_write = 0;
	stats->fsync_io = 0;
	stats->read_io = 0;
	stats->write_io = 0;
	stats->cursor_create = 0;
	stats->cursor_insert = 0;
	stats->cursor_modify = 0;
	stats->cursor_next = 0;
	stats->cursor_restart = 0;
	stats->cursor_prev = 0;
	stats->cursor_remove = 0;
	stats->cursor_reserve = 0;
	stats->cursor_reset = 0;
	stats->cursor_search = 0;
	stats->cursor_search_near = 0;
	stats->cursor_sweep_buckets = 0;
	stats->cursor_sweep_closed = 0;
	stats->cursor_sweep_examined = 0;
	stats->cursor_sweep = 0;
	stats->cursor_update = 0;
	stats->cursor_cache = 0;
	stats->cursor_reopen = 0;
	stats->cursor_truncate = 0;
		/* not clearing dh_conn_handle_count */
	stats->dh_sweep_ref = 0;
	stats->dh_sweep_close = 0;
	stats->dh_sweep_remove = 0;
	stats->dh_sweep_tod = 0;
	stats->dh_sweeps = 0;
	stats->dh_session_handles = 0;
	stats->dh_session_sweeps = 0;
	stats->lock_checkpoint_count = 0;
	stats->lock_checkpoint_wait_application = 0;
	stats->lock_checkpoint_wait_internal = 0;
	stats->lock_commit_timestamp_wait_application = 0;
	stats->lock_commit_timestamp_wait_internal = 0;
	stats->lock_commit_timestamp_read_count = 0;
	stats->lock_commit_timestamp_write_count = 0;
	stats->lock_dhandle_wait_application = 0;
	stats->lock_dhandle_wait_internal = 0;
	stats->lock_dhandle_read_count = 0;
	stats->lock_dhandle_write_count = 0;
	stats->lock_metadata_count = 0;
	stats->lock_metadata_wait_application = 0;
	stats->lock_metadata_wait_internal = 0;
	stats->lock_read_timestamp_wait_application = 0;
	stats->lock_read_timestamp_wait_internal = 0;
	stats->lock_read_timestamp_read_count = 0;
	stats->lock_read_timestamp_write_count = 0;
	stats->lock_schema_count = 0;
	stats->lock_schema_wait_application = 0;
	stats->lock_schema_wait_internal = 0;
	stats->lock_table_wait_application = 0;
	stats->lock_table_wait_internal = 0;
	stats->lock_table_read_count = 0;
	stats->lock_table_write_count = 0;
	stats->lock_txn_global_wait_application = 0;
	stats->lock_txn_global_wait_internal = 0;
	stats->lock_txn_global_read_count = 0;
	stats->lock_txn_global_write_count = 0;
	stats->log_slot_switch_busy = 0;
	stats->log_force_archive_sleep = 0;
	stats->log_bytes_payload = 0;
	stats->log_bytes_written = 0;
	stats->log_zero_fills = 0;
	stats->log_flush = 0;
	stats->log_force_write = 0;
	stats->log_force_write_skip = 0;
	stats->log_compress_writes = 0;
	stats->log_compress_write_fails = 0;
	stats->log_compress_small = 0;
	stats->log_release_write_lsn = 0;
	stats->log_scans = 0;
	stats->log_scan_rereads = 0;
	stats->log_write_lsn = 0;
	stats->log_write_lsn_skip = 0;
	stats->log_sync = 0;
		/* not clearing log_sync_duration */
	stats->log_sync_dir = 0;
		/* not clearing log_sync_dir_duration */
	stats->log_writes = 0;
	stats->log_slot_consolidated = 0;
		/* not clearing log_max_filesize */
		/* not clearing log_prealloc_max */
	stats->log_prealloc_missed = 0;
	stats->log_prealloc_files = 0;
	stats->log_prealloc_used = 0;
	stats->log_scan_records = 0;
	stats->log_slot_close_race = 0;
	stats->log_slot_close_unbuf = 0;
	stats->log_slot_closes = 0;
	stats->log_slot_races = 0;
	stats->log_slot_yield_race = 0;
	stats->log_slot_immediate = 0;
	stats->log_slot_yield_close = 0;
	stats->log_slot_yield_sleep = 0;
	stats->log_slot_yield = 0;
	stats->log_slot_active_closed = 0;
		/* not clearing log_slot_yield_duration */
	stats->log_slot_no_free_slots = 0;
	stats->log_slot_unbuffered = 0;
	stats->log_compress_mem = 0;
		/* not clearing log_buffer_size */
	stats->log_compress_len = 0;
	stats->log_slot_coalesced = 0;
	stats->log_close_yields = 0;
	stats->perf_hist_fsread_latency_lt50 = 0;
	stats->perf_hist_fsread_latency_lt100 = 0;
	stats->perf_hist_fsread_latency_lt250 = 0;
	stats->perf_hist_fsread_latency_lt500 = 0;
	stats->perf_hist_fsread_latency_lt1000 = 0;
	stats->perf_hist_fsread_latency_gt1000 = 0;
	stats->perf_hist_fswrite_latency_lt50 = 0;
	stats->perf_hist_fswrite_latency_lt100 = 0;
	stats->perf_hist_fswrite_latency_lt250 = 0;
	stats->perf_hist_fswrite_latency_lt500 = 0;
	stats->perf_hist_fswrite_latency_lt1000 = 0;
	stats->perf_hist_fswrite_latency_gt1000 = 0;
	stats->perf_hist_opread_latency_lt250 = 0;
	stats->perf_hist_opread_latency_lt500 = 0;
	stats->perf_hist_opread_latency_lt1000 = 0;
	stats->perf_hist_opread_latency_lt10000 = 0;
	stats->perf_hist_opread_latency_gt10000 = 0;
	stats->perf_hist_opwrite_latency_lt250 = 0;
	stats->perf_hist_opwrite_latency_lt500 = 0;
	stats->perf_hist_opwrite_latency_lt1000 = 0;
	stats->perf_hist_opwrite_latency_lt10000 = 0;
	stats->perf_hist_opwrite_latency_gt10000 = 0;
	stats->rec_page_delete_fast = 0;
	stats->rec_pages = 0;
	stats->rec_pages_eviction = 0;
	stats->rec_page_delete = 0;
		/* not clearing rec_split_stashed_bytes */
		/* not clearing rec_split_stashed_objects */
		/* not clearing session_cursor_open */
		/* not clearing session_open */
	stats->session_query_ts = 0;
		/* not clearing session_table_alter_fail */
		/* not clearing session_table_alter_success */
		/* not clearing session_table_alter_skip */
		/* not clearing session_table_compact_fail */
		/* not clearing session_table_compact_success */
		/* not clearing session_table_create_fail */
		/* not clearing session_table_create_success */
		/* not clearing session_table_drop_fail */
		/* not clearing session_table_drop_success */
		/* not clearing session_table_rebalance_fail */
		/* not clearing session_table_rebalance_success */
		/* not clearing session_table_rename_fail */
		/* not clearing session_table_rename_success */
		/* not clearing session_table_salvage_fail */
		/* not clearing session_table_salvage_success */
		/* not clearing session_table_truncate_fail */
		/* not clearing session_table_truncate_success */
		/* not clearing session_table_verify_fail */
		/* not clearing session_table_verify_success */
		/* not clearing thread_fsync_active */
		/* not clearing thread_read_active */
		/* not clearing thread_write_active */
	stats->application_evict_time = 0;
	stats->application_cache_time = 0;
	stats->txn_release_blocked = 0;
	stats->conn_close_blocked_lsm = 0;
	stats->dhandle_lock_blocked = 0;
	stats->page_index_slot_ref_blocked = 0;
	stats->log_server_sync_blocked = 0;
	stats->prepared_transition_blocked_page = 0;
	stats->page_busy_blocked = 0;
	stats->page_forcible_evict_blocked = 0;
	stats->page_locked_blocked = 0;
	stats->page_read_blocked = 0;
	stats->page_sleep = 0;
	stats->page_del_rollback_blocked = 0;
	stats->child_modify_blocked_page = 0;
	stats->txn_commit_queue_walked = 0;
	stats->txn_commit_queue_empty = 0;
	stats->txn_commit_queue_head = 0;
	stats->txn_commit_queue_inserts = 0;
	stats->txn_commit_queue_len = 0;
	stats->txn_snapshots_created = 0;
	stats->txn_snapshots_dropped = 0;
	stats->txn_prepare = 0;
	stats->txn_prepare_commit = 0;
	stats->txn_prepare_active = 0;
	stats->txn_prepare_rollback = 0;
	stats->txn_query_ts = 0;
	stats->txn_read_queue_walked = 0;
	stats->txn_read_queue_empty = 0;
	stats->txn_read_queue_head = 0;
	stats->txn_read_queue_inserts = 0;
	stats->txn_read_queue_len = 0;
	stats->txn_rollback_to_stable = 0;
	stats->txn_rollback_upd_aborted = 0;
	stats->txn_rollback_las_removed = 0;
	stats->txn_set_ts = 0;
	stats->txn_set_ts_commit = 0;
	stats->txn_set_ts_commit_upd = 0;
	stats->txn_set_ts_oldest = 0;
	stats->txn_set_ts_oldest_upd = 0;
	stats->txn_set_ts_stable = 0;
	stats->txn_set_ts_stable_upd = 0;
	stats->txn_begin = 0;
		/* not clearing txn_checkpoint_running */
		/* not clearing txn_checkpoint_generation */
		/* not clearing txn_checkpoint_time_max */
		/* not clearing txn_checkpoint_time_min */
		/* not clearing txn_checkpoint_time_recent */
		/* not clearing txn_checkpoint_scrub_target */
		/* not clearing txn_checkpoint_scrub_time */
		/* not clearing txn_checkpoint_time_total */
	stats->txn_checkpoint = 0;
	stats->txn_checkpoint_skipped = 0;
	stats->txn_fail_cache = 0;
	stats->txn_checkpoint_fsync_post = 0;
		/* not clearing txn_checkpoint_fsync_post_duration */
		/* not clearing txn_pinned_range */
		/* not clearing txn_pinned_checkpoint_range */
		/* not clearing txn_pinned_snapshot_range */
		/* not clearing txn_pinned_timestamp */
		/* not clearing txn_pinned_timestamp_checkpoint */
		/* not clearing txn_pinned_timestamp_oldest */
	stats->txn_sync = 0;
	stats->txn_commit = 0;
	stats->txn_rollback = 0;
	stats->txn_update_conflict = 0;
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
	int64_t v;

	to->lsm_work_queue_app += WT_STAT_READ(from, lsm_work_queue_app);
	to->lsm_work_queue_manager +=
	    WT_STAT_READ(from, lsm_work_queue_manager);
	to->lsm_rows_merged += WT_STAT_READ(from, lsm_rows_merged);
	to->lsm_checkpoint_throttle +=
	    WT_STAT_READ(from, lsm_checkpoint_throttle);
	to->lsm_merge_throttle += WT_STAT_READ(from, lsm_merge_throttle);
	to->lsm_work_queue_switch +=
	    WT_STAT_READ(from, lsm_work_queue_switch);
	to->lsm_work_units_discarded +=
	    WT_STAT_READ(from, lsm_work_units_discarded);
	to->lsm_work_units_done += WT_STAT_READ(from, lsm_work_units_done);
	to->lsm_work_units_created +=
	    WT_STAT_READ(from, lsm_work_units_created);
	to->lsm_work_queue_max += WT_STAT_READ(from, lsm_work_queue_max);
	to->async_cur_queue += WT_STAT_READ(from, async_cur_queue);
	to->async_max_queue += WT_STAT_READ(from, async_max_queue);
	to->async_alloc_race += WT_STAT_READ(from, async_alloc_race);
	to->async_flush += WT_STAT_READ(from, async_flush);
	to->async_alloc_view += WT_STAT_READ(from, async_alloc_view);
	to->async_full += WT_STAT_READ(from, async_full);
	to->async_nowork += WT_STAT_READ(from, async_nowork);
	to->async_op_alloc += WT_STAT_READ(from, async_op_alloc);
	to->async_op_compact += WT_STAT_READ(from, async_op_compact);
	to->async_op_insert += WT_STAT_READ(from, async_op_insert);
	to->async_op_remove += WT_STAT_READ(from, async_op_remove);
	to->async_op_search += WT_STAT_READ(from, async_op_search);
	to->async_op_update += WT_STAT_READ(from, async_op_update);
	to->block_preload += WT_STAT_READ(from, block_preload);
	to->block_read += WT_STAT_READ(from, block_read);
	to->block_write += WT_STAT_READ(from, block_write);
	to->block_byte_read += WT_STAT_READ(from, block_byte_read);
	to->block_byte_write += WT_STAT_READ(from, block_byte_write);
	to->block_byte_write_checkpoint +=
	    WT_STAT_READ(from, block_byte_write_checkpoint);
	to->block_map_read += WT_STAT_READ(from, block_map_read);
	to->block_byte_map_read += WT_STAT_READ(from, block_byte_map_read);
	to->cache_read_app_count += WT_STAT_READ(from, cache_read_app_count);
	to->cache_read_app_time += WT_STAT_READ(from, cache_read_app_time);
	to->cache_write_app_count +=
	    WT_STAT_READ(from, cache_write_app_count);
	to->cache_write_app_time += WT_STAT_READ(from, cache_write_app_time);
	to->cache_bytes_image += WT_STAT_READ(from, cache_bytes_image);
	to->cache_bytes_lookaside +=
	    WT_STAT_READ(from, cache_bytes_lookaside);
	to->cache_bytes_inuse += WT_STAT_READ(from, cache_bytes_inuse);
	to->cache_bytes_other += WT_STAT_READ(from, cache_bytes_other);
	to->cache_bytes_read += WT_STAT_READ(from, cache_bytes_read);
	to->cache_bytes_write += WT_STAT_READ(from, cache_bytes_write);
	to->cache_lookaside_score +=
	    WT_STAT_READ(from, cache_lookaside_score);
	to->cache_lookaside_entries +=
	    WT_STAT_READ(from, cache_lookaside_entries);
	to->cache_lookaside_insert +=
	    WT_STAT_READ(from, cache_lookaside_insert);
	to->cache_lookaside_remove +=
	    WT_STAT_READ(from, cache_lookaside_remove);
	to->cache_eviction_checkpoint +=
	    WT_STAT_READ(from, cache_eviction_checkpoint);
	to->cache_eviction_get_ref +=
	    WT_STAT_READ(from, cache_eviction_get_ref);
	to->cache_eviction_get_ref_empty +=
	    WT_STAT_READ(from, cache_eviction_get_ref_empty);
	to->cache_eviction_get_ref_empty2 +=
	    WT_STAT_READ(from, cache_eviction_get_ref_empty2);
	to->cache_eviction_aggressive_set +=
	    WT_STAT_READ(from, cache_eviction_aggressive_set);
	to->cache_eviction_empty_score +=
	    WT_STAT_READ(from, cache_eviction_empty_score);
	to->cache_eviction_walk_passes +=
	    WT_STAT_READ(from, cache_eviction_walk_passes);
	to->cache_eviction_queue_empty +=
	    WT_STAT_READ(from, cache_eviction_queue_empty);
	to->cache_eviction_queue_not_empty +=
	    WT_STAT_READ(from, cache_eviction_queue_not_empty);
	to->cache_eviction_server_evicting +=
	    WT_STAT_READ(from, cache_eviction_server_evicting);
	to->cache_eviction_server_slept +=
	    WT_STAT_READ(from, cache_eviction_server_slept);
	to->cache_eviction_slow += WT_STAT_READ(from, cache_eviction_slow);
	to->cache_eviction_state += WT_STAT_READ(from, cache_eviction_state);
	to->cache_eviction_target_page_lt10 +=
	    WT_STAT_READ(from, cache_eviction_target_page_lt10);
	to->cache_eviction_target_page_lt32 +=
	    WT_STAT_READ(from, cache_eviction_target_page_lt32);
	to->cache_eviction_target_page_ge128 +=
	    WT_STAT_READ(from, cache_eviction_target_page_ge128);
	to->cache_eviction_target_page_lt64 +=
	    WT_STAT_READ(from, cache_eviction_target_page_lt64);
	to->cache_eviction_target_page_lt128 +=
	    WT_STAT_READ(from, cache_eviction_target_page_lt128);
	to->cache_eviction_walks_abandoned +=
	    WT_STAT_READ(from, cache_eviction_walks_abandoned);
	to->cache_eviction_walks_stopped +=
	    WT_STAT_READ(from, cache_eviction_walks_stopped);
	to->cache_eviction_walks_gave_up_no_targets +=
	    WT_STAT_READ(from, cache_eviction_walks_gave_up_no_targets);
	to->cache_eviction_walks_gave_up_ratio +=
	    WT_STAT_READ(from, cache_eviction_walks_gave_up_ratio);
	to->cache_eviction_walks_ended +=
	    WT_STAT_READ(from, cache_eviction_walks_ended);
	to->cache_eviction_walk_from_root +=
	    WT_STAT_READ(from, cache_eviction_walk_from_root);
	to->cache_eviction_walk_saved_pos +=
	    WT_STAT_READ(from, cache_eviction_walk_saved_pos);
	to->cache_eviction_active_workers +=
	    WT_STAT_READ(from, cache_eviction_active_workers);
	to->cache_eviction_worker_created +=
	    WT_STAT_READ(from, cache_eviction_worker_created);
	to->cache_eviction_worker_evicting +=
	    WT_STAT_READ(from, cache_eviction_worker_evicting);
	to->cache_eviction_worker_removed +=
	    WT_STAT_READ(from, cache_eviction_worker_removed);
	to->cache_eviction_stable_state_workers +=
	    WT_STAT_READ(from, cache_eviction_stable_state_workers);
	to->cache_eviction_force_fail +=
	    WT_STAT_READ(from, cache_eviction_force_fail);
	to->cache_eviction_force_fail_time +=
	    WT_STAT_READ(from, cache_eviction_force_fail_time);
	to->cache_eviction_walks_active +=
	    WT_STAT_READ(from, cache_eviction_walks_active);
	to->cache_eviction_walks_started +=
	    WT_STAT_READ(from, cache_eviction_walks_started);
	to->cache_eviction_force_retune +=
	    WT_STAT_READ(from, cache_eviction_force_retune);
	to->cache_eviction_hazard +=
	    WT_STAT_READ(from, cache_eviction_hazard);
	to->cache_hazard_checks += WT_STAT_READ(from, cache_hazard_checks);
	to->cache_hazard_walks += WT_STAT_READ(from, cache_hazard_walks);
	if ((v = WT_STAT_READ(from, cache_hazard_max)) > to->cache_hazard_max)
		to->cache_hazard_max = v;
	to->cache_inmem_splittable +=
	    WT_STAT_READ(from, cache_inmem_splittable);
	to->cache_inmem_split += WT_STAT_READ(from, cache_inmem_split);
	to->cache_eviction_internal +=
	    WT_STAT_READ(from, cache_eviction_internal);
	to->cache_eviction_split_internal +=
	    WT_STAT_READ(from, cache_eviction_split_internal);
	to->cache_eviction_split_leaf +=
	    WT_STAT_READ(from, cache_eviction_split_leaf);
	to->cache_bytes_max += WT_STAT_READ(from, cache_bytes_max);
	to->cache_eviction_maximum_page_size +=
	    WT_STAT_READ(from, cache_eviction_maximum_page_size);
	to->cache_eviction_dirty += WT_STAT_READ(from, cache_eviction_dirty);
	to->cache_eviction_app_dirty +=
	    WT_STAT_READ(from, cache_eviction_app_dirty);
	to->cache_timed_out_ops += WT_STAT_READ(from, cache_timed_out_ops);
	to->cache_read_overflow += WT_STAT_READ(from, cache_read_overflow);
	to->cache_eviction_deepen +=
	    WT_STAT_READ(from, cache_eviction_deepen);
	to->cache_write_lookaside +=
	    WT_STAT_READ(from, cache_write_lookaside);
	to->cache_pages_inuse += WT_STAT_READ(from, cache_pages_inuse);
	to->cache_eviction_force += WT_STAT_READ(from, cache_eviction_force);
	to->cache_eviction_force_time +=
	    WT_STAT_READ(from, cache_eviction_force_time);
	to->cache_eviction_force_delete +=
	    WT_STAT_READ(from, cache_eviction_force_delete);
	to->cache_eviction_force_delete_time +=
	    WT_STAT_READ(from, cache_eviction_force_delete_time);
	to->cache_eviction_app += WT_STAT_READ(from, cache_eviction_app);
	to->cache_eviction_pages_queued +=
	    WT_STAT_READ(from, cache_eviction_pages_queued);
	to->cache_eviction_pages_queued_urgent +=
	    WT_STAT_READ(from, cache_eviction_pages_queued_urgent);
	to->cache_eviction_pages_queued_oldest +=
	    WT_STAT_READ(from, cache_eviction_pages_queued_oldest);
	to->cache_read += WT_STAT_READ(from, cache_read);
	to->cache_read_deleted += WT_STAT_READ(from, cache_read_deleted);
	to->cache_read_deleted_prepared +=
	    WT_STAT_READ(from, cache_read_deleted_prepared);
	to->cache_read_lookaside += WT_STAT_READ(from, cache_read_lookaside);
	to->cache_read_lookaside_checkpoint +=
	    WT_STAT_READ(from, cache_read_lookaside_checkpoint);
	to->cache_read_lookaside_skipped +=
	    WT_STAT_READ(from, cache_read_lookaside_skipped);
	to->cache_read_lookaside_delay +=
	    WT_STAT_READ(from, cache_read_lookaside_delay);
	to->cache_read_lookaside_delay_checkpoint +=
	    WT_STAT_READ(from, cache_read_lookaside_delay_checkpoint);
	to->cache_pages_requested +=
	    WT_STAT_READ(from, cache_pages_requested);
	to->cache_eviction_pages_seen +=
	    WT_STAT_READ(from, cache_eviction_pages_seen);
	to->cache_eviction_fail += WT_STAT_READ(from, cache_eviction_fail);
	to->cache_eviction_walk += WT_STAT_READ(from, cache_eviction_walk);
	to->cache_write += WT_STAT_READ(from, cache_write);
	to->cache_write_restore += WT_STAT_READ(from, cache_write_restore);
	to->cache_overhead += WT_STAT_READ(from, cache_overhead);
	to->cache_bytes_internal += WT_STAT_READ(from, cache_bytes_internal);
	to->cache_bytes_leaf += WT_STAT_READ(from, cache_bytes_leaf);
	to->cache_bytes_dirty += WT_STAT_READ(from, cache_bytes_dirty);
	to->cache_pages_dirty += WT_STAT_READ(from, cache_pages_dirty);
	to->cache_eviction_clean += WT_STAT_READ(from, cache_eviction_clean);
	to->cond_auto_wait_reset += WT_STAT_READ(from, cond_auto_wait_reset);
	to->cond_auto_wait += WT_STAT_READ(from, cond_auto_wait);
	to->time_travel += WT_STAT_READ(from, time_travel);
	to->file_open += WT_STAT_READ(from, file_open);
	to->memory_allocation += WT_STAT_READ(from, memory_allocation);
	to->memory_free += WT_STAT_READ(from, memory_free);
	to->memory_grow += WT_STAT_READ(from, memory_grow);
	to->cond_wait += WT_STAT_READ(from, cond_wait);
	to->rwlock_read += WT_STAT_READ(from, rwlock_read);
	to->rwlock_write += WT_STAT_READ(from, rwlock_write);
	to->fsync_io += WT_STAT_READ(from, fsync_io);
	to->read_io += WT_STAT_READ(from, read_io);
	to->write_io += WT_STAT_READ(from, write_io);
	to->cursor_create += WT_STAT_READ(from, cursor_create);
	to->cursor_insert += WT_STAT_READ(from, cursor_insert);
	to->cursor_modify += WT_STAT_READ(from, cursor_modify);
	to->cursor_next += WT_STAT_READ(from, cursor_next);
	to->cursor_restart += WT_STAT_READ(from, cursor_restart);
	to->cursor_prev += WT_STAT_READ(from, cursor_prev);
	to->cursor_remove += WT_STAT_READ(from, cursor_remove);
	to->cursor_reserve += WT_STAT_READ(from, cursor_reserve);
	to->cursor_reset += WT_STAT_READ(from, cursor_reset);
	to->cursor_search += WT_STAT_READ(from, cursor_search);
	to->cursor_search_near += WT_STAT_READ(from, cursor_search_near);
	to->cursor_sweep_buckets += WT_STAT_READ(from, cursor_sweep_buckets);
	to->cursor_sweep_closed += WT_STAT_READ(from, cursor_sweep_closed);
	to->cursor_sweep_examined +=
	    WT_STAT_READ(from, cursor_sweep_examined);
	to->cursor_sweep += WT_STAT_READ(from, cursor_sweep);
	to->cursor_update += WT_STAT_READ(from, cursor_update);
	to->cursor_cache += WT_STAT_READ(from, cursor_cache);
	to->cursor_reopen += WT_STAT_READ(from, cursor_reopen);
	to->cursor_truncate += WT_STAT_READ(from, cursor_truncate);
	to->dh_conn_handle_count += WT_STAT_READ(from, dh_conn_handle_count);
	to->dh_sweep_ref += WT_STAT_READ(from, dh_sweep_ref);
	to->dh_sweep_close += WT_STAT_READ(from, dh_sweep_close);
	to->dh_sweep_remove += WT_STAT_READ(from, dh_sweep_remove);
	to->dh_sweep_tod += WT_STAT_READ(from, dh_sweep_tod);
	to->dh_sweeps += WT_STAT_READ(from, dh_sweeps);
	to->dh_session_handles += WT_STAT_READ(from, dh_session_handles);
	to->dh_session_sweeps += WT_STAT_READ(from, dh_session_sweeps);
	to->lock_checkpoint_count +=
	    WT_STAT_READ(from, lock_checkpoint_count);
	to->lock_checkpoint_wait_application +=
	    WT_STAT_READ(from, lock_checkpoint_wait_application);
	to->lock_checkpoint_wait_internal +=
	    WT_STAT_READ(from, lock_checkpoint_wait_internal);
	to->lock_commit_timestamp_wait_application +=
	    WT_STAT_READ(from, lock_commit_timestamp_wait_application);
	to->lock_commit_timestamp_wait_internal +=
	    WT_STAT_READ(from, lock_commit_timestamp_wait_internal);
	to->lock_commit_timestamp_read_count +=
	    WT_STAT_READ(from, lock_commit_timestamp_read_count);
	to->lock_commit_timestamp_write_count +=
	    WT_STAT_READ(from, lock_commit_timestamp_write_count);
	to->lock_dhandle_wait_application +=
	    WT_STAT_READ(from, lock_dhandle_wait_application);
	to->lock_dhandle_wait_internal +=
	    WT_STAT_READ(from, lock_dhandle_wait_internal);
	to->lock_dhandle_read_count +=
	    WT_STAT_READ(from, lock_dhandle_read_count);
	to->lock_dhandle_write_count +=
	    WT_STAT_READ(from, lock_dhandle_write_count);
	to->lock_metadata_count += WT_STAT_READ(from, lock_metadata_count);
	to->lock_metadata_wait_application +=
	    WT_STAT_READ(from, lock_metadata_wait_application);
	to->lock_metadata_wait_internal +=
	    WT_STAT_READ(from, lock_metadata_wait_internal);
	to->lock_read_timestamp_wait_application +=
	    WT_STAT_READ(from, lock_read_timestamp_wait_application);
	to->lock_read_timestamp_wait_internal +=
	    WT_STAT_READ(from, lock_read_timestamp_wait_internal);
	to->lock_read_timestamp_read_count +=
	    WT_STAT_READ(from, lock_read_timestamp_read_count);
	to->lock_read_timestamp_write_count +=
	    WT_STAT_READ(from, lock_read_timestamp_write_count);
	to->lock_schema_count += WT_STAT_READ(from, lock_schema_count);
	to->lock_schema_wait_application +=
	    WT_STAT_READ(from, lock_schema_wait_application);
	to->lock_schema_wait_internal +=
	    WT_STAT_READ(from, lock_schema_wait_internal);
	to->lock_table_wait_application +=
	    WT_STAT_READ(from, lock_table_wait_application);
	to->lock_table_wait_internal +=
	    WT_STAT_READ(from, lock_table_wait_internal);
	to->lock_table_read_count +=
	    WT_STAT_READ(from, lock_table_read_count);
	to->lock_table_write_count +=
	    WT_STAT_READ(from, lock_table_write_count);
	to->lock_txn_global_wait_application +=
	    WT_STAT_READ(from, lock_txn_global_wait_application);
	to->lock_txn_global_wait_internal +=
	    WT_STAT_READ(from, lock_txn_global_wait_internal);
	to->lock_txn_global_read_count +=
	    WT_STAT_READ(from, lock_txn_global_read_count);
	to->lock_txn_global_write_count +=
	    WT_STAT_READ(from, lock_txn_global_write_count);
	to->log_slot_switch_busy += WT_STAT_READ(from, log_slot_switch_busy);
	to->log_force_archive_sleep +=
	    WT_STAT_READ(from, log_force_archive_sleep);
	to->log_bytes_payload += WT_STAT_READ(from, log_bytes_payload);
	to->log_bytes_written += WT_STAT_READ(from, log_bytes_written);
	to->log_zero_fills += WT_STAT_READ(from, log_zero_fills);
	to->log_flush += WT_STAT_READ(from, log_flush);
	to->log_force_write += WT_STAT_READ(from, log_force_write);
	to->log_force_write_skip += WT_STAT_READ(from, log_force_write_skip);
	to->log_compress_writes += WT_STAT_READ(from, log_compress_writes);
	to->log_compress_write_fails +=
	    WT_STAT_READ(from, log_compress_write_fails);
	to->log_compress_small += WT_STAT_READ(from, log_compress_small);
	to->log_release_write_lsn +=
	    WT_STAT_READ(from, log_release_write_lsn);
	to->log_scans += WT_STAT_READ(from, log_scans);
	to->log_scan_rereads += WT_STAT_READ(from, log_scan_rereads);
	to->log_write_lsn += WT_STAT_READ(from, log_write_lsn);
	to->log_write_lsn_skip += WT_STAT_READ(from, log_write_lsn_skip);
	to->log_sync += WT_STAT_READ(from, log_sync);
	to->log_sync_duration += WT_STAT_READ(from, log_sync_duration);
	to->log_sync_dir += WT_STAT_READ(from, log_sync_dir);
	to->log_sync_dir_duration +=
	    WT_STAT_READ(from, log_sync_dir_duration);
	to->log_writes += WT_STAT_READ(from, log_writes);
	to->log_slot_consolidated +=
	    WT_STAT_READ(from, log_slot_consolidated);
	to->log_max_filesize += WT_STAT_READ(from, log_max_filesize);
	to->log_prealloc_max += WT_STAT_READ(from, log_prealloc_max);
	to->log_prealloc_missed += WT_STAT_READ(from, log_prealloc_missed);
	to->log_prealloc_files += WT_STAT_READ(from, log_prealloc_files);
	to->log_prealloc_used += WT_STAT_READ(from, log_prealloc_used);
	to->log_scan_records += WT_STAT_READ(from, log_scan_records);
	to->log_slot_close_race += WT_STAT_READ(from, log_slot_close_race);
	to->log_slot_close_unbuf += WT_STAT_READ(from, log_slot_close_unbuf);
	to->log_slot_closes += WT_STAT_READ(from, log_slot_closes);
	to->log_slot_races += WT_STAT_READ(from, log_slot_races);
	to->log_slot_yield_race += WT_STAT_READ(from, log_slot_yield_race);
	to->log_slot_immediate += WT_STAT_READ(from, log_slot_immediate);
	to->log_slot_yield_close += WT_STAT_READ(from, log_slot_yield_close);
	to->log_slot_yield_sleep += WT_STAT_READ(from, log_slot_yield_sleep);
	to->log_slot_yield += WT_STAT_READ(from, log_slot_yield);
	to->log_slot_active_closed +=
	    WT_STAT_READ(from, log_slot_active_closed);
	to->log_slot_yield_duration +=
	    WT_STAT_READ(from, log_slot_yield_duration);
	to->log_slot_no_free_slots +=
	    WT_STAT_READ(from, log_slot_no_free_slots);
	to->log_slot_unbuffered += WT_STAT_READ(from, log_slot_unbuffered);
	to->log_compress_mem += WT_STAT_READ(from, log_compress_mem);
	to->log_buffer_size += WT_STAT_READ(from, log_buffer_size);
	to->log_compress_len += WT_STAT_READ(from, log_compress_len);
	to->log_slot_coalesced += WT_STAT_READ(from, log_slot_coalesced);
	to->log_close_yields += WT_STAT_READ(from, log_close_yields);
	to->perf_hist_fsread_latency_lt50 +=
	    WT_STAT_READ(from, perf_hist_fsread_latency_lt50);
	to->perf_hist_fsread_latency_lt100 +=
	    WT_STAT_READ(from, perf_hist_fsread_latency_lt100);
	to->perf_hist_fsread_latency_lt250 +=
	    WT_STAT_READ(from, perf_hist_fsread_latency_lt250);
	to->perf_hist_fsread_latency_lt500 +=
	    WT_STAT_READ(from, perf_hist_fsread_latency_lt500);
	to->perf_hist_fsread_latency_lt1000 +=
	    WT_STAT_READ(from, perf_hist_fsread_latency_lt1000);
	to->perf_hist_fsread_latency_gt1000 +=
	    WT_STAT_READ(from, perf_hist_fsread_latency_gt1000);
	to->perf_hist_fswrite_latency_lt50 +=
	    WT_STAT_READ(from, perf_hist_fswrite_latency_lt50);
	to->perf_hist_fswrite_latency_lt100 +=
	    WT_STAT_READ(from, perf_hist_fswrite_latency_lt100);
	to->perf_hist_fswrite_latency_lt250 +=
	    WT_STAT_READ(from, perf_hist_fswrite_latency_lt250);
	to->perf_hist_fswrite_latency_lt500 +=
	    WT_STAT_READ(from, perf_hist_fswrite_latency_lt500);
	to->perf_hist_fswrite_latency_lt1000 +=
	    WT_STAT_READ(from, perf_hist_fswrite_latency_lt1000);
	to->perf_hist_fswrite_latency_gt1000 +=
	    WT_STAT_READ(from, perf_hist_fswrite_latency_gt1000);
	to->perf_hist_opread_latency_lt250 +=
	    WT_STAT_READ(from, perf_hist_opread_latency_lt250);
	to->perf_hist_opread_latency_lt500 +=
	    WT_STAT_READ(from, perf_hist_opread_latency_lt500);
	to->perf_hist_opread_latency_lt1000 +=
	    WT_STAT_READ(from, perf_hist_opread_latency_lt1000);
	to->perf_hist_opread_latency_lt10000 +=
	    WT_STAT_READ(from, perf_hist_opread_latency_lt10000);
	to->perf_hist_opread_latency_gt10000 +=
	    WT_STAT_READ(from, perf_hist_opread_latency_gt10000);
	to->perf_hist_opwrite_latency_lt250 +=
	    WT_STAT_READ(from, perf_hist_opwrite_latency_lt250);
	to->perf_hist_opwrite_latency_lt500 +=
	    WT_STAT_READ(from, perf_hist_opwrite_latency_lt500);
	to->perf_hist_opwrite_latency_lt1000 +=
	    WT_STAT_READ(from, perf_hist_opwrite_latency_lt1000);
	to->perf_hist_opwrite_latency_lt10000 +=
	    WT_STAT_READ(from, perf_hist_opwrite_latency_lt10000);
	to->perf_hist_opwrite_latency_gt10000 +=
	    WT_STAT_READ(from, perf_hist_opwrite_latency_gt10000);
	to->rec_page_delete_fast += WT_STAT_READ(from, rec_page_delete_fast);
	to->rec_pages += WT_STAT_READ(from, rec_pages);
	to->rec_pages_eviction += WT_STAT_READ(from, rec_pages_eviction);
	to->rec_page_delete += WT_STAT_READ(from, rec_page_delete);
	to->rec_split_stashed_bytes +=
	    WT_STAT_READ(from, rec_split_stashed_bytes);
	to->rec_split_stashed_objects +=
	    WT_STAT_READ(from, rec_split_stashed_objects);
	to->session_cursor_open += WT_STAT_READ(from, session_cursor_open);
	to->session_open += WT_STAT_READ(from, session_open);
	to->session_query_ts += WT_STAT_READ(from, session_query_ts);
	to->session_table_alter_fail +=
	    WT_STAT_READ(from, session_table_alter_fail);
	to->session_table_alter_success +=
	    WT_STAT_READ(from, session_table_alter_success);
	to->session_table_alter_skip +=
	    WT_STAT_READ(from, session_table_alter_skip);
	to->session_table_compact_fail +=
	    WT_STAT_READ(from, session_table_compact_fail);
	to->session_table_compact_success +=
	    WT_STAT_READ(from, session_table_compact_success);
	to->session_table_create_fail +=
	    WT_STAT_READ(from, session_table_create_fail);
	to->session_table_create_success +=
	    WT_STAT_READ(from, session_table_create_success);
	to->session_table_drop_fail +=
	    WT_STAT_READ(from, session_table_drop_fail);
	to->session_table_drop_success +=
	    WT_STAT_READ(from, session_table_drop_success);
	to->session_table_rebalance_fail +=
	    WT_STAT_READ(from, session_table_rebalance_fail);
	to->session_table_rebalance_success +=
	    WT_STAT_READ(from, session_table_rebalance_success);
	to->session_table_rename_fail +=
	    WT_STAT_READ(from, session_table_rename_fail);
	to->session_table_rename_success +=
	    WT_STAT_READ(from, session_table_rename_success);
	to->session_table_salvage_fail +=
	    WT_STAT_READ(from, session_table_salvage_fail);
	to->session_table_salvage_success +=
	    WT_STAT_READ(from, session_table_salvage_success);
	to->session_table_truncate_fail +=
	    WT_STAT_READ(from, session_table_truncate_fail);
	to->session_table_truncate_success +=
	    WT_STAT_READ(from, session_table_truncate_success);
	to->session_table_verify_fail +=
	    WT_STAT_READ(from, session_table_verify_fail);
	to->session_table_verify_success +=
	    WT_STAT_READ(from, session_table_verify_success);
	to->thread_fsync_active += WT_STAT_READ(from, thread_fsync_active);
	to->thread_read_active += WT_STAT_READ(from, thread_read_active);
	to->thread_write_active += WT_STAT_READ(from, thread_write_active);
	to->application_evict_time +=
	    WT_STAT_READ(from, application_evict_time);
	to->application_cache_time +=
	    WT_STAT_READ(from, application_cache_time);
	to->txn_release_blocked += WT_STAT_READ(from, txn_release_blocked);
	to->conn_close_blocked_lsm +=
	    WT_STAT_READ(from, conn_close_blocked_lsm);
	to->dhandle_lock_blocked += WT_STAT_READ(from, dhandle_lock_blocked);
	to->page_index_slot_ref_blocked +=
	    WT_STAT_READ(from, page_index_slot_ref_blocked);
	to->log_server_sync_blocked +=
	    WT_STAT_READ(from, log_server_sync_blocked);
	to->prepared_transition_blocked_page +=
	    WT_STAT_READ(from, prepared_transition_blocked_page);
	to->page_busy_blocked += WT_STAT_READ(from, page_busy_blocked);
	to->page_forcible_evict_blocked +=
	    WT_STAT_READ(from, page_forcible_evict_blocked);
	to->page_locked_blocked += WT_STAT_READ(from, page_locked_blocked);
	to->page_read_blocked += WT_STAT_READ(from, page_read_blocked);
	to->page_sleep += WT_STAT_READ(from, page_sleep);
	to->page_del_rollback_blocked +=
	    WT_STAT_READ(from, page_del_rollback_blocked);
	to->child_modify_blocked_page +=
	    WT_STAT_READ(from, child_modify_blocked_page);
	to->txn_commit_queue_walked +=
	    WT_STAT_READ(from, txn_commit_queue_walked);
	to->txn_commit_queue_empty +=
	    WT_STAT_READ(from, txn_commit_queue_empty);
	to->txn_commit_queue_head +=
	    WT_STAT_READ(from, txn_commit_queue_head);
	to->txn_commit_queue_inserts +=
	    WT_STAT_READ(from, txn_commit_queue_inserts);
	to->txn_commit_queue_len += WT_STAT_READ(from, txn_commit_queue_len);
	to->txn_snapshots_created +=
	    WT_STAT_READ(from, txn_snapshots_created);
	to->txn_snapshots_dropped +=
	    WT_STAT_READ(from, txn_snapshots_dropped);
	to->txn_prepare += WT_STAT_READ(from, txn_prepare);
	to->txn_prepare_commit += WT_STAT_READ(from, txn_prepare_commit);
	to->txn_prepare_active += WT_STAT_READ(from, txn_prepare_active);
	to->txn_prepare_rollback += WT_STAT_READ(from, txn_prepare_rollback);
	to->txn_query_ts += WT_STAT_READ(from, txn_query_ts);
	to->txn_read_queue_walked +=
	    WT_STAT_READ(from, txn_read_queue_walked);
	to->txn_read_queue_empty += WT_STAT_READ(from, txn_read_queue_empty);
	to->txn_read_queue_head += WT_STAT_READ(from, txn_read_queue_head);
	to->txn_read_queue_inserts +=
	    WT_STAT_READ(from, txn_read_queue_inserts);
	to->txn_read_queue_len += WT_STAT_READ(from, txn_read_queue_len);
	to->txn_rollback_to_stable +=
	    WT_STAT_READ(from, txn_rollback_to_stable);
	to->txn_rollback_upd_aborted +=
	    WT_STAT_READ(from, txn_rollback_upd_aborted);
	to->txn_rollback_las_removed +=
	    WT_STAT_READ(from, txn_rollback_las_removed);
	to->txn_set_ts += WT_STAT_READ(from, txn_set_ts);
	to->txn_set_ts_commit += WT_STAT_READ(from, txn_set_ts_commit);
	to->txn_set_ts_commit_upd +=
	    WT_STAT_READ(from, txn_set_ts_commit_upd);
	to->txn_set_ts_oldest += WT_STAT_READ(from, txn_set_ts_oldest);
	to->txn_set_ts_oldest_upd +=
	    WT_STAT_READ(from, txn_set_ts_oldest_upd);
	to->txn_set_ts_stable += WT_STAT_READ(from, txn_set_ts_stable);
	to->txn_set_ts_stable_upd +=
	    WT_STAT_READ(from, txn_set_ts_stable_upd);
	to->txn_begin += WT_STAT_READ(from, txn_begin);
	to->txn_checkpoint_running +=
	    WT_STAT_READ(from, txn_checkpoint_running);
	to->txn_checkpoint_generation +=
	    WT_STAT_READ(from, txn_checkpoint_generation);
	to->txn_checkpoint_time_max +=
	    WT_STAT_READ(from, txn_checkpoint_time_max);
	to->txn_checkpoint_time_min +=
	    WT_STAT_READ(from, txn_checkpoint_time_min);
	to->txn_checkpoint_time_recent +=
	    WT_STAT_READ(from, txn_checkpoint_time_recent);
	to->txn_checkpoint_scrub_target +=
	    WT_STAT_READ(from, txn_checkpoint_scrub_target);
	to->txn_checkpoint_scrub_time +=
	    WT_STAT_READ(from, txn_checkpoint_scrub_time);
	to->txn_checkpoint_time_total +=
	    WT_STAT_READ(from, txn_checkpoint_time_total);
	to->txn_checkpoint += WT_STAT_READ(from, txn_checkpoint);
	to->txn_checkpoint_skipped +=
	    WT_STAT_READ(from, txn_checkpoint_skipped);
	to->txn_fail_cache += WT_STAT_READ(from, txn_fail_cache);
	to->txn_checkpoint_fsync_post +=
	    WT_STAT_READ(from, txn_checkpoint_fsync_post);
	to->txn_checkpoint_fsync_post_duration +=
	    WT_STAT_READ(from, txn_checkpoint_fsync_post_duration);
	to->txn_pinned_range += WT_STAT_READ(from, txn_pinned_range);
	to->txn_pinned_checkpoint_range +=
	    WT_STAT_READ(from, txn_pinned_checkpoint_range);
	to->txn_pinned_snapshot_range +=
	    WT_STAT_READ(from, txn_pinned_snapshot_range);
	to->txn_pinned_timestamp += WT_STAT_READ(from, txn_pinned_timestamp);
	to->txn_pinned_timestamp_checkpoint +=
	    WT_STAT_READ(from, txn_pinned_timestamp_checkpoint);
	to->txn_pinned_timestamp_oldest +=
	    WT_STAT_READ(from, txn_pinned_timestamp_oldest);
	to->txn_sync += WT_STAT_READ(from, txn_sync);
	to->txn_commit += WT_STAT_READ(from, txn_commit);
	to->txn_rollback += WT_STAT_READ(from, txn_rollback);
	to->txn_update_conflict += WT_STAT_READ(from, txn_update_conflict);
}

static const char * const __stats_join_desc[] = {
	": accesses to the main table",
	": bloom filter false positives",
	": checks that conditions of membership are satisfied",
	": items inserted into a bloom filter",
	": items iterated",
};

int
__wt_stat_join_desc(WT_CURSOR_STAT *cst, int slot, const char **p)
{
	WT_UNUSED(cst);
	*p = __stats_join_desc[slot];
	return (0);
}

void
__wt_stat_join_init_single(WT_JOIN_STATS *stats)
{
	memset(stats, 0, sizeof(*stats));
}

void
__wt_stat_join_clear_single(WT_JOIN_STATS *stats)
{
	stats->main_access = 0;
	stats->bloom_false_positive = 0;
	stats->membership_check = 0;
	stats->bloom_insert = 0;
	stats->iterated = 0;
}

void
__wt_stat_join_clear_all(WT_JOIN_STATS **stats)
{
	u_int i;

	for (i = 0; i < WT_COUNTER_SLOTS; ++i)
		__wt_stat_join_clear_single(stats[i]);
}

void
__wt_stat_join_aggregate(
    WT_JOIN_STATS **from, WT_JOIN_STATS *to)
{
	to->main_access += WT_STAT_READ(from, main_access);
	to->bloom_false_positive += WT_STAT_READ(from, bloom_false_positive);
	to->membership_check += WT_STAT_READ(from, membership_check);
	to->bloom_insert += WT_STAT_READ(from, bloom_insert);
	to->iterated += WT_STAT_READ(from, iterated);
}
