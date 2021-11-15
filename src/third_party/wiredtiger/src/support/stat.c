/* DO NOT EDIT: automatically built by dist/stat.py. */

#include "wt_internal.h"

static const char *const __stats_dsrc_desc[] = {
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
  "btree: btree clean tree checkpoint expiration time",
  "btree: btree compact pages reviewed",
  "btree: btree compact pages rewritten",
  "btree: btree compact pages skipped",
  "btree: btree skipped by compaction as process would not reduce size",
  "btree: column-store fixed-size leaf pages",
  "btree: column-store fixed-size time windows",
  "btree: column-store internal pages",
  "btree: column-store variable-size RLE encoded values",
  "btree: column-store variable-size deleted values",
  "btree: column-store variable-size leaf pages",
  "btree: fixed-record size",
  "btree: maximum internal page size",
  "btree: maximum leaf page key size",
  "btree: maximum leaf page size",
  "btree: maximum leaf page value size",
  "btree: maximum tree depth",
  "btree: number of key/value pairs",
  "btree: overflow pages",
  "btree: row-store empty values",
  "btree: row-store internal pages",
  "btree: row-store leaf pages",
  "cache: bytes currently in the cache",
  "cache: bytes dirty in the cache cumulative",
  "cache: bytes read into cache",
  "cache: bytes written from cache",
  "cache: checkpoint blocked page eviction",
  "cache: checkpoint of history store file blocked non-history store page eviction",
  "cache: data source pages selected for eviction unable to be evicted",
  "cache: eviction gave up due to detecting an out of order on disk value behind the last update "
  "on the chain",
  "cache: eviction gave up due to detecting an out of order tombstone ahead of the selected on "
  "disk update",
  "cache: eviction gave up due to detecting an out of order tombstone ahead of the selected on "
  "disk update after validating the update chain",
  "cache: eviction gave up due to detecting out of order timestamps on the update chain after the "
  "selected on disk update",
  "cache: eviction walk passes of a file",
  "cache: eviction walk target pages histogram - 0-9",
  "cache: eviction walk target pages histogram - 10-31",
  "cache: eviction walk target pages histogram - 128 and higher",
  "cache: eviction walk target pages histogram - 32-63",
  "cache: eviction walk target pages histogram - 64-128",
  "cache: eviction walk target pages reduced due to history store cache pressure",
  "cache: eviction walks abandoned",
  "cache: eviction walks gave up because they restarted their walk twice",
  "cache: eviction walks gave up because they saw too many pages and found no candidates",
  "cache: eviction walks gave up because they saw too many pages and found too few candidates",
  "cache: eviction walks reached end of tree",
  "cache: eviction walks restarted",
  "cache: eviction walks started from root of tree",
  "cache: eviction walks started from saved location in tree",
  "cache: hazard pointer blocked page eviction",
  "cache: history store table insert calls",
  "cache: history store table insert calls that returned restart",
  "cache: history store table out-of-order resolved updates that lose their durable timestamp",
  "cache: history store table out-of-order updates that were fixed up by reinserting with the "
  "fixed timestamp",
  "cache: history store table reads",
  "cache: history store table reads missed",
  "cache: history store table reads requiring squashed modifies",
  "cache: history store table truncation by rollback to stable to remove an unstable update",
  "cache: history store table truncation by rollback to stable to remove an update",
  "cache: history store table truncation to remove an update",
  "cache: history store table truncation to remove range of updates due to key being removed from "
  "the data page during reconciliation",
  "cache: history store table truncation to remove range of updates due to out-of-order timestamp "
  "update on data page",
  "cache: history store table writes requiring squashed modifies",
  "cache: in-memory page passed criteria to be split",
  "cache: in-memory page splits",
  "cache: internal pages evicted",
  "cache: internal pages split during eviction",
  "cache: leaf pages split during eviction",
  "cache: modified pages evicted",
  "cache: overflow pages read into cache",
  "cache: page split during eviction deepened the tree",
  "cache: page written requiring history store records",
  "cache: pages read into cache",
  "cache: pages read into cache after truncate",
  "cache: pages read into cache after truncate in prepare state",
  "cache: pages requested from the cache",
  "cache: pages seen by eviction walk",
  "cache: pages written from cache",
  "cache: pages written requiring in-memory restoration",
  "cache: the number of times full update inserted to history store",
  "cache: the number of times reverse modify inserted to history store",
  "cache: tracked dirty bytes in the cache",
  "cache: unmodified pages evicted",
  "cache_walk: Average difference between current eviction generation when the page was last "
  "considered",
  "cache_walk: Average on-disk page image size seen",
  "cache_walk: Average time in cache for pages that have been visited by the eviction server",
  "cache_walk: Average time in cache for pages that have not been visited by the eviction server",
  "cache_walk: Clean pages currently in cache",
  "cache_walk: Current eviction generation",
  "cache_walk: Dirty pages currently in cache",
  "cache_walk: Entries in the root page",
  "cache_walk: Internal pages currently in cache",
  "cache_walk: Leaf pages currently in cache",
  "cache_walk: Maximum difference between current eviction generation when the page was last "
  "considered",
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
  "checkpoint-cleanup: pages added for eviction",
  "checkpoint-cleanup: pages removed",
  "checkpoint-cleanup: pages skipped during tree walk",
  "checkpoint-cleanup: pages visited",
  "compression: compressed page maximum internal page size prior to compression",
  "compression: compressed page maximum leaf page size prior to compression ",
  "compression: compressed pages read",
  "compression: compressed pages written",
  "compression: number of blocks with compress ratio greater than 64",
  "compression: number of blocks with compress ratio smaller than 16",
  "compression: number of blocks with compress ratio smaller than 2",
  "compression: number of blocks with compress ratio smaller than 32",
  "compression: number of blocks with compress ratio smaller than 4",
  "compression: number of blocks with compress ratio smaller than 64",
  "compression: number of blocks with compress ratio smaller than 8",
  "compression: page written failed to compress",
  "compression: page written was too small to compress",
  "cursor: Total number of entries skipped by cursor next calls",
  "cursor: Total number of entries skipped by cursor prev calls",
  "cursor: Total number of entries skipped to position the history store cursor",
  "cursor: Total number of times a search near has exited due to prefix config",
  "cursor: bulk loaded cursor insert calls",
  "cursor: cache cursors reuse count",
  "cursor: close calls that result in cache",
  "cursor: create calls",
  "cursor: cursor next calls that skip due to a globally visible history store tombstone",
  "cursor: cursor next calls that skip greater than or equal to 100 entries",
  "cursor: cursor next calls that skip less than 100 entries",
  "cursor: cursor prev calls that skip due to a globally visible history store tombstone",
  "cursor: cursor prev calls that skip greater than or equal to 100 entries",
  "cursor: cursor prev calls that skip less than 100 entries",
  "cursor: insert calls",
  "cursor: insert key and value bytes",
  "cursor: modify",
  "cursor: modify key and value bytes affected",
  "cursor: modify value bytes modified",
  "cursor: next calls",
  "cursor: open cursor count",
  "cursor: operation restarted",
  "cursor: prev calls",
  "cursor: remove calls",
  "cursor: remove key bytes removed",
  "cursor: reserve calls",
  "cursor: reset calls",
  "cursor: search calls",
  "cursor: search history store calls",
  "cursor: search near calls",
  "cursor: truncate calls",
  "cursor: update calls",
  "cursor: update key and value bytes",
  "cursor: update value size change",
  "reconciliation: approximate byte size of timestamps in pages written",
  "reconciliation: approximate byte size of transaction IDs in pages written",
  "reconciliation: dictionary matches",
  "reconciliation: fast-path pages deleted",
  "reconciliation: internal page key bytes discarded using suffix compression",
  "reconciliation: internal page multi-block writes",
  "reconciliation: leaf page key bytes discarded using prefix compression",
  "reconciliation: leaf page multi-block writes",
  "reconciliation: leaf-page overflow keys",
  "reconciliation: maximum blocks required for a page",
  "reconciliation: overflow values written",
  "reconciliation: page checksum matches",
  "reconciliation: page reconciliation calls",
  "reconciliation: page reconciliation calls for eviction",
  "reconciliation: pages deleted",
  "reconciliation: pages written including an aggregated newest start durable timestamp ",
  "reconciliation: pages written including an aggregated newest stop durable timestamp ",
  "reconciliation: pages written including an aggregated newest stop timestamp ",
  "reconciliation: pages written including an aggregated newest stop transaction ID",
  "reconciliation: pages written including an aggregated newest transaction ID ",
  "reconciliation: pages written including an aggregated oldest start timestamp ",
  "reconciliation: pages written including an aggregated prepare",
  "reconciliation: pages written including at least one prepare",
  "reconciliation: pages written including at least one start durable timestamp",
  "reconciliation: pages written including at least one start timestamp",
  "reconciliation: pages written including at least one start transaction ID",
  "reconciliation: pages written including at least one stop durable timestamp",
  "reconciliation: pages written including at least one stop timestamp",
  "reconciliation: pages written including at least one stop transaction ID",
  "reconciliation: records written including a prepare",
  "reconciliation: records written including a start durable timestamp",
  "reconciliation: records written including a start timestamp",
  "reconciliation: records written including a start transaction ID",
  "reconciliation: records written including a stop durable timestamp",
  "reconciliation: records written including a stop timestamp",
  "reconciliation: records written including a stop transaction ID",
  "session: object compaction",
  "session: tiered operations dequeued and processed",
  "session: tiered operations scheduled",
  "session: tiered storage local retention time (secs)",
  "session: tiered storage object size",
  "transaction: race to read prepared update retry",
  "transaction: rollback to stable history store records with stop timestamps older than newer "
  "records",
  "transaction: rollback to stable inconsistent checkpoint",
  "transaction: rollback to stable keys removed",
  "transaction: rollback to stable keys restored",
  "transaction: rollback to stable restored tombstones from history store",
  "transaction: rollback to stable restored updates from history store",
  "transaction: rollback to stable skipping delete rle",
  "transaction: rollback to stable skipping stable rle",
  "transaction: rollback to stable sweeping history store keys",
  "transaction: rollback to stable updates removed from history store",
  "transaction: transaction checkpoints due to obsolete pages",
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
__wt_stat_dsrc_init(WT_SESSION_IMPL *session, WT_DATA_HANDLE *handle)
{
    int i;

    WT_RET(__wt_calloc(
      session, (size_t)WT_COUNTER_SLOTS, sizeof(*handle->stat_array), &handle->stat_array));

    for (i = 0; i < WT_COUNTER_SLOTS; ++i) {
        handle->stats[i] = &handle->stat_array[i];
        __wt_stat_dsrc_init_single(handle->stats[i]);
    }
    return (0);
}

void
__wt_stat_dsrc_discard(WT_SESSION_IMPL *session, WT_DATA_HANDLE *handle)
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
    /* not clearing btree_clean_checkpoint_timer */
    /* not clearing btree_compact_pages_reviewed */
    /* not clearing btree_compact_pages_rewritten */
    /* not clearing btree_compact_pages_skipped */
    /* not clearing btree_compact_skipped */
    stats->btree_column_fix = 0;
    stats->btree_column_tws = 0;
    stats->btree_column_internal = 0;
    stats->btree_column_rle = 0;
    stats->btree_column_deleted = 0;
    stats->btree_column_variable = 0;
    stats->btree_fixed_len = 0;
    stats->btree_maxintlpage = 0;
    stats->btree_maxleafkey = 0;
    stats->btree_maxleafpage = 0;
    stats->btree_maxleafvalue = 0;
    stats->btree_maximum_depth = 0;
    stats->btree_entries = 0;
    stats->btree_overflow = 0;
    stats->btree_row_empty_values = 0;
    stats->btree_row_internal = 0;
    stats->btree_row_leaf = 0;
    /* not clearing cache_bytes_inuse */
    /* not clearing cache_bytes_dirty_total */
    stats->cache_bytes_read = 0;
    stats->cache_bytes_write = 0;
    stats->cache_eviction_checkpoint = 0;
    stats->cache_eviction_blocked_checkpoint_hs = 0;
    stats->cache_eviction_fail = 0;
    stats->cache_eviction_blocked_ooo_checkpoint_race_1 = 0;
    stats->cache_eviction_blocked_ooo_checkpoint_race_2 = 0;
    stats->cache_eviction_blocked_ooo_checkpoint_race_3 = 0;
    stats->cache_eviction_blocked_ooo_checkpoint_race_4 = 0;
    stats->cache_eviction_walk_passes = 0;
    stats->cache_eviction_target_page_lt10 = 0;
    stats->cache_eviction_target_page_lt32 = 0;
    stats->cache_eviction_target_page_ge128 = 0;
    stats->cache_eviction_target_page_lt64 = 0;
    stats->cache_eviction_target_page_lt128 = 0;
    stats->cache_eviction_target_page_reduced = 0;
    stats->cache_eviction_walks_abandoned = 0;
    stats->cache_eviction_walks_stopped = 0;
    stats->cache_eviction_walks_gave_up_no_targets = 0;
    stats->cache_eviction_walks_gave_up_ratio = 0;
    stats->cache_eviction_walks_ended = 0;
    stats->cache_eviction_walk_restart = 0;
    stats->cache_eviction_walk_from_root = 0;
    stats->cache_eviction_walk_saved_pos = 0;
    stats->cache_eviction_hazard = 0;
    stats->cache_hs_insert = 0;
    stats->cache_hs_insert_restart = 0;
    stats->cache_hs_order_lose_durable_timestamp = 0;
    stats->cache_hs_order_reinsert = 0;
    stats->cache_hs_read = 0;
    stats->cache_hs_read_miss = 0;
    stats->cache_hs_read_squash = 0;
    stats->cache_hs_key_truncate_rts_unstable = 0;
    stats->cache_hs_key_truncate_rts = 0;
    stats->cache_hs_key_truncate = 0;
    stats->cache_hs_key_truncate_onpage_removal = 0;
    stats->cache_hs_order_remove = 0;
    stats->cache_hs_write_squash = 0;
    stats->cache_inmem_splittable = 0;
    stats->cache_inmem_split = 0;
    stats->cache_eviction_internal = 0;
    stats->cache_eviction_split_internal = 0;
    stats->cache_eviction_split_leaf = 0;
    stats->cache_eviction_dirty = 0;
    stats->cache_read_overflow = 0;
    stats->cache_eviction_deepen = 0;
    stats->cache_write_hs = 0;
    stats->cache_read = 0;
    stats->cache_read_deleted = 0;
    stats->cache_read_deleted_prepared = 0;
    stats->cache_pages_requested = 0;
    stats->cache_eviction_pages_seen = 0;
    stats->cache_write = 0;
    stats->cache_write_restore = 0;
    stats->cache_hs_insert_full_update = 0;
    stats->cache_hs_insert_reverse_modify = 0;
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
    stats->cc_pages_evict = 0;
    stats->cc_pages_removed = 0;
    stats->cc_pages_walk_skipped = 0;
    stats->cc_pages_visited = 0;
    /* not clearing compress_precomp_intl_max_page_size */
    /* not clearing compress_precomp_leaf_max_page_size */
    stats->compress_read = 0;
    stats->compress_write = 0;
    stats->compress_hist_ratio_max = 0;
    stats->compress_hist_ratio_16 = 0;
    stats->compress_hist_ratio_2 = 0;
    stats->compress_hist_ratio_32 = 0;
    stats->compress_hist_ratio_4 = 0;
    stats->compress_hist_ratio_64 = 0;
    stats->compress_hist_ratio_8 = 0;
    stats->compress_write_fail = 0;
    stats->compress_write_too_small = 0;
    stats->cursor_next_skip_total = 0;
    stats->cursor_prev_skip_total = 0;
    stats->cursor_skip_hs_cur_position = 0;
    stats->cursor_search_near_prefix_fast_paths = 0;
    stats->cursor_insert_bulk = 0;
    stats->cursor_reopen = 0;
    stats->cursor_cache = 0;
    stats->cursor_create = 0;
    stats->cursor_next_hs_tombstone = 0;
    stats->cursor_next_skip_ge_100 = 0;
    stats->cursor_next_skip_lt_100 = 0;
    stats->cursor_prev_hs_tombstone = 0;
    stats->cursor_prev_skip_ge_100 = 0;
    stats->cursor_prev_skip_lt_100 = 0;
    stats->cursor_insert = 0;
    stats->cursor_insert_bytes = 0;
    stats->cursor_modify = 0;
    stats->cursor_modify_bytes = 0;
    stats->cursor_modify_bytes_touch = 0;
    stats->cursor_next = 0;
    /* not clearing cursor_open_count */
    stats->cursor_restart = 0;
    stats->cursor_prev = 0;
    stats->cursor_remove = 0;
    stats->cursor_remove_bytes = 0;
    stats->cursor_reserve = 0;
    stats->cursor_reset = 0;
    stats->cursor_search = 0;
    stats->cursor_search_hs = 0;
    stats->cursor_search_near = 0;
    stats->cursor_truncate = 0;
    stats->cursor_update = 0;
    stats->cursor_update_bytes = 0;
    stats->cursor_update_bytes_changed = 0;
    stats->rec_time_window_bytes_ts = 0;
    stats->rec_time_window_bytes_txn = 0;
    stats->rec_dictionary = 0;
    stats->rec_page_delete_fast = 0;
    stats->rec_suffix_compression = 0;
    stats->rec_multiblock_internal = 0;
    stats->rec_prefix_compression = 0;
    stats->rec_multiblock_leaf = 0;
    stats->rec_overflow_key_leaf = 0;
    stats->rec_multiblock_max = 0;
    stats->rec_overflow_value = 0;
    stats->rec_page_match = 0;
    stats->rec_pages = 0;
    stats->rec_pages_eviction = 0;
    stats->rec_page_delete = 0;
    stats->rec_time_aggr_newest_start_durable_ts = 0;
    stats->rec_time_aggr_newest_stop_durable_ts = 0;
    stats->rec_time_aggr_newest_stop_ts = 0;
    stats->rec_time_aggr_newest_stop_txn = 0;
    stats->rec_time_aggr_newest_txn = 0;
    stats->rec_time_aggr_oldest_start_ts = 0;
    stats->rec_time_aggr_prepared = 0;
    stats->rec_time_window_pages_prepared = 0;
    stats->rec_time_window_pages_durable_start_ts = 0;
    stats->rec_time_window_pages_start_ts = 0;
    stats->rec_time_window_pages_start_txn = 0;
    stats->rec_time_window_pages_durable_stop_ts = 0;
    stats->rec_time_window_pages_stop_ts = 0;
    stats->rec_time_window_pages_stop_txn = 0;
    stats->rec_time_window_prepared = 0;
    stats->rec_time_window_durable_start_ts = 0;
    stats->rec_time_window_start_ts = 0;
    stats->rec_time_window_start_txn = 0;
    stats->rec_time_window_durable_stop_ts = 0;
    stats->rec_time_window_stop_ts = 0;
    stats->rec_time_window_stop_txn = 0;
    stats->session_compact = 0;
    stats->tiered_work_units_dequeued = 0;
    stats->tiered_work_units_created = 0;
    /* not clearing tiered_retention */
    /* not clearing tiered_object_size */
    stats->txn_read_race_prepare_update = 0;
    stats->txn_rts_hs_stop_older_than_newer_start = 0;
    stats->txn_rts_inconsistent_ckpt = 0;
    stats->txn_rts_keys_removed = 0;
    stats->txn_rts_keys_restored = 0;
    stats->txn_rts_hs_restore_tombstones = 0;
    stats->txn_rts_hs_restore_updates = 0;
    stats->txn_rts_delete_rle_skipped = 0;
    stats->txn_rts_stable_rle_skipped = 0;
    stats->txn_rts_sweep_hs_keys = 0;
    stats->txn_rts_hs_removed = 0;
    stats->txn_checkpoint_obsolete_applied = 0;
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
__wt_stat_dsrc_aggregate_single(WT_DSRC_STATS *from, WT_DSRC_STATS *to)
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
    to->btree_clean_checkpoint_timer += from->btree_clean_checkpoint_timer;
    to->btree_compact_pages_reviewed += from->btree_compact_pages_reviewed;
    to->btree_compact_pages_rewritten += from->btree_compact_pages_rewritten;
    to->btree_compact_pages_skipped += from->btree_compact_pages_skipped;
    to->btree_compact_skipped += from->btree_compact_skipped;
    to->btree_column_fix += from->btree_column_fix;
    to->btree_column_tws += from->btree_column_tws;
    to->btree_column_internal += from->btree_column_internal;
    to->btree_column_rle += from->btree_column_rle;
    to->btree_column_deleted += from->btree_column_deleted;
    to->btree_column_variable += from->btree_column_variable;
    if (from->btree_fixed_len > to->btree_fixed_len)
        to->btree_fixed_len = from->btree_fixed_len;
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
    to->btree_row_empty_values += from->btree_row_empty_values;
    to->btree_row_internal += from->btree_row_internal;
    to->btree_row_leaf += from->btree_row_leaf;
    to->cache_bytes_inuse += from->cache_bytes_inuse;
    to->cache_bytes_dirty_total += from->cache_bytes_dirty_total;
    to->cache_bytes_read += from->cache_bytes_read;
    to->cache_bytes_write += from->cache_bytes_write;
    to->cache_eviction_checkpoint += from->cache_eviction_checkpoint;
    to->cache_eviction_blocked_checkpoint_hs += from->cache_eviction_blocked_checkpoint_hs;
    to->cache_eviction_fail += from->cache_eviction_fail;
    to->cache_eviction_blocked_ooo_checkpoint_race_1 +=
      from->cache_eviction_blocked_ooo_checkpoint_race_1;
    to->cache_eviction_blocked_ooo_checkpoint_race_2 +=
      from->cache_eviction_blocked_ooo_checkpoint_race_2;
    to->cache_eviction_blocked_ooo_checkpoint_race_3 +=
      from->cache_eviction_blocked_ooo_checkpoint_race_3;
    to->cache_eviction_blocked_ooo_checkpoint_race_4 +=
      from->cache_eviction_blocked_ooo_checkpoint_race_4;
    to->cache_eviction_walk_passes += from->cache_eviction_walk_passes;
    to->cache_eviction_target_page_lt10 += from->cache_eviction_target_page_lt10;
    to->cache_eviction_target_page_lt32 += from->cache_eviction_target_page_lt32;
    to->cache_eviction_target_page_ge128 += from->cache_eviction_target_page_ge128;
    to->cache_eviction_target_page_lt64 += from->cache_eviction_target_page_lt64;
    to->cache_eviction_target_page_lt128 += from->cache_eviction_target_page_lt128;
    to->cache_eviction_target_page_reduced += from->cache_eviction_target_page_reduced;
    to->cache_eviction_walks_abandoned += from->cache_eviction_walks_abandoned;
    to->cache_eviction_walks_stopped += from->cache_eviction_walks_stopped;
    to->cache_eviction_walks_gave_up_no_targets += from->cache_eviction_walks_gave_up_no_targets;
    to->cache_eviction_walks_gave_up_ratio += from->cache_eviction_walks_gave_up_ratio;
    to->cache_eviction_walks_ended += from->cache_eviction_walks_ended;
    to->cache_eviction_walk_restart += from->cache_eviction_walk_restart;
    to->cache_eviction_walk_from_root += from->cache_eviction_walk_from_root;
    to->cache_eviction_walk_saved_pos += from->cache_eviction_walk_saved_pos;
    to->cache_eviction_hazard += from->cache_eviction_hazard;
    to->cache_hs_insert += from->cache_hs_insert;
    to->cache_hs_insert_restart += from->cache_hs_insert_restart;
    to->cache_hs_order_lose_durable_timestamp += from->cache_hs_order_lose_durable_timestamp;
    to->cache_hs_order_reinsert += from->cache_hs_order_reinsert;
    to->cache_hs_read += from->cache_hs_read;
    to->cache_hs_read_miss += from->cache_hs_read_miss;
    to->cache_hs_read_squash += from->cache_hs_read_squash;
    to->cache_hs_key_truncate_rts_unstable += from->cache_hs_key_truncate_rts_unstable;
    to->cache_hs_key_truncate_rts += from->cache_hs_key_truncate_rts;
    to->cache_hs_key_truncate += from->cache_hs_key_truncate;
    to->cache_hs_key_truncate_onpage_removal += from->cache_hs_key_truncate_onpage_removal;
    to->cache_hs_order_remove += from->cache_hs_order_remove;
    to->cache_hs_write_squash += from->cache_hs_write_squash;
    to->cache_inmem_splittable += from->cache_inmem_splittable;
    to->cache_inmem_split += from->cache_inmem_split;
    to->cache_eviction_internal += from->cache_eviction_internal;
    to->cache_eviction_split_internal += from->cache_eviction_split_internal;
    to->cache_eviction_split_leaf += from->cache_eviction_split_leaf;
    to->cache_eviction_dirty += from->cache_eviction_dirty;
    to->cache_read_overflow += from->cache_read_overflow;
    to->cache_eviction_deepen += from->cache_eviction_deepen;
    to->cache_write_hs += from->cache_write_hs;
    to->cache_read += from->cache_read;
    to->cache_read_deleted += from->cache_read_deleted;
    to->cache_read_deleted_prepared += from->cache_read_deleted_prepared;
    to->cache_pages_requested += from->cache_pages_requested;
    to->cache_eviction_pages_seen += from->cache_eviction_pages_seen;
    to->cache_write += from->cache_write;
    to->cache_write_restore += from->cache_write_restore;
    to->cache_hs_insert_full_update += from->cache_hs_insert_full_update;
    to->cache_hs_insert_reverse_modify += from->cache_hs_insert_reverse_modify;
    to->cache_bytes_dirty += from->cache_bytes_dirty;
    to->cache_eviction_clean += from->cache_eviction_clean;
    to->cache_state_gen_avg_gap += from->cache_state_gen_avg_gap;
    to->cache_state_avg_written_size += from->cache_state_avg_written_size;
    to->cache_state_avg_visited_age += from->cache_state_avg_visited_age;
    to->cache_state_avg_unvisited_age += from->cache_state_avg_unvisited_age;
    to->cache_state_pages_clean += from->cache_state_pages_clean;
    to->cache_state_gen_current += from->cache_state_gen_current;
    to->cache_state_pages_dirty += from->cache_state_pages_dirty;
    to->cache_state_root_entries += from->cache_state_root_entries;
    to->cache_state_pages_internal += from->cache_state_pages_internal;
    to->cache_state_pages_leaf += from->cache_state_pages_leaf;
    to->cache_state_gen_max_gap += from->cache_state_gen_max_gap;
    to->cache_state_max_pagesize += from->cache_state_max_pagesize;
    to->cache_state_min_written_size += from->cache_state_min_written_size;
    to->cache_state_unvisited_count += from->cache_state_unvisited_count;
    to->cache_state_smaller_alloc_size += from->cache_state_smaller_alloc_size;
    to->cache_state_memory += from->cache_state_memory;
    to->cache_state_queued += from->cache_state_queued;
    to->cache_state_not_queueable += from->cache_state_not_queueable;
    to->cache_state_refs_skipped += from->cache_state_refs_skipped;
    to->cache_state_root_size += from->cache_state_root_size;
    to->cache_state_pages += from->cache_state_pages;
    to->cc_pages_evict += from->cc_pages_evict;
    to->cc_pages_removed += from->cc_pages_removed;
    to->cc_pages_walk_skipped += from->cc_pages_walk_skipped;
    to->cc_pages_visited += from->cc_pages_visited;
    to->compress_precomp_intl_max_page_size += from->compress_precomp_intl_max_page_size;
    to->compress_precomp_leaf_max_page_size += from->compress_precomp_leaf_max_page_size;
    to->compress_read += from->compress_read;
    to->compress_write += from->compress_write;
    to->compress_hist_ratio_max += from->compress_hist_ratio_max;
    to->compress_hist_ratio_16 += from->compress_hist_ratio_16;
    to->compress_hist_ratio_2 += from->compress_hist_ratio_2;
    to->compress_hist_ratio_32 += from->compress_hist_ratio_32;
    to->compress_hist_ratio_4 += from->compress_hist_ratio_4;
    to->compress_hist_ratio_64 += from->compress_hist_ratio_64;
    to->compress_hist_ratio_8 += from->compress_hist_ratio_8;
    to->compress_write_fail += from->compress_write_fail;
    to->compress_write_too_small += from->compress_write_too_small;
    to->cursor_next_skip_total += from->cursor_next_skip_total;
    to->cursor_prev_skip_total += from->cursor_prev_skip_total;
    to->cursor_skip_hs_cur_position += from->cursor_skip_hs_cur_position;
    to->cursor_search_near_prefix_fast_paths += from->cursor_search_near_prefix_fast_paths;
    to->cursor_insert_bulk += from->cursor_insert_bulk;
    to->cursor_reopen += from->cursor_reopen;
    to->cursor_cache += from->cursor_cache;
    to->cursor_create += from->cursor_create;
    to->cursor_next_hs_tombstone += from->cursor_next_hs_tombstone;
    to->cursor_next_skip_ge_100 += from->cursor_next_skip_ge_100;
    to->cursor_next_skip_lt_100 += from->cursor_next_skip_lt_100;
    to->cursor_prev_hs_tombstone += from->cursor_prev_hs_tombstone;
    to->cursor_prev_skip_ge_100 += from->cursor_prev_skip_ge_100;
    to->cursor_prev_skip_lt_100 += from->cursor_prev_skip_lt_100;
    to->cursor_insert += from->cursor_insert;
    to->cursor_insert_bytes += from->cursor_insert_bytes;
    to->cursor_modify += from->cursor_modify;
    to->cursor_modify_bytes += from->cursor_modify_bytes;
    to->cursor_modify_bytes_touch += from->cursor_modify_bytes_touch;
    to->cursor_next += from->cursor_next;
    to->cursor_open_count += from->cursor_open_count;
    to->cursor_restart += from->cursor_restart;
    to->cursor_prev += from->cursor_prev;
    to->cursor_remove += from->cursor_remove;
    to->cursor_remove_bytes += from->cursor_remove_bytes;
    to->cursor_reserve += from->cursor_reserve;
    to->cursor_reset += from->cursor_reset;
    to->cursor_search += from->cursor_search;
    to->cursor_search_hs += from->cursor_search_hs;
    to->cursor_search_near += from->cursor_search_near;
    to->cursor_truncate += from->cursor_truncate;
    to->cursor_update += from->cursor_update;
    to->cursor_update_bytes += from->cursor_update_bytes;
    to->cursor_update_bytes_changed += from->cursor_update_bytes_changed;
    to->rec_time_window_bytes_ts += from->rec_time_window_bytes_ts;
    to->rec_time_window_bytes_txn += from->rec_time_window_bytes_txn;
    to->rec_dictionary += from->rec_dictionary;
    to->rec_page_delete_fast += from->rec_page_delete_fast;
    to->rec_suffix_compression += from->rec_suffix_compression;
    to->rec_multiblock_internal += from->rec_multiblock_internal;
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
    to->rec_time_aggr_newest_start_durable_ts += from->rec_time_aggr_newest_start_durable_ts;
    to->rec_time_aggr_newest_stop_durable_ts += from->rec_time_aggr_newest_stop_durable_ts;
    to->rec_time_aggr_newest_stop_ts += from->rec_time_aggr_newest_stop_ts;
    to->rec_time_aggr_newest_stop_txn += from->rec_time_aggr_newest_stop_txn;
    to->rec_time_aggr_newest_txn += from->rec_time_aggr_newest_txn;
    to->rec_time_aggr_oldest_start_ts += from->rec_time_aggr_oldest_start_ts;
    to->rec_time_aggr_prepared += from->rec_time_aggr_prepared;
    to->rec_time_window_pages_prepared += from->rec_time_window_pages_prepared;
    to->rec_time_window_pages_durable_start_ts += from->rec_time_window_pages_durable_start_ts;
    to->rec_time_window_pages_start_ts += from->rec_time_window_pages_start_ts;
    to->rec_time_window_pages_start_txn += from->rec_time_window_pages_start_txn;
    to->rec_time_window_pages_durable_stop_ts += from->rec_time_window_pages_durable_stop_ts;
    to->rec_time_window_pages_stop_ts += from->rec_time_window_pages_stop_ts;
    to->rec_time_window_pages_stop_txn += from->rec_time_window_pages_stop_txn;
    to->rec_time_window_prepared += from->rec_time_window_prepared;
    to->rec_time_window_durable_start_ts += from->rec_time_window_durable_start_ts;
    to->rec_time_window_start_ts += from->rec_time_window_start_ts;
    to->rec_time_window_start_txn += from->rec_time_window_start_txn;
    to->rec_time_window_durable_stop_ts += from->rec_time_window_durable_stop_ts;
    to->rec_time_window_stop_ts += from->rec_time_window_stop_ts;
    to->rec_time_window_stop_txn += from->rec_time_window_stop_txn;
    to->session_compact += from->session_compact;
    to->tiered_work_units_dequeued += from->tiered_work_units_dequeued;
    to->tiered_work_units_created += from->tiered_work_units_created;
    to->tiered_retention += from->tiered_retention;
    to->tiered_object_size += from->tiered_object_size;
    to->txn_read_race_prepare_update += from->txn_read_race_prepare_update;
    to->txn_rts_hs_stop_older_than_newer_start += from->txn_rts_hs_stop_older_than_newer_start;
    to->txn_rts_inconsistent_ckpt += from->txn_rts_inconsistent_ckpt;
    to->txn_rts_keys_removed += from->txn_rts_keys_removed;
    to->txn_rts_keys_restored += from->txn_rts_keys_restored;
    to->txn_rts_hs_restore_tombstones += from->txn_rts_hs_restore_tombstones;
    to->txn_rts_hs_restore_updates += from->txn_rts_hs_restore_updates;
    to->txn_rts_delete_rle_skipped += from->txn_rts_delete_rle_skipped;
    to->txn_rts_stable_rle_skipped += from->txn_rts_stable_rle_skipped;
    to->txn_rts_sweep_hs_keys += from->txn_rts_sweep_hs_keys;
    to->txn_rts_hs_removed += from->txn_rts_hs_removed;
    to->txn_checkpoint_obsolete_applied += from->txn_checkpoint_obsolete_applied;
    to->txn_update_conflict += from->txn_update_conflict;
}

void
__wt_stat_dsrc_aggregate(WT_DSRC_STATS **from, WT_DSRC_STATS *to)
{
    int64_t v;

    to->bloom_false_positive += WT_STAT_READ(from, bloom_false_positive);
    to->bloom_hit += WT_STAT_READ(from, bloom_hit);
    to->bloom_miss += WT_STAT_READ(from, bloom_miss);
    to->bloom_page_evict += WT_STAT_READ(from, bloom_page_evict);
    to->bloom_page_read += WT_STAT_READ(from, bloom_page_read);
    to->bloom_count += WT_STAT_READ(from, bloom_count);
    to->lsm_chunk_count += WT_STAT_READ(from, lsm_chunk_count);
    if ((v = WT_STAT_READ(from, lsm_generation_max)) > to->lsm_generation_max)
        to->lsm_generation_max = v;
    to->lsm_lookup_no_bloom += WT_STAT_READ(from, lsm_lookup_no_bloom);
    to->lsm_checkpoint_throttle += WT_STAT_READ(from, lsm_checkpoint_throttle);
    to->lsm_merge_throttle += WT_STAT_READ(from, lsm_merge_throttle);
    to->bloom_size += WT_STAT_READ(from, bloom_size);
    to->block_extension += WT_STAT_READ(from, block_extension);
    to->block_alloc += WT_STAT_READ(from, block_alloc);
    to->block_free += WT_STAT_READ(from, block_free);
    to->block_checkpoint_size += WT_STAT_READ(from, block_checkpoint_size);
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
    to->btree_checkpoint_generation += WT_STAT_READ(from, btree_checkpoint_generation);
    to->btree_clean_checkpoint_timer += WT_STAT_READ(from, btree_clean_checkpoint_timer);
    to->btree_compact_pages_reviewed += WT_STAT_READ(from, btree_compact_pages_reviewed);
    to->btree_compact_pages_rewritten += WT_STAT_READ(from, btree_compact_pages_rewritten);
    to->btree_compact_pages_skipped += WT_STAT_READ(from, btree_compact_pages_skipped);
    to->btree_compact_skipped += WT_STAT_READ(from, btree_compact_skipped);
    to->btree_column_fix += WT_STAT_READ(from, btree_column_fix);
    to->btree_column_tws += WT_STAT_READ(from, btree_column_tws);
    to->btree_column_internal += WT_STAT_READ(from, btree_column_internal);
    to->btree_column_rle += WT_STAT_READ(from, btree_column_rle);
    to->btree_column_deleted += WT_STAT_READ(from, btree_column_deleted);
    to->btree_column_variable += WT_STAT_READ(from, btree_column_variable);
    if ((v = WT_STAT_READ(from, btree_fixed_len)) > to->btree_fixed_len)
        to->btree_fixed_len = v;
    if ((v = WT_STAT_READ(from, btree_maxintlpage)) > to->btree_maxintlpage)
        to->btree_maxintlpage = v;
    if ((v = WT_STAT_READ(from, btree_maxleafkey)) > to->btree_maxleafkey)
        to->btree_maxleafkey = v;
    if ((v = WT_STAT_READ(from, btree_maxleafpage)) > to->btree_maxleafpage)
        to->btree_maxleafpage = v;
    if ((v = WT_STAT_READ(from, btree_maxleafvalue)) > to->btree_maxleafvalue)
        to->btree_maxleafvalue = v;
    if ((v = WT_STAT_READ(from, btree_maximum_depth)) > to->btree_maximum_depth)
        to->btree_maximum_depth = v;
    to->btree_entries += WT_STAT_READ(from, btree_entries);
    to->btree_overflow += WT_STAT_READ(from, btree_overflow);
    to->btree_row_empty_values += WT_STAT_READ(from, btree_row_empty_values);
    to->btree_row_internal += WT_STAT_READ(from, btree_row_internal);
    to->btree_row_leaf += WT_STAT_READ(from, btree_row_leaf);
    to->cache_bytes_inuse += WT_STAT_READ(from, cache_bytes_inuse);
    to->cache_bytes_dirty_total += WT_STAT_READ(from, cache_bytes_dirty_total);
    to->cache_bytes_read += WT_STAT_READ(from, cache_bytes_read);
    to->cache_bytes_write += WT_STAT_READ(from, cache_bytes_write);
    to->cache_eviction_checkpoint += WT_STAT_READ(from, cache_eviction_checkpoint);
    to->cache_eviction_blocked_checkpoint_hs +=
      WT_STAT_READ(from, cache_eviction_blocked_checkpoint_hs);
    to->cache_eviction_fail += WT_STAT_READ(from, cache_eviction_fail);
    to->cache_eviction_blocked_ooo_checkpoint_race_1 +=
      WT_STAT_READ(from, cache_eviction_blocked_ooo_checkpoint_race_1);
    to->cache_eviction_blocked_ooo_checkpoint_race_2 +=
      WT_STAT_READ(from, cache_eviction_blocked_ooo_checkpoint_race_2);
    to->cache_eviction_blocked_ooo_checkpoint_race_3 +=
      WT_STAT_READ(from, cache_eviction_blocked_ooo_checkpoint_race_3);
    to->cache_eviction_blocked_ooo_checkpoint_race_4 +=
      WT_STAT_READ(from, cache_eviction_blocked_ooo_checkpoint_race_4);
    to->cache_eviction_walk_passes += WT_STAT_READ(from, cache_eviction_walk_passes);
    to->cache_eviction_target_page_lt10 += WT_STAT_READ(from, cache_eviction_target_page_lt10);
    to->cache_eviction_target_page_lt32 += WT_STAT_READ(from, cache_eviction_target_page_lt32);
    to->cache_eviction_target_page_ge128 += WT_STAT_READ(from, cache_eviction_target_page_ge128);
    to->cache_eviction_target_page_lt64 += WT_STAT_READ(from, cache_eviction_target_page_lt64);
    to->cache_eviction_target_page_lt128 += WT_STAT_READ(from, cache_eviction_target_page_lt128);
    to->cache_eviction_target_page_reduced +=
      WT_STAT_READ(from, cache_eviction_target_page_reduced);
    to->cache_eviction_walks_abandoned += WT_STAT_READ(from, cache_eviction_walks_abandoned);
    to->cache_eviction_walks_stopped += WT_STAT_READ(from, cache_eviction_walks_stopped);
    to->cache_eviction_walks_gave_up_no_targets +=
      WT_STAT_READ(from, cache_eviction_walks_gave_up_no_targets);
    to->cache_eviction_walks_gave_up_ratio +=
      WT_STAT_READ(from, cache_eviction_walks_gave_up_ratio);
    to->cache_eviction_walks_ended += WT_STAT_READ(from, cache_eviction_walks_ended);
    to->cache_eviction_walk_restart += WT_STAT_READ(from, cache_eviction_walk_restart);
    to->cache_eviction_walk_from_root += WT_STAT_READ(from, cache_eviction_walk_from_root);
    to->cache_eviction_walk_saved_pos += WT_STAT_READ(from, cache_eviction_walk_saved_pos);
    to->cache_eviction_hazard += WT_STAT_READ(from, cache_eviction_hazard);
    to->cache_hs_insert += WT_STAT_READ(from, cache_hs_insert);
    to->cache_hs_insert_restart += WT_STAT_READ(from, cache_hs_insert_restart);
    to->cache_hs_order_lose_durable_timestamp +=
      WT_STAT_READ(from, cache_hs_order_lose_durable_timestamp);
    to->cache_hs_order_reinsert += WT_STAT_READ(from, cache_hs_order_reinsert);
    to->cache_hs_read += WT_STAT_READ(from, cache_hs_read);
    to->cache_hs_read_miss += WT_STAT_READ(from, cache_hs_read_miss);
    to->cache_hs_read_squash += WT_STAT_READ(from, cache_hs_read_squash);
    to->cache_hs_key_truncate_rts_unstable +=
      WT_STAT_READ(from, cache_hs_key_truncate_rts_unstable);
    to->cache_hs_key_truncate_rts += WT_STAT_READ(from, cache_hs_key_truncate_rts);
    to->cache_hs_key_truncate += WT_STAT_READ(from, cache_hs_key_truncate);
    to->cache_hs_key_truncate_onpage_removal +=
      WT_STAT_READ(from, cache_hs_key_truncate_onpage_removal);
    to->cache_hs_order_remove += WT_STAT_READ(from, cache_hs_order_remove);
    to->cache_hs_write_squash += WT_STAT_READ(from, cache_hs_write_squash);
    to->cache_inmem_splittable += WT_STAT_READ(from, cache_inmem_splittable);
    to->cache_inmem_split += WT_STAT_READ(from, cache_inmem_split);
    to->cache_eviction_internal += WT_STAT_READ(from, cache_eviction_internal);
    to->cache_eviction_split_internal += WT_STAT_READ(from, cache_eviction_split_internal);
    to->cache_eviction_split_leaf += WT_STAT_READ(from, cache_eviction_split_leaf);
    to->cache_eviction_dirty += WT_STAT_READ(from, cache_eviction_dirty);
    to->cache_read_overflow += WT_STAT_READ(from, cache_read_overflow);
    to->cache_eviction_deepen += WT_STAT_READ(from, cache_eviction_deepen);
    to->cache_write_hs += WT_STAT_READ(from, cache_write_hs);
    to->cache_read += WT_STAT_READ(from, cache_read);
    to->cache_read_deleted += WT_STAT_READ(from, cache_read_deleted);
    to->cache_read_deleted_prepared += WT_STAT_READ(from, cache_read_deleted_prepared);
    to->cache_pages_requested += WT_STAT_READ(from, cache_pages_requested);
    to->cache_eviction_pages_seen += WT_STAT_READ(from, cache_eviction_pages_seen);
    to->cache_write += WT_STAT_READ(from, cache_write);
    to->cache_write_restore += WT_STAT_READ(from, cache_write_restore);
    to->cache_hs_insert_full_update += WT_STAT_READ(from, cache_hs_insert_full_update);
    to->cache_hs_insert_reverse_modify += WT_STAT_READ(from, cache_hs_insert_reverse_modify);
    to->cache_bytes_dirty += WT_STAT_READ(from, cache_bytes_dirty);
    to->cache_eviction_clean += WT_STAT_READ(from, cache_eviction_clean);
    to->cache_state_gen_avg_gap += WT_STAT_READ(from, cache_state_gen_avg_gap);
    to->cache_state_avg_written_size += WT_STAT_READ(from, cache_state_avg_written_size);
    to->cache_state_avg_visited_age += WT_STAT_READ(from, cache_state_avg_visited_age);
    to->cache_state_avg_unvisited_age += WT_STAT_READ(from, cache_state_avg_unvisited_age);
    to->cache_state_pages_clean += WT_STAT_READ(from, cache_state_pages_clean);
    to->cache_state_gen_current += WT_STAT_READ(from, cache_state_gen_current);
    to->cache_state_pages_dirty += WT_STAT_READ(from, cache_state_pages_dirty);
    to->cache_state_root_entries += WT_STAT_READ(from, cache_state_root_entries);
    to->cache_state_pages_internal += WT_STAT_READ(from, cache_state_pages_internal);
    to->cache_state_pages_leaf += WT_STAT_READ(from, cache_state_pages_leaf);
    to->cache_state_gen_max_gap += WT_STAT_READ(from, cache_state_gen_max_gap);
    to->cache_state_max_pagesize += WT_STAT_READ(from, cache_state_max_pagesize);
    to->cache_state_min_written_size += WT_STAT_READ(from, cache_state_min_written_size);
    to->cache_state_unvisited_count += WT_STAT_READ(from, cache_state_unvisited_count);
    to->cache_state_smaller_alloc_size += WT_STAT_READ(from, cache_state_smaller_alloc_size);
    to->cache_state_memory += WT_STAT_READ(from, cache_state_memory);
    to->cache_state_queued += WT_STAT_READ(from, cache_state_queued);
    to->cache_state_not_queueable += WT_STAT_READ(from, cache_state_not_queueable);
    to->cache_state_refs_skipped += WT_STAT_READ(from, cache_state_refs_skipped);
    to->cache_state_root_size += WT_STAT_READ(from, cache_state_root_size);
    to->cache_state_pages += WT_STAT_READ(from, cache_state_pages);
    to->cc_pages_evict += WT_STAT_READ(from, cc_pages_evict);
    to->cc_pages_removed += WT_STAT_READ(from, cc_pages_removed);
    to->cc_pages_walk_skipped += WT_STAT_READ(from, cc_pages_walk_skipped);
    to->cc_pages_visited += WT_STAT_READ(from, cc_pages_visited);
    to->compress_precomp_intl_max_page_size +=
      WT_STAT_READ(from, compress_precomp_intl_max_page_size);
    to->compress_precomp_leaf_max_page_size +=
      WT_STAT_READ(from, compress_precomp_leaf_max_page_size);
    to->compress_read += WT_STAT_READ(from, compress_read);
    to->compress_write += WT_STAT_READ(from, compress_write);
    to->compress_hist_ratio_max += WT_STAT_READ(from, compress_hist_ratio_max);
    to->compress_hist_ratio_16 += WT_STAT_READ(from, compress_hist_ratio_16);
    to->compress_hist_ratio_2 += WT_STAT_READ(from, compress_hist_ratio_2);
    to->compress_hist_ratio_32 += WT_STAT_READ(from, compress_hist_ratio_32);
    to->compress_hist_ratio_4 += WT_STAT_READ(from, compress_hist_ratio_4);
    to->compress_hist_ratio_64 += WT_STAT_READ(from, compress_hist_ratio_64);
    to->compress_hist_ratio_8 += WT_STAT_READ(from, compress_hist_ratio_8);
    to->compress_write_fail += WT_STAT_READ(from, compress_write_fail);
    to->compress_write_too_small += WT_STAT_READ(from, compress_write_too_small);
    to->cursor_next_skip_total += WT_STAT_READ(from, cursor_next_skip_total);
    to->cursor_prev_skip_total += WT_STAT_READ(from, cursor_prev_skip_total);
    to->cursor_skip_hs_cur_position += WT_STAT_READ(from, cursor_skip_hs_cur_position);
    to->cursor_search_near_prefix_fast_paths +=
      WT_STAT_READ(from, cursor_search_near_prefix_fast_paths);
    to->cursor_insert_bulk += WT_STAT_READ(from, cursor_insert_bulk);
    to->cursor_reopen += WT_STAT_READ(from, cursor_reopen);
    to->cursor_cache += WT_STAT_READ(from, cursor_cache);
    to->cursor_create += WT_STAT_READ(from, cursor_create);
    to->cursor_next_hs_tombstone += WT_STAT_READ(from, cursor_next_hs_tombstone);
    to->cursor_next_skip_ge_100 += WT_STAT_READ(from, cursor_next_skip_ge_100);
    to->cursor_next_skip_lt_100 += WT_STAT_READ(from, cursor_next_skip_lt_100);
    to->cursor_prev_hs_tombstone += WT_STAT_READ(from, cursor_prev_hs_tombstone);
    to->cursor_prev_skip_ge_100 += WT_STAT_READ(from, cursor_prev_skip_ge_100);
    to->cursor_prev_skip_lt_100 += WT_STAT_READ(from, cursor_prev_skip_lt_100);
    to->cursor_insert += WT_STAT_READ(from, cursor_insert);
    to->cursor_insert_bytes += WT_STAT_READ(from, cursor_insert_bytes);
    to->cursor_modify += WT_STAT_READ(from, cursor_modify);
    to->cursor_modify_bytes += WT_STAT_READ(from, cursor_modify_bytes);
    to->cursor_modify_bytes_touch += WT_STAT_READ(from, cursor_modify_bytes_touch);
    to->cursor_next += WT_STAT_READ(from, cursor_next);
    to->cursor_open_count += WT_STAT_READ(from, cursor_open_count);
    to->cursor_restart += WT_STAT_READ(from, cursor_restart);
    to->cursor_prev += WT_STAT_READ(from, cursor_prev);
    to->cursor_remove += WT_STAT_READ(from, cursor_remove);
    to->cursor_remove_bytes += WT_STAT_READ(from, cursor_remove_bytes);
    to->cursor_reserve += WT_STAT_READ(from, cursor_reserve);
    to->cursor_reset += WT_STAT_READ(from, cursor_reset);
    to->cursor_search += WT_STAT_READ(from, cursor_search);
    to->cursor_search_hs += WT_STAT_READ(from, cursor_search_hs);
    to->cursor_search_near += WT_STAT_READ(from, cursor_search_near);
    to->cursor_truncate += WT_STAT_READ(from, cursor_truncate);
    to->cursor_update += WT_STAT_READ(from, cursor_update);
    to->cursor_update_bytes += WT_STAT_READ(from, cursor_update_bytes);
    to->cursor_update_bytes_changed += WT_STAT_READ(from, cursor_update_bytes_changed);
    to->rec_time_window_bytes_ts += WT_STAT_READ(from, rec_time_window_bytes_ts);
    to->rec_time_window_bytes_txn += WT_STAT_READ(from, rec_time_window_bytes_txn);
    to->rec_dictionary += WT_STAT_READ(from, rec_dictionary);
    to->rec_page_delete_fast += WT_STAT_READ(from, rec_page_delete_fast);
    to->rec_suffix_compression += WT_STAT_READ(from, rec_suffix_compression);
    to->rec_multiblock_internal += WT_STAT_READ(from, rec_multiblock_internal);
    to->rec_prefix_compression += WT_STAT_READ(from, rec_prefix_compression);
    to->rec_multiblock_leaf += WT_STAT_READ(from, rec_multiblock_leaf);
    to->rec_overflow_key_leaf += WT_STAT_READ(from, rec_overflow_key_leaf);
    if ((v = WT_STAT_READ(from, rec_multiblock_max)) > to->rec_multiblock_max)
        to->rec_multiblock_max = v;
    to->rec_overflow_value += WT_STAT_READ(from, rec_overflow_value);
    to->rec_page_match += WT_STAT_READ(from, rec_page_match);
    to->rec_pages += WT_STAT_READ(from, rec_pages);
    to->rec_pages_eviction += WT_STAT_READ(from, rec_pages_eviction);
    to->rec_page_delete += WT_STAT_READ(from, rec_page_delete);
    to->rec_time_aggr_newest_start_durable_ts +=
      WT_STAT_READ(from, rec_time_aggr_newest_start_durable_ts);
    to->rec_time_aggr_newest_stop_durable_ts +=
      WT_STAT_READ(from, rec_time_aggr_newest_stop_durable_ts);
    to->rec_time_aggr_newest_stop_ts += WT_STAT_READ(from, rec_time_aggr_newest_stop_ts);
    to->rec_time_aggr_newest_stop_txn += WT_STAT_READ(from, rec_time_aggr_newest_stop_txn);
    to->rec_time_aggr_newest_txn += WT_STAT_READ(from, rec_time_aggr_newest_txn);
    to->rec_time_aggr_oldest_start_ts += WT_STAT_READ(from, rec_time_aggr_oldest_start_ts);
    to->rec_time_aggr_prepared += WT_STAT_READ(from, rec_time_aggr_prepared);
    to->rec_time_window_pages_prepared += WT_STAT_READ(from, rec_time_window_pages_prepared);
    to->rec_time_window_pages_durable_start_ts +=
      WT_STAT_READ(from, rec_time_window_pages_durable_start_ts);
    to->rec_time_window_pages_start_ts += WT_STAT_READ(from, rec_time_window_pages_start_ts);
    to->rec_time_window_pages_start_txn += WT_STAT_READ(from, rec_time_window_pages_start_txn);
    to->rec_time_window_pages_durable_stop_ts +=
      WT_STAT_READ(from, rec_time_window_pages_durable_stop_ts);
    to->rec_time_window_pages_stop_ts += WT_STAT_READ(from, rec_time_window_pages_stop_ts);
    to->rec_time_window_pages_stop_txn += WT_STAT_READ(from, rec_time_window_pages_stop_txn);
    to->rec_time_window_prepared += WT_STAT_READ(from, rec_time_window_prepared);
    to->rec_time_window_durable_start_ts += WT_STAT_READ(from, rec_time_window_durable_start_ts);
    to->rec_time_window_start_ts += WT_STAT_READ(from, rec_time_window_start_ts);
    to->rec_time_window_start_txn += WT_STAT_READ(from, rec_time_window_start_txn);
    to->rec_time_window_durable_stop_ts += WT_STAT_READ(from, rec_time_window_durable_stop_ts);
    to->rec_time_window_stop_ts += WT_STAT_READ(from, rec_time_window_stop_ts);
    to->rec_time_window_stop_txn += WT_STAT_READ(from, rec_time_window_stop_txn);
    to->session_compact += WT_STAT_READ(from, session_compact);
    to->tiered_work_units_dequeued += WT_STAT_READ(from, tiered_work_units_dequeued);
    to->tiered_work_units_created += WT_STAT_READ(from, tiered_work_units_created);
    to->tiered_retention += WT_STAT_READ(from, tiered_retention);
    to->tiered_object_size += WT_STAT_READ(from, tiered_object_size);
    to->txn_read_race_prepare_update += WT_STAT_READ(from, txn_read_race_prepare_update);
    to->txn_rts_hs_stop_older_than_newer_start +=
      WT_STAT_READ(from, txn_rts_hs_stop_older_than_newer_start);
    to->txn_rts_inconsistent_ckpt += WT_STAT_READ(from, txn_rts_inconsistent_ckpt);
    to->txn_rts_keys_removed += WT_STAT_READ(from, txn_rts_keys_removed);
    to->txn_rts_keys_restored += WT_STAT_READ(from, txn_rts_keys_restored);
    to->txn_rts_hs_restore_tombstones += WT_STAT_READ(from, txn_rts_hs_restore_tombstones);
    to->txn_rts_hs_restore_updates += WT_STAT_READ(from, txn_rts_hs_restore_updates);
    to->txn_rts_delete_rle_skipped += WT_STAT_READ(from, txn_rts_delete_rle_skipped);
    to->txn_rts_stable_rle_skipped += WT_STAT_READ(from, txn_rts_stable_rle_skipped);
    to->txn_rts_sweep_hs_keys += WT_STAT_READ(from, txn_rts_sweep_hs_keys);
    to->txn_rts_hs_removed += WT_STAT_READ(from, txn_rts_hs_removed);
    to->txn_checkpoint_obsolete_applied += WT_STAT_READ(from, txn_checkpoint_obsolete_applied);
    to->txn_update_conflict += WT_STAT_READ(from, txn_update_conflict);
}

static const char *const __stats_connection_desc[] = {
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
  "block-manager: block cache cached blocks updated",
  "block-manager: block cache cached bytes updated",
  "block-manager: block cache evicted blocks",
  "block-manager: block cache file size causing bypass",
  "block-manager: block cache lookups",
  "block-manager: block cache number of blocks not evicted due to overhead",
  "block-manager: block cache number of bypasses because no-write-allocate setting was on",
  "block-manager: block cache number of bypasses due to overhead on put",
  "block-manager: block cache number of bypasses on get",
  "block-manager: block cache number of bypasses on put because file is too small",
  "block-manager: block cache number of eviction passes",
  "block-manager: block cache number of hits including existence checks",
  "block-manager: block cache number of misses including existence checks",
  "block-manager: block cache number of put bypasses on checkpoint I/O",
  "block-manager: block cache removed blocks",
  "block-manager: block cache total blocks",
  "block-manager: block cache total blocks inserted on read path",
  "block-manager: block cache total blocks inserted on write path",
  "block-manager: block cache total bytes",
  "block-manager: block cache total bytes inserted on read path",
  "block-manager: block cache total bytes inserted on write path",
  "block-manager: blocks pre-loaded",
  "block-manager: blocks read",
  "block-manager: blocks written",
  "block-manager: bytes read",
  "block-manager: bytes read via memory map API",
  "block-manager: bytes read via system call API",
  "block-manager: bytes written",
  "block-manager: bytes written for checkpoint",
  "block-manager: bytes written via memory map API",
  "block-manager: bytes written via system call API",
  "block-manager: mapped blocks read",
  "block-manager: mapped bytes read",
  "block-manager: number of times the file was remapped because it changed size via fallocate or "
  "truncate",
  "block-manager: number of times the region was remapped via write",
  "cache: application threads page read from disk to cache count",
  "cache: application threads page read from disk to cache time (usecs)",
  "cache: application threads page write from cache to disk count",
  "cache: application threads page write from cache to disk time (usecs)",
  "cache: bytes allocated for updates",
  "cache: bytes belonging to page images in the cache",
  "cache: bytes belonging to the history store table in the cache",
  "cache: bytes currently in the cache",
  "cache: bytes dirty in the cache cumulative",
  "cache: bytes not belonging to page images in the cache",
  "cache: bytes read into cache",
  "cache: bytes written from cache",
  "cache: cache overflow score",
  "cache: checkpoint blocked page eviction",
  "cache: checkpoint of history store file blocked non-history store page eviction",
  "cache: eviction calls to get a page",
  "cache: eviction calls to get a page found queue empty",
  "cache: eviction calls to get a page found queue empty after locking",
  "cache: eviction currently operating in aggressive mode",
  "cache: eviction empty score",
  "cache: eviction gave up due to detecting an out of order on disk value behind the last update "
  "on the chain",
  "cache: eviction gave up due to detecting an out of order tombstone ahead of the selected on "
  "disk update",
  "cache: eviction gave up due to detecting an out of order tombstone ahead of the selected on "
  "disk update after validating the update chain",
  "cache: eviction gave up due to detecting out of order timestamps on the update chain after the "
  "selected on disk update",
  "cache: eviction passes of a file",
  "cache: eviction server candidate queue empty when topping up",
  "cache: eviction server candidate queue not empty when topping up",
  "cache: eviction server evicting pages",
  "cache: eviction server slept, because we did not make progress with eviction",
  "cache: eviction server unable to reach eviction goal",
  "cache: eviction server waiting for a leaf page",
  "cache: eviction state",
  "cache: eviction walk most recent sleeps for checkpoint handle gathering",
  "cache: eviction walk target pages histogram - 0-9",
  "cache: eviction walk target pages histogram - 10-31",
  "cache: eviction walk target pages histogram - 128 and higher",
  "cache: eviction walk target pages histogram - 32-63",
  "cache: eviction walk target pages histogram - 64-128",
  "cache: eviction walk target pages reduced due to history store cache pressure",
  "cache: eviction walk target strategy both clean and dirty pages",
  "cache: eviction walk target strategy only clean pages",
  "cache: eviction walk target strategy only dirty pages",
  "cache: eviction walks abandoned",
  "cache: eviction walks gave up because they restarted their walk twice",
  "cache: eviction walks gave up because they saw too many pages and found no candidates",
  "cache: eviction walks gave up because they saw too many pages and found too few candidates",
  "cache: eviction walks reached end of tree",
  "cache: eviction walks restarted",
  "cache: eviction walks started from root of tree",
  "cache: eviction walks started from saved location in tree",
  "cache: eviction worker thread active",
  "cache: eviction worker thread created",
  "cache: eviction worker thread evicting pages",
  "cache: eviction worker thread removed",
  "cache: eviction worker thread stable number",
  "cache: files with active eviction walks",
  "cache: files with new eviction walks started",
  "cache: force re-tuning of eviction workers once in a while",
  "cache: forced eviction - history store pages failed to evict while session has history store "
  "cursor open",
  "cache: forced eviction - history store pages selected while session has history store cursor "
  "open",
  "cache: forced eviction - history store pages successfully evicted while session has history "
  "store cursor open",
  "cache: forced eviction - pages evicted that were clean count",
  "cache: forced eviction - pages evicted that were clean time (usecs)",
  "cache: forced eviction - pages evicted that were dirty count",
  "cache: forced eviction - pages evicted that were dirty time (usecs)",
  "cache: forced eviction - pages selected because of a large number of updates to a single item",
  "cache: forced eviction - pages selected because of too many deleted items count",
  "cache: forced eviction - pages selected count",
  "cache: forced eviction - pages selected unable to be evicted count",
  "cache: forced eviction - pages selected unable to be evicted time",
  "cache: hazard pointer blocked page eviction",
  "cache: hazard pointer check calls",
  "cache: hazard pointer check entries walked",
  "cache: hazard pointer maximum array length",
  "cache: history store score",
  "cache: history store table insert calls",
  "cache: history store table insert calls that returned restart",
  "cache: history store table max on-disk size",
  "cache: history store table on-disk size",
  "cache: history store table out-of-order resolved updates that lose their durable timestamp",
  "cache: history store table out-of-order updates that were fixed up by reinserting with the "
  "fixed timestamp",
  "cache: history store table reads",
  "cache: history store table reads missed",
  "cache: history store table reads requiring squashed modifies",
  "cache: history store table truncation by rollback to stable to remove an unstable update",
  "cache: history store table truncation by rollback to stable to remove an update",
  "cache: history store table truncation to remove an update",
  "cache: history store table truncation to remove range of updates due to key being removed from "
  "the data page during reconciliation",
  "cache: history store table truncation to remove range of updates due to out-of-order timestamp "
  "update on data page",
  "cache: history store table writes requiring squashed modifies",
  "cache: in-memory page passed criteria to be split",
  "cache: in-memory page splits",
  "cache: internal pages evicted",
  "cache: internal pages queued for eviction",
  "cache: internal pages seen by eviction walk",
  "cache: internal pages seen by eviction walk that are already queued",
  "cache: internal pages split during eviction",
  "cache: leaf pages split during eviction",
  "cache: maximum bytes configured",
  "cache: maximum page size at eviction",
  "cache: modified pages evicted",
  "cache: modified pages evicted by application threads",
  "cache: operations timed out waiting for space in cache",
  "cache: overflow pages read into cache",
  "cache: page split during eviction deepened the tree",
  "cache: page written requiring history store records",
  "cache: pages currently held in the cache",
  "cache: pages evicted by application threads",
  "cache: pages evicted in parallel with checkpoint",
  "cache: pages queued for eviction",
  "cache: pages queued for eviction post lru sorting",
  "cache: pages queued for urgent eviction",
  "cache: pages queued for urgent eviction during walk",
  "cache: pages queued for urgent eviction from history store due to high dirty content",
  "cache: pages read into cache",
  "cache: pages read into cache after truncate",
  "cache: pages read into cache after truncate in prepare state",
  "cache: pages requested from the cache",
  "cache: pages seen by eviction walk",
  "cache: pages seen by eviction walk that are already queued",
  "cache: pages selected for eviction unable to be evicted",
  "cache: pages selected for eviction unable to be evicted because of active children on an "
  "internal page",
  "cache: pages selected for eviction unable to be evicted because of failure in reconciliation",
  "cache: pages selected for eviction unable to be evicted because of race between checkpoint and "
  "out of order timestamps handling",
  "cache: pages walked for eviction",
  "cache: pages written from cache",
  "cache: pages written requiring in-memory restoration",
  "cache: percentage overhead",
  "cache: the number of times full update inserted to history store",
  "cache: the number of times reverse modify inserted to history store",
  "cache: tracked bytes belonging to internal pages in the cache",
  "cache: tracked bytes belonging to leaf pages in the cache",
  "cache: tracked dirty bytes in the cache",
  "cache: tracked dirty pages in the cache",
  "cache: unmodified pages evicted",
  "capacity: background fsync file handles considered",
  "capacity: background fsync file handles synced",
  "capacity: background fsync time (msecs)",
  "capacity: bytes read",
  "capacity: bytes written for checkpoint",
  "capacity: bytes written for eviction",
  "capacity: bytes written for log",
  "capacity: bytes written total",
  "capacity: threshold to call fsync",
  "capacity: time waiting due to total capacity (usecs)",
  "capacity: time waiting during checkpoint (usecs)",
  "capacity: time waiting during eviction (usecs)",
  "capacity: time waiting during logging (usecs)",
  "capacity: time waiting during read (usecs)",
  "checkpoint-cleanup: pages added for eviction",
  "checkpoint-cleanup: pages removed",
  "checkpoint-cleanup: pages skipped during tree walk",
  "checkpoint-cleanup: pages visited",
  "connection: auto adjusting condition resets",
  "connection: auto adjusting condition wait calls",
  "connection: auto adjusting condition wait raced to update timeout and skipped updating",
  "connection: detected system time went backwards",
  "connection: files currently open",
  "connection: hash bucket array size for data handles",
  "connection: hash bucket array size general",
  "connection: memory allocations",
  "connection: memory frees",
  "connection: memory re-allocations",
  "connection: pthread mutex condition wait calls",
  "connection: pthread mutex shared lock read-lock calls",
  "connection: pthread mutex shared lock write-lock calls",
  "connection: total fsync I/Os",
  "connection: total read I/Os",
  "connection: total write I/Os",
  "cursor: Total number of entries skipped by cursor next calls",
  "cursor: Total number of entries skipped by cursor prev calls",
  "cursor: Total number of entries skipped to position the history store cursor",
  "cursor: Total number of times a search near has exited due to prefix config",
  "cursor: cached cursor count",
  "cursor: cursor bulk loaded cursor insert calls",
  "cursor: cursor close calls that result in cache",
  "cursor: cursor create calls",
  "cursor: cursor insert calls",
  "cursor: cursor insert key and value bytes",
  "cursor: cursor modify calls",
  "cursor: cursor modify key and value bytes affected",
  "cursor: cursor modify value bytes modified",
  "cursor: cursor next calls",
  "cursor: cursor next calls that skip due to a globally visible history store tombstone",
  "cursor: cursor next calls that skip greater than or equal to 100 entries",
  "cursor: cursor next calls that skip less than 100 entries",
  "cursor: cursor operation restarted",
  "cursor: cursor prev calls",
  "cursor: cursor prev calls that skip due to a globally visible history store tombstone",
  "cursor: cursor prev calls that skip greater than or equal to 100 entries",
  "cursor: cursor prev calls that skip less than 100 entries",
  "cursor: cursor remove calls",
  "cursor: cursor remove key bytes removed",
  "cursor: cursor reserve calls",
  "cursor: cursor reset calls",
  "cursor: cursor search calls",
  "cursor: cursor search history store calls",
  "cursor: cursor search near calls",
  "cursor: cursor sweep buckets",
  "cursor: cursor sweep cursors closed",
  "cursor: cursor sweep cursors examined",
  "cursor: cursor sweeps",
  "cursor: cursor truncate calls",
  "cursor: cursor update calls",
  "cursor: cursor update key and value bytes",
  "cursor: cursor update value size change",
  "cursor: cursors reused from cache",
  "cursor: open cursor count",
  "data-handle: connection data handle size",
  "data-handle: connection data handles currently active",
  "data-handle: connection sweep candidate became referenced",
  "data-handle: connection sweep dhandles closed",
  "data-handle: connection sweep dhandles removed from hash list",
  "data-handle: connection sweep time-of-death sets",
  "data-handle: connection sweeps",
  "data-handle: connection sweeps skipped due to checkpoint gathering handles",
  "data-handle: session dhandles swept",
  "data-handle: session sweep attempts",
  "lock: checkpoint lock acquisitions",
  "lock: checkpoint lock application thread wait time (usecs)",
  "lock: checkpoint lock internal thread wait time (usecs)",
  "lock: dhandle lock application thread time waiting (usecs)",
  "lock: dhandle lock internal thread time waiting (usecs)",
  "lock: dhandle read lock acquisitions",
  "lock: dhandle write lock acquisitions",
  "lock: durable timestamp queue lock application thread time waiting (usecs)",
  "lock: durable timestamp queue lock internal thread time waiting (usecs)",
  "lock: durable timestamp queue read lock acquisitions",
  "lock: durable timestamp queue write lock acquisitions",
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
  "reconciliation: approximate byte size of timestamps in pages written",
  "reconciliation: approximate byte size of transaction IDs in pages written",
  "reconciliation: fast-path pages deleted",
  "reconciliation: leaf-page overflow keys",
  "reconciliation: maximum seconds spent in a reconciliation call",
  "reconciliation: page reconciliation calls",
  "reconciliation: page reconciliation calls for eviction",
  "reconciliation: page reconciliation calls that resulted in values with prepared transaction "
  "metadata",
  "reconciliation: page reconciliation calls that resulted in values with timestamps",
  "reconciliation: page reconciliation calls that resulted in values with transaction ids",
  "reconciliation: pages deleted",
  "reconciliation: pages written including an aggregated newest start durable timestamp ",
  "reconciliation: pages written including an aggregated newest stop durable timestamp ",
  "reconciliation: pages written including an aggregated newest stop timestamp ",
  "reconciliation: pages written including an aggregated newest stop transaction ID",
  "reconciliation: pages written including an aggregated newest transaction ID ",
  "reconciliation: pages written including an aggregated oldest start timestamp ",
  "reconciliation: pages written including an aggregated prepare",
  "reconciliation: pages written including at least one prepare state",
  "reconciliation: pages written including at least one start durable timestamp",
  "reconciliation: pages written including at least one start timestamp",
  "reconciliation: pages written including at least one start transaction ID",
  "reconciliation: pages written including at least one stop durable timestamp",
  "reconciliation: pages written including at least one stop timestamp",
  "reconciliation: pages written including at least one stop transaction ID",
  "reconciliation: records written including a prepare state",
  "reconciliation: records written including a start durable timestamp",
  "reconciliation: records written including a start timestamp",
  "reconciliation: records written including a start transaction ID",
  "reconciliation: records written including a stop durable timestamp",
  "reconciliation: records written including a stop timestamp",
  "reconciliation: records written including a stop transaction ID",
  "reconciliation: split bytes currently awaiting free",
  "reconciliation: split objects currently awaiting free",
  "session: attempts to remove a local object and the object is in use",
  "session: flush_tier operation calls",
  "session: local objects removed",
  "session: open session count",
  "session: session query timestamp calls",
  "session: table alter failed calls",
  "session: table alter successful calls",
  "session: table alter triggering checkpoint calls",
  "session: table alter unchanged and skipped",
  "session: table compact failed calls",
  "session: table compact failed calls due to cache pressure",
  "session: table compact running",
  "session: table compact skipped as process would not reduce file size",
  "session: table compact successful calls",
  "session: table compact timeout",
  "session: table create failed calls",
  "session: table create successful calls",
  "session: table drop failed calls",
  "session: table drop successful calls",
  "session: table rename failed calls",
  "session: table rename successful calls",
  "session: table salvage failed calls",
  "session: table salvage successful calls",
  "session: table truncate failed calls",
  "session: table truncate successful calls",
  "session: table verify failed calls",
  "session: table verify successful calls",
  "session: tiered operations dequeued and processed",
  "session: tiered operations scheduled",
  "session: tiered storage local retention time (secs)",
  "session: tiered storage object size",
  "thread-state: active filesystem fsync calls",
  "thread-state: active filesystem read calls",
  "thread-state: active filesystem write calls",
  "thread-yield: application thread time evicting (usecs)",
  "thread-yield: application thread time waiting for cache (usecs)",
  "thread-yield: connection close blocked waiting for transaction state stabilization",
  "thread-yield: connection close yielded for lsm manager shutdown",
  "thread-yield: data handle lock yielded",
  "thread-yield: get reference for page index and slot time sleeping (usecs)",
  "thread-yield: page access yielded due to prepare state change",
  "thread-yield: page acquire busy blocked",
  "thread-yield: page acquire eviction blocked",
  "thread-yield: page acquire locked blocked",
  "thread-yield: page acquire read blocked",
  "thread-yield: page acquire time sleeping (usecs)",
  "thread-yield: page delete rollback time sleeping for state change (usecs)",
  "thread-yield: page reconciliation yielded due to child modification",
  "transaction: Number of prepared updates",
  "transaction: Number of prepared updates committed",
  "transaction: Number of prepared updates repeated on the same key",
  "transaction: Number of prepared updates rolled back",
  "transaction: prepared transactions",
  "transaction: prepared transactions committed",
  "transaction: prepared transactions currently active",
  "transaction: prepared transactions rolled back",
  "transaction: prepared transactions rolled back and do not remove the history store entry",
  "transaction: prepared transactions rolled back and fix the history store entry with checkpoint "
  "reserved transaction id",
  "transaction: query timestamp calls",
  "transaction: race to read prepared update retry",
  "transaction: rollback to stable calls",
  "transaction: rollback to stable history store records with stop timestamps older than newer "
  "records",
  "transaction: rollback to stable inconsistent checkpoint",
  "transaction: rollback to stable keys removed",
  "transaction: rollback to stable keys restored",
  "transaction: rollback to stable pages visited",
  "transaction: rollback to stable restored tombstones from history store",
  "transaction: rollback to stable restored updates from history store",
  "transaction: rollback to stable skipping delete rle",
  "transaction: rollback to stable skipping stable rle",
  "transaction: rollback to stable sweeping history store keys",
  "transaction: rollback to stable tree walk skipping pages",
  "transaction: rollback to stable updates aborted",
  "transaction: rollback to stable updates removed from history store",
  "transaction: sessions scanned in each walk of concurrent sessions",
  "transaction: set timestamp calls",
  "transaction: set timestamp durable calls",
  "transaction: set timestamp durable updates",
  "transaction: set timestamp oldest calls",
  "transaction: set timestamp oldest updates",
  "transaction: set timestamp stable calls",
  "transaction: set timestamp stable updates",
  "transaction: transaction begins",
  "transaction: transaction checkpoint currently running",
  "transaction: transaction checkpoint currently running for history store file",
  "transaction: transaction checkpoint generation",
  "transaction: transaction checkpoint history store file duration (usecs)",
  "transaction: transaction checkpoint max time (msecs)",
  "transaction: transaction checkpoint min time (msecs)",
  "transaction: transaction checkpoint most recent duration for gathering all handles (usecs)",
  "transaction: transaction checkpoint most recent duration for gathering applied handles (usecs)",
  "transaction: transaction checkpoint most recent duration for gathering skipped handles (usecs)",
  "transaction: transaction checkpoint most recent handles applied",
  "transaction: transaction checkpoint most recent handles skipped",
  "transaction: transaction checkpoint most recent handles walked",
  "transaction: transaction checkpoint most recent time (msecs)",
  "transaction: transaction checkpoint prepare currently running",
  "transaction: transaction checkpoint prepare max time (msecs)",
  "transaction: transaction checkpoint prepare min time (msecs)",
  "transaction: transaction checkpoint prepare most recent time (msecs)",
  "transaction: transaction checkpoint prepare total time (msecs)",
  "transaction: transaction checkpoint scrub dirty target",
  "transaction: transaction checkpoint scrub time (msecs)",
  "transaction: transaction checkpoint total time (msecs)",
  "transaction: transaction checkpoints",
  "transaction: transaction checkpoints due to obsolete pages",
  "transaction: transaction checkpoints skipped because database was clean",
  "transaction: transaction failures due to history store",
  "transaction: transaction fsync calls for checkpoint after allocating the transaction ID",
  "transaction: transaction fsync duration for checkpoint after allocating the transaction ID "
  "(usecs)",
  "transaction: transaction range of IDs currently pinned",
  "transaction: transaction range of IDs currently pinned by a checkpoint",
  "transaction: transaction range of timestamps currently pinned",
  "transaction: transaction range of timestamps pinned by a checkpoint",
  "transaction: transaction range of timestamps pinned by the oldest active read timestamp",
  "transaction: transaction range of timestamps pinned by the oldest timestamp",
  "transaction: transaction read timestamp of the oldest active reader",
  "transaction: transaction rollback to stable currently running",
  "transaction: transaction walk of concurrent sessions",
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
__wt_stat_connection_init(WT_SESSION_IMPL *session, WT_CONNECTION_IMPL *handle)
{
    int i;

    WT_RET(__wt_calloc(
      session, (size_t)WT_COUNTER_SLOTS, sizeof(*handle->stat_array), &handle->stat_array));

    for (i = 0; i < WT_COUNTER_SLOTS; ++i) {
        handle->stats[i] = &handle->stat_array[i];
        __wt_stat_connection_init_single(handle->stats[i]);
    }
    return (0);
}

void
__wt_stat_connection_discard(WT_SESSION_IMPL *session, WT_CONNECTION_IMPL *handle)
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
    stats->block_cache_blocks_update = 0;
    stats->block_cache_bytes_update = 0;
    stats->block_cache_blocks_evicted = 0;
    stats->block_cache_bypass_filesize = 0;
    stats->block_cache_data_refs = 0;
    stats->block_cache_not_evicted_overhead = 0;
    stats->block_cache_bypass_writealloc = 0;
    stats->block_cache_bypass_overhead_put = 0;
    stats->block_cache_bypass_get = 0;
    stats->block_cache_bypass_put = 0;
    stats->block_cache_eviction_passes = 0;
    stats->block_cache_hits = 0;
    stats->block_cache_misses = 0;
    stats->block_cache_bypass_chkpt = 0;
    stats->block_cache_blocks_removed = 0;
    stats->block_cache_blocks = 0;
    stats->block_cache_blocks_insert_read = 0;
    stats->block_cache_blocks_insert_write = 0;
    stats->block_cache_bytes = 0;
    stats->block_cache_bytes_insert_read = 0;
    stats->block_cache_bytes_insert_write = 0;
    stats->block_preload = 0;
    stats->block_read = 0;
    stats->block_write = 0;
    stats->block_byte_read = 0;
    stats->block_byte_read_mmap = 0;
    stats->block_byte_read_syscall = 0;
    stats->block_byte_write = 0;
    stats->block_byte_write_checkpoint = 0;
    stats->block_byte_write_mmap = 0;
    stats->block_byte_write_syscall = 0;
    stats->block_map_read = 0;
    stats->block_byte_map_read = 0;
    stats->block_remap_file_resize = 0;
    stats->block_remap_file_write = 0;
    stats->cache_read_app_count = 0;
    stats->cache_read_app_time = 0;
    stats->cache_write_app_count = 0;
    stats->cache_write_app_time = 0;
    /* not clearing cache_bytes_updates */
    /* not clearing cache_bytes_image */
    /* not clearing cache_bytes_hs */
    /* not clearing cache_bytes_inuse */
    /* not clearing cache_bytes_dirty_total */
    /* not clearing cache_bytes_other */
    stats->cache_bytes_read = 0;
    stats->cache_bytes_write = 0;
    /* not clearing cache_lookaside_score */
    stats->cache_eviction_checkpoint = 0;
    stats->cache_eviction_blocked_checkpoint_hs = 0;
    stats->cache_eviction_get_ref = 0;
    stats->cache_eviction_get_ref_empty = 0;
    stats->cache_eviction_get_ref_empty2 = 0;
    /* not clearing cache_eviction_aggressive_set */
    /* not clearing cache_eviction_empty_score */
    stats->cache_eviction_blocked_ooo_checkpoint_race_1 = 0;
    stats->cache_eviction_blocked_ooo_checkpoint_race_2 = 0;
    stats->cache_eviction_blocked_ooo_checkpoint_race_3 = 0;
    stats->cache_eviction_blocked_ooo_checkpoint_race_4 = 0;
    stats->cache_eviction_walk_passes = 0;
    stats->cache_eviction_queue_empty = 0;
    stats->cache_eviction_queue_not_empty = 0;
    stats->cache_eviction_server_evicting = 0;
    stats->cache_eviction_server_slept = 0;
    stats->cache_eviction_slow = 0;
    stats->cache_eviction_walk_leaf_notfound = 0;
    /* not clearing cache_eviction_state */
    stats->cache_eviction_walk_sleeps = 0;
    stats->cache_eviction_target_page_lt10 = 0;
    stats->cache_eviction_target_page_lt32 = 0;
    stats->cache_eviction_target_page_ge128 = 0;
    stats->cache_eviction_target_page_lt64 = 0;
    stats->cache_eviction_target_page_lt128 = 0;
    stats->cache_eviction_target_page_reduced = 0;
    stats->cache_eviction_target_strategy_both_clean_and_dirty = 0;
    stats->cache_eviction_target_strategy_clean = 0;
    stats->cache_eviction_target_strategy_dirty = 0;
    stats->cache_eviction_walks_abandoned = 0;
    stats->cache_eviction_walks_stopped = 0;
    stats->cache_eviction_walks_gave_up_no_targets = 0;
    stats->cache_eviction_walks_gave_up_ratio = 0;
    stats->cache_eviction_walks_ended = 0;
    stats->cache_eviction_walk_restart = 0;
    stats->cache_eviction_walk_from_root = 0;
    stats->cache_eviction_walk_saved_pos = 0;
    /* not clearing cache_eviction_active_workers */
    stats->cache_eviction_worker_created = 0;
    stats->cache_eviction_worker_evicting = 0;
    stats->cache_eviction_worker_removed = 0;
    /* not clearing cache_eviction_stable_state_workers */
    /* not clearing cache_eviction_walks_active */
    stats->cache_eviction_walks_started = 0;
    stats->cache_eviction_force_retune = 0;
    stats->cache_eviction_force_hs_fail = 0;
    stats->cache_eviction_force_hs = 0;
    stats->cache_eviction_force_hs_success = 0;
    stats->cache_eviction_force_clean = 0;
    stats->cache_eviction_force_clean_time = 0;
    stats->cache_eviction_force_dirty = 0;
    stats->cache_eviction_force_dirty_time = 0;
    stats->cache_eviction_force_long_update_list = 0;
    stats->cache_eviction_force_delete = 0;
    stats->cache_eviction_force = 0;
    stats->cache_eviction_force_fail = 0;
    stats->cache_eviction_force_fail_time = 0;
    stats->cache_eviction_hazard = 0;
    stats->cache_hazard_checks = 0;
    stats->cache_hazard_walks = 0;
    stats->cache_hazard_max = 0;
    /* not clearing cache_hs_score */
    stats->cache_hs_insert = 0;
    stats->cache_hs_insert_restart = 0;
    /* not clearing cache_hs_ondisk_max */
    /* not clearing cache_hs_ondisk */
    stats->cache_hs_order_lose_durable_timestamp = 0;
    stats->cache_hs_order_reinsert = 0;
    stats->cache_hs_read = 0;
    stats->cache_hs_read_miss = 0;
    stats->cache_hs_read_squash = 0;
    stats->cache_hs_key_truncate_rts_unstable = 0;
    stats->cache_hs_key_truncate_rts = 0;
    stats->cache_hs_key_truncate = 0;
    stats->cache_hs_key_truncate_onpage_removal = 0;
    stats->cache_hs_order_remove = 0;
    stats->cache_hs_write_squash = 0;
    stats->cache_inmem_splittable = 0;
    stats->cache_inmem_split = 0;
    stats->cache_eviction_internal = 0;
    stats->cache_eviction_internal_pages_queued = 0;
    stats->cache_eviction_internal_pages_seen = 0;
    stats->cache_eviction_internal_pages_already_queued = 0;
    stats->cache_eviction_split_internal = 0;
    stats->cache_eviction_split_leaf = 0;
    /* not clearing cache_bytes_max */
    /* not clearing cache_eviction_maximum_page_size */
    stats->cache_eviction_dirty = 0;
    stats->cache_eviction_app_dirty = 0;
    stats->cache_timed_out_ops = 0;
    stats->cache_read_overflow = 0;
    stats->cache_eviction_deepen = 0;
    stats->cache_write_hs = 0;
    /* not clearing cache_pages_inuse */
    stats->cache_eviction_app = 0;
    stats->cache_eviction_pages_in_parallel_with_checkpoint = 0;
    stats->cache_eviction_pages_queued = 0;
    stats->cache_eviction_pages_queued_post_lru = 0;
    stats->cache_eviction_pages_queued_urgent = 0;
    stats->cache_eviction_pages_queued_oldest = 0;
    stats->cache_eviction_pages_queued_urgent_hs_dirty = 0;
    stats->cache_read = 0;
    stats->cache_read_deleted = 0;
    stats->cache_read_deleted_prepared = 0;
    stats->cache_pages_requested = 0;
    stats->cache_eviction_pages_seen = 0;
    stats->cache_eviction_pages_already_queued = 0;
    stats->cache_eviction_fail = 0;
    stats->cache_eviction_fail_active_children_on_an_internal_page = 0;
    stats->cache_eviction_fail_in_reconciliation = 0;
    stats->cache_eviction_fail_checkpoint_out_of_order_ts = 0;
    stats->cache_eviction_walk = 0;
    stats->cache_write = 0;
    stats->cache_write_restore = 0;
    /* not clearing cache_overhead */
    stats->cache_hs_insert_full_update = 0;
    stats->cache_hs_insert_reverse_modify = 0;
    /* not clearing cache_bytes_internal */
    /* not clearing cache_bytes_leaf */
    /* not clearing cache_bytes_dirty */
    /* not clearing cache_pages_dirty */
    stats->cache_eviction_clean = 0;
    stats->fsync_all_fh_total = 0;
    stats->fsync_all_fh = 0;
    /* not clearing fsync_all_time */
    stats->capacity_bytes_read = 0;
    stats->capacity_bytes_ckpt = 0;
    stats->capacity_bytes_evict = 0;
    stats->capacity_bytes_log = 0;
    stats->capacity_bytes_written = 0;
    stats->capacity_threshold = 0;
    stats->capacity_time_total = 0;
    stats->capacity_time_ckpt = 0;
    stats->capacity_time_evict = 0;
    stats->capacity_time_log = 0;
    stats->capacity_time_read = 0;
    stats->cc_pages_evict = 0;
    stats->cc_pages_removed = 0;
    stats->cc_pages_walk_skipped = 0;
    stats->cc_pages_visited = 0;
    stats->cond_auto_wait_reset = 0;
    stats->cond_auto_wait = 0;
    stats->cond_auto_wait_skipped = 0;
    stats->time_travel = 0;
    /* not clearing file_open */
    /* not clearing buckets_dh */
    /* not clearing buckets */
    stats->memory_allocation = 0;
    stats->memory_free = 0;
    stats->memory_grow = 0;
    stats->cond_wait = 0;
    stats->rwlock_read = 0;
    stats->rwlock_write = 0;
    stats->fsync_io = 0;
    stats->read_io = 0;
    stats->write_io = 0;
    stats->cursor_next_skip_total = 0;
    stats->cursor_prev_skip_total = 0;
    stats->cursor_skip_hs_cur_position = 0;
    stats->cursor_search_near_prefix_fast_paths = 0;
    /* not clearing cursor_cached_count */
    stats->cursor_insert_bulk = 0;
    stats->cursor_cache = 0;
    stats->cursor_create = 0;
    stats->cursor_insert = 0;
    stats->cursor_insert_bytes = 0;
    stats->cursor_modify = 0;
    stats->cursor_modify_bytes = 0;
    stats->cursor_modify_bytes_touch = 0;
    stats->cursor_next = 0;
    stats->cursor_next_hs_tombstone = 0;
    stats->cursor_next_skip_ge_100 = 0;
    stats->cursor_next_skip_lt_100 = 0;
    stats->cursor_restart = 0;
    stats->cursor_prev = 0;
    stats->cursor_prev_hs_tombstone = 0;
    stats->cursor_prev_skip_ge_100 = 0;
    stats->cursor_prev_skip_lt_100 = 0;
    stats->cursor_remove = 0;
    stats->cursor_remove_bytes = 0;
    stats->cursor_reserve = 0;
    stats->cursor_reset = 0;
    stats->cursor_search = 0;
    stats->cursor_search_hs = 0;
    stats->cursor_search_near = 0;
    stats->cursor_sweep_buckets = 0;
    stats->cursor_sweep_closed = 0;
    stats->cursor_sweep_examined = 0;
    stats->cursor_sweep = 0;
    stats->cursor_truncate = 0;
    stats->cursor_update = 0;
    stats->cursor_update_bytes = 0;
    stats->cursor_update_bytes_changed = 0;
    stats->cursor_reopen = 0;
    /* not clearing cursor_open_count */
    /* not clearing dh_conn_handle_size */
    /* not clearing dh_conn_handle_count */
    stats->dh_sweep_ref = 0;
    stats->dh_sweep_close = 0;
    stats->dh_sweep_remove = 0;
    stats->dh_sweep_tod = 0;
    stats->dh_sweeps = 0;
    stats->dh_sweep_skip_ckpt = 0;
    stats->dh_session_handles = 0;
    stats->dh_session_sweeps = 0;
    stats->lock_checkpoint_count = 0;
    stats->lock_checkpoint_wait_application = 0;
    stats->lock_checkpoint_wait_internal = 0;
    stats->lock_dhandle_wait_application = 0;
    stats->lock_dhandle_wait_internal = 0;
    stats->lock_dhandle_read_count = 0;
    stats->lock_dhandle_write_count = 0;
    stats->lock_durable_timestamp_wait_application = 0;
    stats->lock_durable_timestamp_wait_internal = 0;
    stats->lock_durable_timestamp_read_count = 0;
    stats->lock_durable_timestamp_write_count = 0;
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
    stats->rec_time_window_bytes_ts = 0;
    stats->rec_time_window_bytes_txn = 0;
    stats->rec_page_delete_fast = 0;
    stats->rec_overflow_key_leaf = 0;
    /* not clearing rec_maximum_seconds */
    stats->rec_pages = 0;
    stats->rec_pages_eviction = 0;
    stats->rec_pages_with_prepare = 0;
    stats->rec_pages_with_ts = 0;
    stats->rec_pages_with_txn = 0;
    stats->rec_page_delete = 0;
    stats->rec_time_aggr_newest_start_durable_ts = 0;
    stats->rec_time_aggr_newest_stop_durable_ts = 0;
    stats->rec_time_aggr_newest_stop_ts = 0;
    stats->rec_time_aggr_newest_stop_txn = 0;
    stats->rec_time_aggr_newest_txn = 0;
    stats->rec_time_aggr_oldest_start_ts = 0;
    stats->rec_time_aggr_prepared = 0;
    stats->rec_time_window_pages_prepared = 0;
    stats->rec_time_window_pages_durable_start_ts = 0;
    stats->rec_time_window_pages_start_ts = 0;
    stats->rec_time_window_pages_start_txn = 0;
    stats->rec_time_window_pages_durable_stop_ts = 0;
    stats->rec_time_window_pages_stop_ts = 0;
    stats->rec_time_window_pages_stop_txn = 0;
    stats->rec_time_window_prepared = 0;
    stats->rec_time_window_durable_start_ts = 0;
    stats->rec_time_window_start_ts = 0;
    stats->rec_time_window_start_txn = 0;
    stats->rec_time_window_durable_stop_ts = 0;
    stats->rec_time_window_stop_ts = 0;
    stats->rec_time_window_stop_txn = 0;
    /* not clearing rec_split_stashed_bytes */
    /* not clearing rec_split_stashed_objects */
    stats->local_objects_inuse = 0;
    stats->flush_tier = 0;
    stats->local_objects_removed = 0;
    /* not clearing session_open */
    stats->session_query_ts = 0;
    /* not clearing session_table_alter_fail */
    /* not clearing session_table_alter_success */
    /* not clearing session_table_alter_trigger_checkpoint */
    /* not clearing session_table_alter_skip */
    /* not clearing session_table_compact_fail */
    /* not clearing session_table_compact_fail_cache_pressure */
    /* not clearing session_table_compact_running */
    /* not clearing session_table_compact_skipped */
    /* not clearing session_table_compact_success */
    /* not clearing session_table_compact_timeout */
    /* not clearing session_table_create_fail */
    /* not clearing session_table_create_success */
    /* not clearing session_table_drop_fail */
    /* not clearing session_table_drop_success */
    /* not clearing session_table_rename_fail */
    /* not clearing session_table_rename_success */
    /* not clearing session_table_salvage_fail */
    /* not clearing session_table_salvage_success */
    /* not clearing session_table_truncate_fail */
    /* not clearing session_table_truncate_success */
    /* not clearing session_table_verify_fail */
    /* not clearing session_table_verify_success */
    stats->tiered_work_units_dequeued = 0;
    stats->tiered_work_units_created = 0;
    /* not clearing tiered_retention */
    /* not clearing tiered_object_size */
    /* not clearing thread_fsync_active */
    /* not clearing thread_read_active */
    /* not clearing thread_write_active */
    stats->application_evict_time = 0;
    stats->application_cache_time = 0;
    stats->txn_release_blocked = 0;
    stats->conn_close_blocked_lsm = 0;
    stats->dhandle_lock_blocked = 0;
    stats->page_index_slot_ref_blocked = 0;
    stats->prepared_transition_blocked_page = 0;
    stats->page_busy_blocked = 0;
    stats->page_forcible_evict_blocked = 0;
    stats->page_locked_blocked = 0;
    stats->page_read_blocked = 0;
    stats->page_sleep = 0;
    stats->page_del_rollback_blocked = 0;
    stats->child_modify_blocked_page = 0;
    stats->txn_prepared_updates = 0;
    stats->txn_prepared_updates_committed = 0;
    stats->txn_prepared_updates_key_repeated = 0;
    stats->txn_prepared_updates_rolledback = 0;
    stats->txn_prepare = 0;
    stats->txn_prepare_commit = 0;
    stats->txn_prepare_active = 0;
    stats->txn_prepare_rollback = 0;
    stats->txn_prepare_rollback_do_not_remove_hs_update = 0;
    stats->txn_prepare_rollback_fix_hs_update_with_ckpt_reserved_txnid = 0;
    stats->txn_query_ts = 0;
    stats->txn_read_race_prepare_update = 0;
    stats->txn_rts = 0;
    stats->txn_rts_hs_stop_older_than_newer_start = 0;
    stats->txn_rts_inconsistent_ckpt = 0;
    stats->txn_rts_keys_removed = 0;
    stats->txn_rts_keys_restored = 0;
    stats->txn_rts_pages_visited = 0;
    stats->txn_rts_hs_restore_tombstones = 0;
    stats->txn_rts_hs_restore_updates = 0;
    stats->txn_rts_delete_rle_skipped = 0;
    stats->txn_rts_stable_rle_skipped = 0;
    stats->txn_rts_sweep_hs_keys = 0;
    stats->txn_rts_tree_walk_skip_pages = 0;
    stats->txn_rts_upd_aborted = 0;
    stats->txn_rts_hs_removed = 0;
    stats->txn_sessions_walked = 0;
    stats->txn_set_ts = 0;
    stats->txn_set_ts_durable = 0;
    stats->txn_set_ts_durable_upd = 0;
    stats->txn_set_ts_oldest = 0;
    stats->txn_set_ts_oldest_upd = 0;
    stats->txn_set_ts_stable = 0;
    stats->txn_set_ts_stable_upd = 0;
    stats->txn_begin = 0;
    /* not clearing txn_checkpoint_running */
    /* not clearing txn_checkpoint_running_hs */
    /* not clearing txn_checkpoint_generation */
    stats->txn_hs_ckpt_duration = 0;
    /* not clearing txn_checkpoint_time_max */
    /* not clearing txn_checkpoint_time_min */
    /* not clearing txn_checkpoint_handle_duration */
    /* not clearing txn_checkpoint_handle_duration_apply */
    /* not clearing txn_checkpoint_handle_duration_skip */
    stats->txn_checkpoint_handle_applied = 0;
    stats->txn_checkpoint_handle_skipped = 0;
    stats->txn_checkpoint_handle_walked = 0;
    /* not clearing txn_checkpoint_time_recent */
    /* not clearing txn_checkpoint_prep_running */
    /* not clearing txn_checkpoint_prep_max */
    /* not clearing txn_checkpoint_prep_min */
    /* not clearing txn_checkpoint_prep_recent */
    /* not clearing txn_checkpoint_prep_total */
    /* not clearing txn_checkpoint_scrub_target */
    /* not clearing txn_checkpoint_scrub_time */
    /* not clearing txn_checkpoint_time_total */
    stats->txn_checkpoint = 0;
    stats->txn_checkpoint_obsolete_applied = 0;
    stats->txn_checkpoint_skipped = 0;
    stats->txn_fail_cache = 0;
    stats->txn_checkpoint_fsync_post = 0;
    /* not clearing txn_checkpoint_fsync_post_duration */
    /* not clearing txn_pinned_range */
    /* not clearing txn_pinned_checkpoint_range */
    /* not clearing txn_pinned_timestamp */
    /* not clearing txn_pinned_timestamp_checkpoint */
    /* not clearing txn_pinned_timestamp_reader */
    /* not clearing txn_pinned_timestamp_oldest */
    /* not clearing txn_timestamp_oldest_active_read */
    /* not clearing txn_rollback_to_stable_running */
    stats->txn_walk_sessions = 0;
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
__wt_stat_connection_aggregate(WT_CONNECTION_STATS **from, WT_CONNECTION_STATS *to)
{
    int64_t v;

    to->lsm_work_queue_app += WT_STAT_READ(from, lsm_work_queue_app);
    to->lsm_work_queue_manager += WT_STAT_READ(from, lsm_work_queue_manager);
    to->lsm_rows_merged += WT_STAT_READ(from, lsm_rows_merged);
    to->lsm_checkpoint_throttle += WT_STAT_READ(from, lsm_checkpoint_throttle);
    to->lsm_merge_throttle += WT_STAT_READ(from, lsm_merge_throttle);
    to->lsm_work_queue_switch += WT_STAT_READ(from, lsm_work_queue_switch);
    to->lsm_work_units_discarded += WT_STAT_READ(from, lsm_work_units_discarded);
    to->lsm_work_units_done += WT_STAT_READ(from, lsm_work_units_done);
    to->lsm_work_units_created += WT_STAT_READ(from, lsm_work_units_created);
    to->lsm_work_queue_max += WT_STAT_READ(from, lsm_work_queue_max);
    to->block_cache_blocks_update += WT_STAT_READ(from, block_cache_blocks_update);
    to->block_cache_bytes_update += WT_STAT_READ(from, block_cache_bytes_update);
    to->block_cache_blocks_evicted += WT_STAT_READ(from, block_cache_blocks_evicted);
    to->block_cache_bypass_filesize += WT_STAT_READ(from, block_cache_bypass_filesize);
    to->block_cache_data_refs += WT_STAT_READ(from, block_cache_data_refs);
    to->block_cache_not_evicted_overhead += WT_STAT_READ(from, block_cache_not_evicted_overhead);
    to->block_cache_bypass_writealloc += WT_STAT_READ(from, block_cache_bypass_writealloc);
    to->block_cache_bypass_overhead_put += WT_STAT_READ(from, block_cache_bypass_overhead_put);
    to->block_cache_bypass_get += WT_STAT_READ(from, block_cache_bypass_get);
    to->block_cache_bypass_put += WT_STAT_READ(from, block_cache_bypass_put);
    to->block_cache_eviction_passes += WT_STAT_READ(from, block_cache_eviction_passes);
    to->block_cache_hits += WT_STAT_READ(from, block_cache_hits);
    to->block_cache_misses += WT_STAT_READ(from, block_cache_misses);
    to->block_cache_bypass_chkpt += WT_STAT_READ(from, block_cache_bypass_chkpt);
    to->block_cache_blocks_removed += WT_STAT_READ(from, block_cache_blocks_removed);
    to->block_cache_blocks += WT_STAT_READ(from, block_cache_blocks);
    to->block_cache_blocks_insert_read += WT_STAT_READ(from, block_cache_blocks_insert_read);
    to->block_cache_blocks_insert_write += WT_STAT_READ(from, block_cache_blocks_insert_write);
    to->block_cache_bytes += WT_STAT_READ(from, block_cache_bytes);
    to->block_cache_bytes_insert_read += WT_STAT_READ(from, block_cache_bytes_insert_read);
    to->block_cache_bytes_insert_write += WT_STAT_READ(from, block_cache_bytes_insert_write);
    to->block_preload += WT_STAT_READ(from, block_preload);
    to->block_read += WT_STAT_READ(from, block_read);
    to->block_write += WT_STAT_READ(from, block_write);
    to->block_byte_read += WT_STAT_READ(from, block_byte_read);
    to->block_byte_read_mmap += WT_STAT_READ(from, block_byte_read_mmap);
    to->block_byte_read_syscall += WT_STAT_READ(from, block_byte_read_syscall);
    to->block_byte_write += WT_STAT_READ(from, block_byte_write);
    to->block_byte_write_checkpoint += WT_STAT_READ(from, block_byte_write_checkpoint);
    to->block_byte_write_mmap += WT_STAT_READ(from, block_byte_write_mmap);
    to->block_byte_write_syscall += WT_STAT_READ(from, block_byte_write_syscall);
    to->block_map_read += WT_STAT_READ(from, block_map_read);
    to->block_byte_map_read += WT_STAT_READ(from, block_byte_map_read);
    to->block_remap_file_resize += WT_STAT_READ(from, block_remap_file_resize);
    to->block_remap_file_write += WT_STAT_READ(from, block_remap_file_write);
    to->cache_read_app_count += WT_STAT_READ(from, cache_read_app_count);
    to->cache_read_app_time += WT_STAT_READ(from, cache_read_app_time);
    to->cache_write_app_count += WT_STAT_READ(from, cache_write_app_count);
    to->cache_write_app_time += WT_STAT_READ(from, cache_write_app_time);
    to->cache_bytes_updates += WT_STAT_READ(from, cache_bytes_updates);
    to->cache_bytes_image += WT_STAT_READ(from, cache_bytes_image);
    to->cache_bytes_hs += WT_STAT_READ(from, cache_bytes_hs);
    to->cache_bytes_inuse += WT_STAT_READ(from, cache_bytes_inuse);
    to->cache_bytes_dirty_total += WT_STAT_READ(from, cache_bytes_dirty_total);
    to->cache_bytes_other += WT_STAT_READ(from, cache_bytes_other);
    to->cache_bytes_read += WT_STAT_READ(from, cache_bytes_read);
    to->cache_bytes_write += WT_STAT_READ(from, cache_bytes_write);
    to->cache_lookaside_score += WT_STAT_READ(from, cache_lookaside_score);
    to->cache_eviction_checkpoint += WT_STAT_READ(from, cache_eviction_checkpoint);
    to->cache_eviction_blocked_checkpoint_hs +=
      WT_STAT_READ(from, cache_eviction_blocked_checkpoint_hs);
    to->cache_eviction_get_ref += WT_STAT_READ(from, cache_eviction_get_ref);
    to->cache_eviction_get_ref_empty += WT_STAT_READ(from, cache_eviction_get_ref_empty);
    to->cache_eviction_get_ref_empty2 += WT_STAT_READ(from, cache_eviction_get_ref_empty2);
    to->cache_eviction_aggressive_set += WT_STAT_READ(from, cache_eviction_aggressive_set);
    to->cache_eviction_empty_score += WT_STAT_READ(from, cache_eviction_empty_score);
    to->cache_eviction_blocked_ooo_checkpoint_race_1 +=
      WT_STAT_READ(from, cache_eviction_blocked_ooo_checkpoint_race_1);
    to->cache_eviction_blocked_ooo_checkpoint_race_2 +=
      WT_STAT_READ(from, cache_eviction_blocked_ooo_checkpoint_race_2);
    to->cache_eviction_blocked_ooo_checkpoint_race_3 +=
      WT_STAT_READ(from, cache_eviction_blocked_ooo_checkpoint_race_3);
    to->cache_eviction_blocked_ooo_checkpoint_race_4 +=
      WT_STAT_READ(from, cache_eviction_blocked_ooo_checkpoint_race_4);
    to->cache_eviction_walk_passes += WT_STAT_READ(from, cache_eviction_walk_passes);
    to->cache_eviction_queue_empty += WT_STAT_READ(from, cache_eviction_queue_empty);
    to->cache_eviction_queue_not_empty += WT_STAT_READ(from, cache_eviction_queue_not_empty);
    to->cache_eviction_server_evicting += WT_STAT_READ(from, cache_eviction_server_evicting);
    to->cache_eviction_server_slept += WT_STAT_READ(from, cache_eviction_server_slept);
    to->cache_eviction_slow += WT_STAT_READ(from, cache_eviction_slow);
    to->cache_eviction_walk_leaf_notfound += WT_STAT_READ(from, cache_eviction_walk_leaf_notfound);
    to->cache_eviction_state += WT_STAT_READ(from, cache_eviction_state);
    to->cache_eviction_walk_sleeps += WT_STAT_READ(from, cache_eviction_walk_sleeps);
    to->cache_eviction_target_page_lt10 += WT_STAT_READ(from, cache_eviction_target_page_lt10);
    to->cache_eviction_target_page_lt32 += WT_STAT_READ(from, cache_eviction_target_page_lt32);
    to->cache_eviction_target_page_ge128 += WT_STAT_READ(from, cache_eviction_target_page_ge128);
    to->cache_eviction_target_page_lt64 += WT_STAT_READ(from, cache_eviction_target_page_lt64);
    to->cache_eviction_target_page_lt128 += WT_STAT_READ(from, cache_eviction_target_page_lt128);
    to->cache_eviction_target_page_reduced +=
      WT_STAT_READ(from, cache_eviction_target_page_reduced);
    to->cache_eviction_target_strategy_both_clean_and_dirty +=
      WT_STAT_READ(from, cache_eviction_target_strategy_both_clean_and_dirty);
    to->cache_eviction_target_strategy_clean +=
      WT_STAT_READ(from, cache_eviction_target_strategy_clean);
    to->cache_eviction_target_strategy_dirty +=
      WT_STAT_READ(from, cache_eviction_target_strategy_dirty);
    to->cache_eviction_walks_abandoned += WT_STAT_READ(from, cache_eviction_walks_abandoned);
    to->cache_eviction_walks_stopped += WT_STAT_READ(from, cache_eviction_walks_stopped);
    to->cache_eviction_walks_gave_up_no_targets +=
      WT_STAT_READ(from, cache_eviction_walks_gave_up_no_targets);
    to->cache_eviction_walks_gave_up_ratio +=
      WT_STAT_READ(from, cache_eviction_walks_gave_up_ratio);
    to->cache_eviction_walks_ended += WT_STAT_READ(from, cache_eviction_walks_ended);
    to->cache_eviction_walk_restart += WT_STAT_READ(from, cache_eviction_walk_restart);
    to->cache_eviction_walk_from_root += WT_STAT_READ(from, cache_eviction_walk_from_root);
    to->cache_eviction_walk_saved_pos += WT_STAT_READ(from, cache_eviction_walk_saved_pos);
    to->cache_eviction_active_workers += WT_STAT_READ(from, cache_eviction_active_workers);
    to->cache_eviction_worker_created += WT_STAT_READ(from, cache_eviction_worker_created);
    to->cache_eviction_worker_evicting += WT_STAT_READ(from, cache_eviction_worker_evicting);
    to->cache_eviction_worker_removed += WT_STAT_READ(from, cache_eviction_worker_removed);
    to->cache_eviction_stable_state_workers +=
      WT_STAT_READ(from, cache_eviction_stable_state_workers);
    to->cache_eviction_walks_active += WT_STAT_READ(from, cache_eviction_walks_active);
    to->cache_eviction_walks_started += WT_STAT_READ(from, cache_eviction_walks_started);
    to->cache_eviction_force_retune += WT_STAT_READ(from, cache_eviction_force_retune);
    to->cache_eviction_force_hs_fail += WT_STAT_READ(from, cache_eviction_force_hs_fail);
    to->cache_eviction_force_hs += WT_STAT_READ(from, cache_eviction_force_hs);
    to->cache_eviction_force_hs_success += WT_STAT_READ(from, cache_eviction_force_hs_success);
    to->cache_eviction_force_clean += WT_STAT_READ(from, cache_eviction_force_clean);
    to->cache_eviction_force_clean_time += WT_STAT_READ(from, cache_eviction_force_clean_time);
    to->cache_eviction_force_dirty += WT_STAT_READ(from, cache_eviction_force_dirty);
    to->cache_eviction_force_dirty_time += WT_STAT_READ(from, cache_eviction_force_dirty_time);
    to->cache_eviction_force_long_update_list +=
      WT_STAT_READ(from, cache_eviction_force_long_update_list);
    to->cache_eviction_force_delete += WT_STAT_READ(from, cache_eviction_force_delete);
    to->cache_eviction_force += WT_STAT_READ(from, cache_eviction_force);
    to->cache_eviction_force_fail += WT_STAT_READ(from, cache_eviction_force_fail);
    to->cache_eviction_force_fail_time += WT_STAT_READ(from, cache_eviction_force_fail_time);
    to->cache_eviction_hazard += WT_STAT_READ(from, cache_eviction_hazard);
    to->cache_hazard_checks += WT_STAT_READ(from, cache_hazard_checks);
    to->cache_hazard_walks += WT_STAT_READ(from, cache_hazard_walks);
    if ((v = WT_STAT_READ(from, cache_hazard_max)) > to->cache_hazard_max)
        to->cache_hazard_max = v;
    to->cache_hs_score += WT_STAT_READ(from, cache_hs_score);
    to->cache_hs_insert += WT_STAT_READ(from, cache_hs_insert);
    to->cache_hs_insert_restart += WT_STAT_READ(from, cache_hs_insert_restart);
    to->cache_hs_ondisk_max += WT_STAT_READ(from, cache_hs_ondisk_max);
    to->cache_hs_ondisk += WT_STAT_READ(from, cache_hs_ondisk);
    to->cache_hs_order_lose_durable_timestamp +=
      WT_STAT_READ(from, cache_hs_order_lose_durable_timestamp);
    to->cache_hs_order_reinsert += WT_STAT_READ(from, cache_hs_order_reinsert);
    to->cache_hs_read += WT_STAT_READ(from, cache_hs_read);
    to->cache_hs_read_miss += WT_STAT_READ(from, cache_hs_read_miss);
    to->cache_hs_read_squash += WT_STAT_READ(from, cache_hs_read_squash);
    to->cache_hs_key_truncate_rts_unstable +=
      WT_STAT_READ(from, cache_hs_key_truncate_rts_unstable);
    to->cache_hs_key_truncate_rts += WT_STAT_READ(from, cache_hs_key_truncate_rts);
    to->cache_hs_key_truncate += WT_STAT_READ(from, cache_hs_key_truncate);
    to->cache_hs_key_truncate_onpage_removal +=
      WT_STAT_READ(from, cache_hs_key_truncate_onpage_removal);
    to->cache_hs_order_remove += WT_STAT_READ(from, cache_hs_order_remove);
    to->cache_hs_write_squash += WT_STAT_READ(from, cache_hs_write_squash);
    to->cache_inmem_splittable += WT_STAT_READ(from, cache_inmem_splittable);
    to->cache_inmem_split += WT_STAT_READ(from, cache_inmem_split);
    to->cache_eviction_internal += WT_STAT_READ(from, cache_eviction_internal);
    to->cache_eviction_internal_pages_queued +=
      WT_STAT_READ(from, cache_eviction_internal_pages_queued);
    to->cache_eviction_internal_pages_seen +=
      WT_STAT_READ(from, cache_eviction_internal_pages_seen);
    to->cache_eviction_internal_pages_already_queued +=
      WT_STAT_READ(from, cache_eviction_internal_pages_already_queued);
    to->cache_eviction_split_internal += WT_STAT_READ(from, cache_eviction_split_internal);
    to->cache_eviction_split_leaf += WT_STAT_READ(from, cache_eviction_split_leaf);
    to->cache_bytes_max += WT_STAT_READ(from, cache_bytes_max);
    to->cache_eviction_maximum_page_size += WT_STAT_READ(from, cache_eviction_maximum_page_size);
    to->cache_eviction_dirty += WT_STAT_READ(from, cache_eviction_dirty);
    to->cache_eviction_app_dirty += WT_STAT_READ(from, cache_eviction_app_dirty);
    to->cache_timed_out_ops += WT_STAT_READ(from, cache_timed_out_ops);
    to->cache_read_overflow += WT_STAT_READ(from, cache_read_overflow);
    to->cache_eviction_deepen += WT_STAT_READ(from, cache_eviction_deepen);
    to->cache_write_hs += WT_STAT_READ(from, cache_write_hs);
    to->cache_pages_inuse += WT_STAT_READ(from, cache_pages_inuse);
    to->cache_eviction_app += WT_STAT_READ(from, cache_eviction_app);
    to->cache_eviction_pages_in_parallel_with_checkpoint +=
      WT_STAT_READ(from, cache_eviction_pages_in_parallel_with_checkpoint);
    to->cache_eviction_pages_queued += WT_STAT_READ(from, cache_eviction_pages_queued);
    to->cache_eviction_pages_queued_post_lru +=
      WT_STAT_READ(from, cache_eviction_pages_queued_post_lru);
    to->cache_eviction_pages_queued_urgent +=
      WT_STAT_READ(from, cache_eviction_pages_queued_urgent);
    to->cache_eviction_pages_queued_oldest +=
      WT_STAT_READ(from, cache_eviction_pages_queued_oldest);
    to->cache_eviction_pages_queued_urgent_hs_dirty +=
      WT_STAT_READ(from, cache_eviction_pages_queued_urgent_hs_dirty);
    to->cache_read += WT_STAT_READ(from, cache_read);
    to->cache_read_deleted += WT_STAT_READ(from, cache_read_deleted);
    to->cache_read_deleted_prepared += WT_STAT_READ(from, cache_read_deleted_prepared);
    to->cache_pages_requested += WT_STAT_READ(from, cache_pages_requested);
    to->cache_eviction_pages_seen += WT_STAT_READ(from, cache_eviction_pages_seen);
    to->cache_eviction_pages_already_queued +=
      WT_STAT_READ(from, cache_eviction_pages_already_queued);
    to->cache_eviction_fail += WT_STAT_READ(from, cache_eviction_fail);
    to->cache_eviction_fail_active_children_on_an_internal_page +=
      WT_STAT_READ(from, cache_eviction_fail_active_children_on_an_internal_page);
    to->cache_eviction_fail_in_reconciliation +=
      WT_STAT_READ(from, cache_eviction_fail_in_reconciliation);
    to->cache_eviction_fail_checkpoint_out_of_order_ts +=
      WT_STAT_READ(from, cache_eviction_fail_checkpoint_out_of_order_ts);
    to->cache_eviction_walk += WT_STAT_READ(from, cache_eviction_walk);
    to->cache_write += WT_STAT_READ(from, cache_write);
    to->cache_write_restore += WT_STAT_READ(from, cache_write_restore);
    to->cache_overhead += WT_STAT_READ(from, cache_overhead);
    to->cache_hs_insert_full_update += WT_STAT_READ(from, cache_hs_insert_full_update);
    to->cache_hs_insert_reverse_modify += WT_STAT_READ(from, cache_hs_insert_reverse_modify);
    to->cache_bytes_internal += WT_STAT_READ(from, cache_bytes_internal);
    to->cache_bytes_leaf += WT_STAT_READ(from, cache_bytes_leaf);
    to->cache_bytes_dirty += WT_STAT_READ(from, cache_bytes_dirty);
    to->cache_pages_dirty += WT_STAT_READ(from, cache_pages_dirty);
    to->cache_eviction_clean += WT_STAT_READ(from, cache_eviction_clean);
    to->fsync_all_fh_total += WT_STAT_READ(from, fsync_all_fh_total);
    to->fsync_all_fh += WT_STAT_READ(from, fsync_all_fh);
    to->fsync_all_time += WT_STAT_READ(from, fsync_all_time);
    to->capacity_bytes_read += WT_STAT_READ(from, capacity_bytes_read);
    to->capacity_bytes_ckpt += WT_STAT_READ(from, capacity_bytes_ckpt);
    to->capacity_bytes_evict += WT_STAT_READ(from, capacity_bytes_evict);
    to->capacity_bytes_log += WT_STAT_READ(from, capacity_bytes_log);
    to->capacity_bytes_written += WT_STAT_READ(from, capacity_bytes_written);
    to->capacity_threshold += WT_STAT_READ(from, capacity_threshold);
    to->capacity_time_total += WT_STAT_READ(from, capacity_time_total);
    to->capacity_time_ckpt += WT_STAT_READ(from, capacity_time_ckpt);
    to->capacity_time_evict += WT_STAT_READ(from, capacity_time_evict);
    to->capacity_time_log += WT_STAT_READ(from, capacity_time_log);
    to->capacity_time_read += WT_STAT_READ(from, capacity_time_read);
    to->cc_pages_evict += WT_STAT_READ(from, cc_pages_evict);
    to->cc_pages_removed += WT_STAT_READ(from, cc_pages_removed);
    to->cc_pages_walk_skipped += WT_STAT_READ(from, cc_pages_walk_skipped);
    to->cc_pages_visited += WT_STAT_READ(from, cc_pages_visited);
    to->cond_auto_wait_reset += WT_STAT_READ(from, cond_auto_wait_reset);
    to->cond_auto_wait += WT_STAT_READ(from, cond_auto_wait);
    to->cond_auto_wait_skipped += WT_STAT_READ(from, cond_auto_wait_skipped);
    to->time_travel += WT_STAT_READ(from, time_travel);
    to->file_open += WT_STAT_READ(from, file_open);
    to->buckets_dh += WT_STAT_READ(from, buckets_dh);
    to->buckets += WT_STAT_READ(from, buckets);
    to->memory_allocation += WT_STAT_READ(from, memory_allocation);
    to->memory_free += WT_STAT_READ(from, memory_free);
    to->memory_grow += WT_STAT_READ(from, memory_grow);
    to->cond_wait += WT_STAT_READ(from, cond_wait);
    to->rwlock_read += WT_STAT_READ(from, rwlock_read);
    to->rwlock_write += WT_STAT_READ(from, rwlock_write);
    to->fsync_io += WT_STAT_READ(from, fsync_io);
    to->read_io += WT_STAT_READ(from, read_io);
    to->write_io += WT_STAT_READ(from, write_io);
    to->cursor_next_skip_total += WT_STAT_READ(from, cursor_next_skip_total);
    to->cursor_prev_skip_total += WT_STAT_READ(from, cursor_prev_skip_total);
    to->cursor_skip_hs_cur_position += WT_STAT_READ(from, cursor_skip_hs_cur_position);
    to->cursor_search_near_prefix_fast_paths +=
      WT_STAT_READ(from, cursor_search_near_prefix_fast_paths);
    to->cursor_cached_count += WT_STAT_READ(from, cursor_cached_count);
    to->cursor_insert_bulk += WT_STAT_READ(from, cursor_insert_bulk);
    to->cursor_cache += WT_STAT_READ(from, cursor_cache);
    to->cursor_create += WT_STAT_READ(from, cursor_create);
    to->cursor_insert += WT_STAT_READ(from, cursor_insert);
    to->cursor_insert_bytes += WT_STAT_READ(from, cursor_insert_bytes);
    to->cursor_modify += WT_STAT_READ(from, cursor_modify);
    to->cursor_modify_bytes += WT_STAT_READ(from, cursor_modify_bytes);
    to->cursor_modify_bytes_touch += WT_STAT_READ(from, cursor_modify_bytes_touch);
    to->cursor_next += WT_STAT_READ(from, cursor_next);
    to->cursor_next_hs_tombstone += WT_STAT_READ(from, cursor_next_hs_tombstone);
    to->cursor_next_skip_ge_100 += WT_STAT_READ(from, cursor_next_skip_ge_100);
    to->cursor_next_skip_lt_100 += WT_STAT_READ(from, cursor_next_skip_lt_100);
    to->cursor_restart += WT_STAT_READ(from, cursor_restart);
    to->cursor_prev += WT_STAT_READ(from, cursor_prev);
    to->cursor_prev_hs_tombstone += WT_STAT_READ(from, cursor_prev_hs_tombstone);
    to->cursor_prev_skip_ge_100 += WT_STAT_READ(from, cursor_prev_skip_ge_100);
    to->cursor_prev_skip_lt_100 += WT_STAT_READ(from, cursor_prev_skip_lt_100);
    to->cursor_remove += WT_STAT_READ(from, cursor_remove);
    to->cursor_remove_bytes += WT_STAT_READ(from, cursor_remove_bytes);
    to->cursor_reserve += WT_STAT_READ(from, cursor_reserve);
    to->cursor_reset += WT_STAT_READ(from, cursor_reset);
    to->cursor_search += WT_STAT_READ(from, cursor_search);
    to->cursor_search_hs += WT_STAT_READ(from, cursor_search_hs);
    to->cursor_search_near += WT_STAT_READ(from, cursor_search_near);
    to->cursor_sweep_buckets += WT_STAT_READ(from, cursor_sweep_buckets);
    to->cursor_sweep_closed += WT_STAT_READ(from, cursor_sweep_closed);
    to->cursor_sweep_examined += WT_STAT_READ(from, cursor_sweep_examined);
    to->cursor_sweep += WT_STAT_READ(from, cursor_sweep);
    to->cursor_truncate += WT_STAT_READ(from, cursor_truncate);
    to->cursor_update += WT_STAT_READ(from, cursor_update);
    to->cursor_update_bytes += WT_STAT_READ(from, cursor_update_bytes);
    to->cursor_update_bytes_changed += WT_STAT_READ(from, cursor_update_bytes_changed);
    to->cursor_reopen += WT_STAT_READ(from, cursor_reopen);
    to->cursor_open_count += WT_STAT_READ(from, cursor_open_count);
    to->dh_conn_handle_size += WT_STAT_READ(from, dh_conn_handle_size);
    to->dh_conn_handle_count += WT_STAT_READ(from, dh_conn_handle_count);
    to->dh_sweep_ref += WT_STAT_READ(from, dh_sweep_ref);
    to->dh_sweep_close += WT_STAT_READ(from, dh_sweep_close);
    to->dh_sweep_remove += WT_STAT_READ(from, dh_sweep_remove);
    to->dh_sweep_tod += WT_STAT_READ(from, dh_sweep_tod);
    to->dh_sweeps += WT_STAT_READ(from, dh_sweeps);
    to->dh_sweep_skip_ckpt += WT_STAT_READ(from, dh_sweep_skip_ckpt);
    to->dh_session_handles += WT_STAT_READ(from, dh_session_handles);
    to->dh_session_sweeps += WT_STAT_READ(from, dh_session_sweeps);
    to->lock_checkpoint_count += WT_STAT_READ(from, lock_checkpoint_count);
    to->lock_checkpoint_wait_application += WT_STAT_READ(from, lock_checkpoint_wait_application);
    to->lock_checkpoint_wait_internal += WT_STAT_READ(from, lock_checkpoint_wait_internal);
    to->lock_dhandle_wait_application += WT_STAT_READ(from, lock_dhandle_wait_application);
    to->lock_dhandle_wait_internal += WT_STAT_READ(from, lock_dhandle_wait_internal);
    to->lock_dhandle_read_count += WT_STAT_READ(from, lock_dhandle_read_count);
    to->lock_dhandle_write_count += WT_STAT_READ(from, lock_dhandle_write_count);
    to->lock_durable_timestamp_wait_application +=
      WT_STAT_READ(from, lock_durable_timestamp_wait_application);
    to->lock_durable_timestamp_wait_internal +=
      WT_STAT_READ(from, lock_durable_timestamp_wait_internal);
    to->lock_durable_timestamp_read_count += WT_STAT_READ(from, lock_durable_timestamp_read_count);
    to->lock_durable_timestamp_write_count +=
      WT_STAT_READ(from, lock_durable_timestamp_write_count);
    to->lock_metadata_count += WT_STAT_READ(from, lock_metadata_count);
    to->lock_metadata_wait_application += WT_STAT_READ(from, lock_metadata_wait_application);
    to->lock_metadata_wait_internal += WT_STAT_READ(from, lock_metadata_wait_internal);
    to->lock_read_timestamp_wait_application +=
      WT_STAT_READ(from, lock_read_timestamp_wait_application);
    to->lock_read_timestamp_wait_internal += WT_STAT_READ(from, lock_read_timestamp_wait_internal);
    to->lock_read_timestamp_read_count += WT_STAT_READ(from, lock_read_timestamp_read_count);
    to->lock_read_timestamp_write_count += WT_STAT_READ(from, lock_read_timestamp_write_count);
    to->lock_schema_count += WT_STAT_READ(from, lock_schema_count);
    to->lock_schema_wait_application += WT_STAT_READ(from, lock_schema_wait_application);
    to->lock_schema_wait_internal += WT_STAT_READ(from, lock_schema_wait_internal);
    to->lock_table_wait_application += WT_STAT_READ(from, lock_table_wait_application);
    to->lock_table_wait_internal += WT_STAT_READ(from, lock_table_wait_internal);
    to->lock_table_read_count += WT_STAT_READ(from, lock_table_read_count);
    to->lock_table_write_count += WT_STAT_READ(from, lock_table_write_count);
    to->lock_txn_global_wait_application += WT_STAT_READ(from, lock_txn_global_wait_application);
    to->lock_txn_global_wait_internal += WT_STAT_READ(from, lock_txn_global_wait_internal);
    to->lock_txn_global_read_count += WT_STAT_READ(from, lock_txn_global_read_count);
    to->lock_txn_global_write_count += WT_STAT_READ(from, lock_txn_global_write_count);
    to->log_slot_switch_busy += WT_STAT_READ(from, log_slot_switch_busy);
    to->log_force_archive_sleep += WT_STAT_READ(from, log_force_archive_sleep);
    to->log_bytes_payload += WT_STAT_READ(from, log_bytes_payload);
    to->log_bytes_written += WT_STAT_READ(from, log_bytes_written);
    to->log_zero_fills += WT_STAT_READ(from, log_zero_fills);
    to->log_flush += WT_STAT_READ(from, log_flush);
    to->log_force_write += WT_STAT_READ(from, log_force_write);
    to->log_force_write_skip += WT_STAT_READ(from, log_force_write_skip);
    to->log_compress_writes += WT_STAT_READ(from, log_compress_writes);
    to->log_compress_write_fails += WT_STAT_READ(from, log_compress_write_fails);
    to->log_compress_small += WT_STAT_READ(from, log_compress_small);
    to->log_release_write_lsn += WT_STAT_READ(from, log_release_write_lsn);
    to->log_scans += WT_STAT_READ(from, log_scans);
    to->log_scan_rereads += WT_STAT_READ(from, log_scan_rereads);
    to->log_write_lsn += WT_STAT_READ(from, log_write_lsn);
    to->log_write_lsn_skip += WT_STAT_READ(from, log_write_lsn_skip);
    to->log_sync += WT_STAT_READ(from, log_sync);
    to->log_sync_duration += WT_STAT_READ(from, log_sync_duration);
    to->log_sync_dir += WT_STAT_READ(from, log_sync_dir);
    to->log_sync_dir_duration += WT_STAT_READ(from, log_sync_dir_duration);
    to->log_writes += WT_STAT_READ(from, log_writes);
    to->log_slot_consolidated += WT_STAT_READ(from, log_slot_consolidated);
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
    to->log_slot_active_closed += WT_STAT_READ(from, log_slot_active_closed);
    to->log_slot_yield_duration += WT_STAT_READ(from, log_slot_yield_duration);
    to->log_slot_no_free_slots += WT_STAT_READ(from, log_slot_no_free_slots);
    to->log_slot_unbuffered += WT_STAT_READ(from, log_slot_unbuffered);
    to->log_compress_mem += WT_STAT_READ(from, log_compress_mem);
    to->log_buffer_size += WT_STAT_READ(from, log_buffer_size);
    to->log_compress_len += WT_STAT_READ(from, log_compress_len);
    to->log_slot_coalesced += WT_STAT_READ(from, log_slot_coalesced);
    to->log_close_yields += WT_STAT_READ(from, log_close_yields);
    to->perf_hist_fsread_latency_lt50 += WT_STAT_READ(from, perf_hist_fsread_latency_lt50);
    to->perf_hist_fsread_latency_lt100 += WT_STAT_READ(from, perf_hist_fsread_latency_lt100);
    to->perf_hist_fsread_latency_lt250 += WT_STAT_READ(from, perf_hist_fsread_latency_lt250);
    to->perf_hist_fsread_latency_lt500 += WT_STAT_READ(from, perf_hist_fsread_latency_lt500);
    to->perf_hist_fsread_latency_lt1000 += WT_STAT_READ(from, perf_hist_fsread_latency_lt1000);
    to->perf_hist_fsread_latency_gt1000 += WT_STAT_READ(from, perf_hist_fsread_latency_gt1000);
    to->perf_hist_fswrite_latency_lt50 += WT_STAT_READ(from, perf_hist_fswrite_latency_lt50);
    to->perf_hist_fswrite_latency_lt100 += WT_STAT_READ(from, perf_hist_fswrite_latency_lt100);
    to->perf_hist_fswrite_latency_lt250 += WT_STAT_READ(from, perf_hist_fswrite_latency_lt250);
    to->perf_hist_fswrite_latency_lt500 += WT_STAT_READ(from, perf_hist_fswrite_latency_lt500);
    to->perf_hist_fswrite_latency_lt1000 += WT_STAT_READ(from, perf_hist_fswrite_latency_lt1000);
    to->perf_hist_fswrite_latency_gt1000 += WT_STAT_READ(from, perf_hist_fswrite_latency_gt1000);
    to->perf_hist_opread_latency_lt250 += WT_STAT_READ(from, perf_hist_opread_latency_lt250);
    to->perf_hist_opread_latency_lt500 += WT_STAT_READ(from, perf_hist_opread_latency_lt500);
    to->perf_hist_opread_latency_lt1000 += WT_STAT_READ(from, perf_hist_opread_latency_lt1000);
    to->perf_hist_opread_latency_lt10000 += WT_STAT_READ(from, perf_hist_opread_latency_lt10000);
    to->perf_hist_opread_latency_gt10000 += WT_STAT_READ(from, perf_hist_opread_latency_gt10000);
    to->perf_hist_opwrite_latency_lt250 += WT_STAT_READ(from, perf_hist_opwrite_latency_lt250);
    to->perf_hist_opwrite_latency_lt500 += WT_STAT_READ(from, perf_hist_opwrite_latency_lt500);
    to->perf_hist_opwrite_latency_lt1000 += WT_STAT_READ(from, perf_hist_opwrite_latency_lt1000);
    to->perf_hist_opwrite_latency_lt10000 += WT_STAT_READ(from, perf_hist_opwrite_latency_lt10000);
    to->perf_hist_opwrite_latency_gt10000 += WT_STAT_READ(from, perf_hist_opwrite_latency_gt10000);
    to->rec_time_window_bytes_ts += WT_STAT_READ(from, rec_time_window_bytes_ts);
    to->rec_time_window_bytes_txn += WT_STAT_READ(from, rec_time_window_bytes_txn);
    to->rec_page_delete_fast += WT_STAT_READ(from, rec_page_delete_fast);
    to->rec_overflow_key_leaf += WT_STAT_READ(from, rec_overflow_key_leaf);
    to->rec_maximum_seconds += WT_STAT_READ(from, rec_maximum_seconds);
    to->rec_pages += WT_STAT_READ(from, rec_pages);
    to->rec_pages_eviction += WT_STAT_READ(from, rec_pages_eviction);
    to->rec_pages_with_prepare += WT_STAT_READ(from, rec_pages_with_prepare);
    to->rec_pages_with_ts += WT_STAT_READ(from, rec_pages_with_ts);
    to->rec_pages_with_txn += WT_STAT_READ(from, rec_pages_with_txn);
    to->rec_page_delete += WT_STAT_READ(from, rec_page_delete);
    to->rec_time_aggr_newest_start_durable_ts +=
      WT_STAT_READ(from, rec_time_aggr_newest_start_durable_ts);
    to->rec_time_aggr_newest_stop_durable_ts +=
      WT_STAT_READ(from, rec_time_aggr_newest_stop_durable_ts);
    to->rec_time_aggr_newest_stop_ts += WT_STAT_READ(from, rec_time_aggr_newest_stop_ts);
    to->rec_time_aggr_newest_stop_txn += WT_STAT_READ(from, rec_time_aggr_newest_stop_txn);
    to->rec_time_aggr_newest_txn += WT_STAT_READ(from, rec_time_aggr_newest_txn);
    to->rec_time_aggr_oldest_start_ts += WT_STAT_READ(from, rec_time_aggr_oldest_start_ts);
    to->rec_time_aggr_prepared += WT_STAT_READ(from, rec_time_aggr_prepared);
    to->rec_time_window_pages_prepared += WT_STAT_READ(from, rec_time_window_pages_prepared);
    to->rec_time_window_pages_durable_start_ts +=
      WT_STAT_READ(from, rec_time_window_pages_durable_start_ts);
    to->rec_time_window_pages_start_ts += WT_STAT_READ(from, rec_time_window_pages_start_ts);
    to->rec_time_window_pages_start_txn += WT_STAT_READ(from, rec_time_window_pages_start_txn);
    to->rec_time_window_pages_durable_stop_ts +=
      WT_STAT_READ(from, rec_time_window_pages_durable_stop_ts);
    to->rec_time_window_pages_stop_ts += WT_STAT_READ(from, rec_time_window_pages_stop_ts);
    to->rec_time_window_pages_stop_txn += WT_STAT_READ(from, rec_time_window_pages_stop_txn);
    to->rec_time_window_prepared += WT_STAT_READ(from, rec_time_window_prepared);
    to->rec_time_window_durable_start_ts += WT_STAT_READ(from, rec_time_window_durable_start_ts);
    to->rec_time_window_start_ts += WT_STAT_READ(from, rec_time_window_start_ts);
    to->rec_time_window_start_txn += WT_STAT_READ(from, rec_time_window_start_txn);
    to->rec_time_window_durable_stop_ts += WT_STAT_READ(from, rec_time_window_durable_stop_ts);
    to->rec_time_window_stop_ts += WT_STAT_READ(from, rec_time_window_stop_ts);
    to->rec_time_window_stop_txn += WT_STAT_READ(from, rec_time_window_stop_txn);
    to->rec_split_stashed_bytes += WT_STAT_READ(from, rec_split_stashed_bytes);
    to->rec_split_stashed_objects += WT_STAT_READ(from, rec_split_stashed_objects);
    to->local_objects_inuse += WT_STAT_READ(from, local_objects_inuse);
    to->flush_tier += WT_STAT_READ(from, flush_tier);
    to->local_objects_removed += WT_STAT_READ(from, local_objects_removed);
    to->session_open += WT_STAT_READ(from, session_open);
    to->session_query_ts += WT_STAT_READ(from, session_query_ts);
    to->session_table_alter_fail += WT_STAT_READ(from, session_table_alter_fail);
    to->session_table_alter_success += WT_STAT_READ(from, session_table_alter_success);
    to->session_table_alter_trigger_checkpoint +=
      WT_STAT_READ(from, session_table_alter_trigger_checkpoint);
    to->session_table_alter_skip += WT_STAT_READ(from, session_table_alter_skip);
    to->session_table_compact_fail += WT_STAT_READ(from, session_table_compact_fail);
    to->session_table_compact_fail_cache_pressure +=
      WT_STAT_READ(from, session_table_compact_fail_cache_pressure);
    to->session_table_compact_running += WT_STAT_READ(from, session_table_compact_running);
    to->session_table_compact_skipped += WT_STAT_READ(from, session_table_compact_skipped);
    to->session_table_compact_success += WT_STAT_READ(from, session_table_compact_success);
    to->session_table_compact_timeout += WT_STAT_READ(from, session_table_compact_timeout);
    to->session_table_create_fail += WT_STAT_READ(from, session_table_create_fail);
    to->session_table_create_success += WT_STAT_READ(from, session_table_create_success);
    to->session_table_drop_fail += WT_STAT_READ(from, session_table_drop_fail);
    to->session_table_drop_success += WT_STAT_READ(from, session_table_drop_success);
    to->session_table_rename_fail += WT_STAT_READ(from, session_table_rename_fail);
    to->session_table_rename_success += WT_STAT_READ(from, session_table_rename_success);
    to->session_table_salvage_fail += WT_STAT_READ(from, session_table_salvage_fail);
    to->session_table_salvage_success += WT_STAT_READ(from, session_table_salvage_success);
    to->session_table_truncate_fail += WT_STAT_READ(from, session_table_truncate_fail);
    to->session_table_truncate_success += WT_STAT_READ(from, session_table_truncate_success);
    to->session_table_verify_fail += WT_STAT_READ(from, session_table_verify_fail);
    to->session_table_verify_success += WT_STAT_READ(from, session_table_verify_success);
    to->tiered_work_units_dequeued += WT_STAT_READ(from, tiered_work_units_dequeued);
    to->tiered_work_units_created += WT_STAT_READ(from, tiered_work_units_created);
    to->tiered_retention += WT_STAT_READ(from, tiered_retention);
    to->tiered_object_size += WT_STAT_READ(from, tiered_object_size);
    to->thread_fsync_active += WT_STAT_READ(from, thread_fsync_active);
    to->thread_read_active += WT_STAT_READ(from, thread_read_active);
    to->thread_write_active += WT_STAT_READ(from, thread_write_active);
    to->application_evict_time += WT_STAT_READ(from, application_evict_time);
    to->application_cache_time += WT_STAT_READ(from, application_cache_time);
    to->txn_release_blocked += WT_STAT_READ(from, txn_release_blocked);
    to->conn_close_blocked_lsm += WT_STAT_READ(from, conn_close_blocked_lsm);
    to->dhandle_lock_blocked += WT_STAT_READ(from, dhandle_lock_blocked);
    to->page_index_slot_ref_blocked += WT_STAT_READ(from, page_index_slot_ref_blocked);
    to->prepared_transition_blocked_page += WT_STAT_READ(from, prepared_transition_blocked_page);
    to->page_busy_blocked += WT_STAT_READ(from, page_busy_blocked);
    to->page_forcible_evict_blocked += WT_STAT_READ(from, page_forcible_evict_blocked);
    to->page_locked_blocked += WT_STAT_READ(from, page_locked_blocked);
    to->page_read_blocked += WT_STAT_READ(from, page_read_blocked);
    to->page_sleep += WT_STAT_READ(from, page_sleep);
    to->page_del_rollback_blocked += WT_STAT_READ(from, page_del_rollback_blocked);
    to->child_modify_blocked_page += WT_STAT_READ(from, child_modify_blocked_page);
    to->txn_prepared_updates += WT_STAT_READ(from, txn_prepared_updates);
    to->txn_prepared_updates_committed += WT_STAT_READ(from, txn_prepared_updates_committed);
    to->txn_prepared_updates_key_repeated += WT_STAT_READ(from, txn_prepared_updates_key_repeated);
    to->txn_prepared_updates_rolledback += WT_STAT_READ(from, txn_prepared_updates_rolledback);
    to->txn_prepare += WT_STAT_READ(from, txn_prepare);
    to->txn_prepare_commit += WT_STAT_READ(from, txn_prepare_commit);
    to->txn_prepare_active += WT_STAT_READ(from, txn_prepare_active);
    to->txn_prepare_rollback += WT_STAT_READ(from, txn_prepare_rollback);
    to->txn_prepare_rollback_do_not_remove_hs_update +=
      WT_STAT_READ(from, txn_prepare_rollback_do_not_remove_hs_update);
    to->txn_prepare_rollback_fix_hs_update_with_ckpt_reserved_txnid +=
      WT_STAT_READ(from, txn_prepare_rollback_fix_hs_update_with_ckpt_reserved_txnid);
    to->txn_query_ts += WT_STAT_READ(from, txn_query_ts);
    to->txn_read_race_prepare_update += WT_STAT_READ(from, txn_read_race_prepare_update);
    to->txn_rts += WT_STAT_READ(from, txn_rts);
    to->txn_rts_hs_stop_older_than_newer_start +=
      WT_STAT_READ(from, txn_rts_hs_stop_older_than_newer_start);
    to->txn_rts_inconsistent_ckpt += WT_STAT_READ(from, txn_rts_inconsistent_ckpt);
    to->txn_rts_keys_removed += WT_STAT_READ(from, txn_rts_keys_removed);
    to->txn_rts_keys_restored += WT_STAT_READ(from, txn_rts_keys_restored);
    to->txn_rts_pages_visited += WT_STAT_READ(from, txn_rts_pages_visited);
    to->txn_rts_hs_restore_tombstones += WT_STAT_READ(from, txn_rts_hs_restore_tombstones);
    to->txn_rts_hs_restore_updates += WT_STAT_READ(from, txn_rts_hs_restore_updates);
    to->txn_rts_delete_rle_skipped += WT_STAT_READ(from, txn_rts_delete_rle_skipped);
    to->txn_rts_stable_rle_skipped += WT_STAT_READ(from, txn_rts_stable_rle_skipped);
    to->txn_rts_sweep_hs_keys += WT_STAT_READ(from, txn_rts_sweep_hs_keys);
    to->txn_rts_tree_walk_skip_pages += WT_STAT_READ(from, txn_rts_tree_walk_skip_pages);
    to->txn_rts_upd_aborted += WT_STAT_READ(from, txn_rts_upd_aborted);
    to->txn_rts_hs_removed += WT_STAT_READ(from, txn_rts_hs_removed);
    to->txn_sessions_walked += WT_STAT_READ(from, txn_sessions_walked);
    to->txn_set_ts += WT_STAT_READ(from, txn_set_ts);
    to->txn_set_ts_durable += WT_STAT_READ(from, txn_set_ts_durable);
    to->txn_set_ts_durable_upd += WT_STAT_READ(from, txn_set_ts_durable_upd);
    to->txn_set_ts_oldest += WT_STAT_READ(from, txn_set_ts_oldest);
    to->txn_set_ts_oldest_upd += WT_STAT_READ(from, txn_set_ts_oldest_upd);
    to->txn_set_ts_stable += WT_STAT_READ(from, txn_set_ts_stable);
    to->txn_set_ts_stable_upd += WT_STAT_READ(from, txn_set_ts_stable_upd);
    to->txn_begin += WT_STAT_READ(from, txn_begin);
    to->txn_checkpoint_running += WT_STAT_READ(from, txn_checkpoint_running);
    to->txn_checkpoint_running_hs += WT_STAT_READ(from, txn_checkpoint_running_hs);
    to->txn_checkpoint_generation += WT_STAT_READ(from, txn_checkpoint_generation);
    to->txn_hs_ckpt_duration += WT_STAT_READ(from, txn_hs_ckpt_duration);
    to->txn_checkpoint_time_max += WT_STAT_READ(from, txn_checkpoint_time_max);
    to->txn_checkpoint_time_min += WT_STAT_READ(from, txn_checkpoint_time_min);
    to->txn_checkpoint_handle_duration += WT_STAT_READ(from, txn_checkpoint_handle_duration);
    to->txn_checkpoint_handle_duration_apply +=
      WT_STAT_READ(from, txn_checkpoint_handle_duration_apply);
    to->txn_checkpoint_handle_duration_skip +=
      WT_STAT_READ(from, txn_checkpoint_handle_duration_skip);
    to->txn_checkpoint_handle_applied += WT_STAT_READ(from, txn_checkpoint_handle_applied);
    to->txn_checkpoint_handle_skipped += WT_STAT_READ(from, txn_checkpoint_handle_skipped);
    to->txn_checkpoint_handle_walked += WT_STAT_READ(from, txn_checkpoint_handle_walked);
    to->txn_checkpoint_time_recent += WT_STAT_READ(from, txn_checkpoint_time_recent);
    to->txn_checkpoint_prep_running += WT_STAT_READ(from, txn_checkpoint_prep_running);
    to->txn_checkpoint_prep_max += WT_STAT_READ(from, txn_checkpoint_prep_max);
    to->txn_checkpoint_prep_min += WT_STAT_READ(from, txn_checkpoint_prep_min);
    to->txn_checkpoint_prep_recent += WT_STAT_READ(from, txn_checkpoint_prep_recent);
    to->txn_checkpoint_prep_total += WT_STAT_READ(from, txn_checkpoint_prep_total);
    to->txn_checkpoint_scrub_target += WT_STAT_READ(from, txn_checkpoint_scrub_target);
    to->txn_checkpoint_scrub_time += WT_STAT_READ(from, txn_checkpoint_scrub_time);
    to->txn_checkpoint_time_total += WT_STAT_READ(from, txn_checkpoint_time_total);
    to->txn_checkpoint += WT_STAT_READ(from, txn_checkpoint);
    to->txn_checkpoint_obsolete_applied += WT_STAT_READ(from, txn_checkpoint_obsolete_applied);
    to->txn_checkpoint_skipped += WT_STAT_READ(from, txn_checkpoint_skipped);
    to->txn_fail_cache += WT_STAT_READ(from, txn_fail_cache);
    to->txn_checkpoint_fsync_post += WT_STAT_READ(from, txn_checkpoint_fsync_post);
    to->txn_checkpoint_fsync_post_duration +=
      WT_STAT_READ(from, txn_checkpoint_fsync_post_duration);
    to->txn_pinned_range += WT_STAT_READ(from, txn_pinned_range);
    to->txn_pinned_checkpoint_range += WT_STAT_READ(from, txn_pinned_checkpoint_range);
    to->txn_pinned_timestamp += WT_STAT_READ(from, txn_pinned_timestamp);
    to->txn_pinned_timestamp_checkpoint += WT_STAT_READ(from, txn_pinned_timestamp_checkpoint);
    to->txn_pinned_timestamp_reader += WT_STAT_READ(from, txn_pinned_timestamp_reader);
    to->txn_pinned_timestamp_oldest += WT_STAT_READ(from, txn_pinned_timestamp_oldest);
    to->txn_timestamp_oldest_active_read += WT_STAT_READ(from, txn_timestamp_oldest_active_read);
    to->txn_rollback_to_stable_running += WT_STAT_READ(from, txn_rollback_to_stable_running);
    to->txn_walk_sessions += WT_STAT_READ(from, txn_walk_sessions);
    to->txn_commit += WT_STAT_READ(from, txn_commit);
    to->txn_rollback += WT_STAT_READ(from, txn_rollback);
    to->txn_update_conflict += WT_STAT_READ(from, txn_update_conflict);
}

static const char *const __stats_join_desc[] = {
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
__wt_stat_join_aggregate(WT_JOIN_STATS **from, WT_JOIN_STATS *to)
{
    to->main_access += WT_STAT_READ(from, main_access);
    to->bloom_false_positive += WT_STAT_READ(from, bloom_false_positive);
    to->membership_check += WT_STAT_READ(from, membership_check);
    to->bloom_insert += WT_STAT_READ(from, bloom_insert);
    to->iterated += WT_STAT_READ(from, iterated);
}

static const char *const __stats_session_desc[] = {
  "session: bytes read into cache",
  "session: bytes written from cache",
  "session: dhandle lock wait time (usecs)",
  "session: page read from disk to cache time (usecs)",
  "session: page write from cache to disk time (usecs)",
  "session: schema lock wait time (usecs)",
  "session: time waiting for cache (usecs)",
};

int
__wt_stat_session_desc(WT_CURSOR_STAT *cst, int slot, const char **p)
{
    WT_UNUSED(cst);
    *p = __stats_session_desc[slot];
    return (0);
}

void
__wt_stat_session_init_single(WT_SESSION_STATS *stats)
{
    memset(stats, 0, sizeof(*stats));
}

void
__wt_stat_session_clear_single(WT_SESSION_STATS *stats)
{
    stats->bytes_read = 0;
    stats->bytes_write = 0;
    stats->lock_dhandle_wait = 0;
    stats->read_time = 0;
    stats->write_time = 0;
    stats->lock_schema_wait = 0;
    stats->cache_time = 0;
}
