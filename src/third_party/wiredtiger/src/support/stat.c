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
  "autocommit: retries for readonly operations",
  "autocommit: retries for update operations",
  "backup: total modified incremental blocks with compressed data",
  "backup: total modified incremental blocks without compressed data",
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
  "btree: btree expected number of compact bytes rewritten",
  "btree: btree expected number of compact pages rewritten",
  "btree: btree number of pages reconciled during checkpoint",
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
  "cache: application threads eviction requested with cache fill ratio < 25%",
  "cache: application threads eviction requested with cache fill ratio >= 25% and < 50%",
  "cache: application threads eviction requested with cache fill ratio >= 50% and < 75%",
  "cache: application threads eviction requested with cache fill ratio >= 75%",
  "cache: bytes currently in the cache",
  "cache: bytes dirty in the cache cumulative",
  "cache: bytes read into cache",
  "cache: bytes written from cache",
  "cache: checkpoint blocked page eviction",
  "cache: checkpoint of history store file blocked non-history store page eviction",
  "cache: data source pages selected for eviction unable to be evicted",
  "cache: eviction gave up due to detecting a disk value without a timestamp behind the last "
  "update on the chain",
  "cache: eviction gave up due to detecting a tombstone without a timestamp ahead of the selected "
  "on disk update",
  "cache: eviction gave up due to detecting a tombstone without a timestamp ahead of the selected "
  "on disk update after validating the update chain",
  "cache: eviction gave up due to detecting update chain entries without timestamps after the "
  "selected on disk update",
  "cache: eviction gave up due to needing to remove a record from the history store but checkpoint "
  "is running",
  "cache: eviction gave up due to no progress being made",
  "cache: eviction walk pages queued that had updates",
  "cache: eviction walk pages queued that were clean",
  "cache: eviction walk pages queued that were dirty",
  "cache: eviction walk pages seen that had updates",
  "cache: eviction walk pages seen that were clean",
  "cache: eviction walk pages seen that were dirty",
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
  "cache: eviction walks random search fails to locate a page, results in a null position",
  "cache: eviction walks reached end of tree",
  "cache: eviction walks restarted",
  "cache: eviction walks started from root of tree",
  "cache: eviction walks started from saved location in tree",
  "cache: hazard pointer blocked page eviction",
  "cache: history store table insert calls",
  "cache: history store table insert calls that returned restart",
  "cache: history store table reads",
  "cache: history store table reads missed",
  "cache: history store table reads requiring squashed modifies",
  "cache: history store table resolved updates without timestamps that lose their durable "
  "timestamp",
  "cache: history store table truncation by rollback to stable to remove an unstable update",
  "cache: history store table truncation by rollback to stable to remove an update",
  "cache: history store table truncation to remove all the keys of a btree",
  "cache: history store table truncation to remove an update",
  "cache: history store table truncation to remove range of updates due to an update without a "
  "timestamp on data page",
  "cache: history store table truncation to remove range of updates due to key being removed from "
  "the data page during reconciliation",
  "cache: history store table truncations that would have happened in non-dryrun mode",
  "cache: history store table truncations to remove an unstable update that would have happened in "
  "non-dryrun mode",
  "cache: history store table truncations to remove an update that would have happened in "
  "non-dryrun mode",
  "cache: history store table updates without timestamps fixed up by reinserting with the fixed "
  "timestamp",
  "cache: history store table writes requiring squashed modifies",
  "cache: in-memory page passed criteria to be split",
  "cache: in-memory page splits",
  "cache: internal page split blocked its eviction",
  "cache: internal pages evicted",
  "cache: internal pages split during eviction",
  "cache: leaf pages split during eviction",
  "cache: locate a random in-mem ref by examining all entries on the root page",
  "cache: modified pages evicted",
  "cache: multi-block reconciliation blocked whilst checkpoint is running",
  "cache: number of times dirty trigger was reached",
  "cache: number of times eviction trigger was reached",
  "cache: number of times updates trigger was reached",
  "cache: overflow keys on a multiblock row-store page blocked its eviction",
  "cache: overflow pages read into cache",
  "cache: page split during eviction deepened the tree",
  "cache: page written requiring history store records",
  "cache: pages dirtied due to obsolete time window by eviction",
  "cache: pages read into cache",
  "cache: pages read into cache after truncate",
  "cache: pages read into cache after truncate in prepare state",
  "cache: pages read into cache by checkpoint",
  "cache: pages requested from the cache",
  "cache: pages requested from the cache due to pre-fetch",
  "cache: pages seen by eviction walk",
  "cache: pages written from cache",
  "cache: pages written requiring in-memory restoration",
  "cache: recent modification of a page blocked its eviction",
  "cache: reverse splits performed",
  "cache: reverse splits skipped because of VLCS namespace gap restrictions",
  "cache: the number of times full update inserted to history store",
  "cache: the number of times reverse modify inserted to history store",
  "cache: tracked dirty bytes in the cache",
  "cache: tracked dirty internal page bytes in the cache",
  "cache: tracked dirty leaf page bytes in the cache",
  "cache: uncommitted truncate blocked page eviction",
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
  "checkpoint: checkpoint has acquired a snapshot for its transaction",
  "checkpoint: pages added for eviction during checkpoint cleanup",
  "checkpoint: pages dirtied due to obsolete time window by checkpoint cleanup",
  "checkpoint: pages read into cache during checkpoint cleanup (reclaim_space)",
  "checkpoint: pages read into cache during checkpoint cleanup due to obsolete time window",
  "checkpoint: pages removed during checkpoint cleanup",
  "checkpoint: pages skipped during checkpoint cleanup tree walk",
  "checkpoint: pages visited during checkpoint cleanup",
  "checkpoint: transaction checkpoints due to obsolete pages",
  "compression: compressed page maximum internal page size prior to compression",
  "compression: compressed page maximum leaf page size prior to compression ",
  "compression: page written to disk failed to compress",
  "compression: page written to disk was too small to compress",
  "compression: pages read from disk",
  "compression: pages read from disk with compression ratio greater than 64",
  "compression: pages read from disk with compression ratio smaller than  2",
  "compression: pages read from disk with compression ratio smaller than  4",
  "compression: pages read from disk with compression ratio smaller than  8",
  "compression: pages read from disk with compression ratio smaller than 16",
  "compression: pages read from disk with compression ratio smaller than 32",
  "compression: pages read from disk with compression ratio smaller than 64",
  "compression: pages written to disk",
  "compression: pages written to disk with compression ratio greater than 64",
  "compression: pages written to disk with compression ratio smaller than  2",
  "compression: pages written to disk with compression ratio smaller than  4",
  "compression: pages written to disk with compression ratio smaller than  8",
  "compression: pages written to disk with compression ratio smaller than 16",
  "compression: pages written to disk with compression ratio smaller than 32",
  "compression: pages written to disk with compression ratio smaller than 64",
  "cursor: Total number of deleted pages skipped during tree walk",
  "cursor: Total number of entries skipped by cursor next calls",
  "cursor: Total number of entries skipped by cursor prev calls",
  "cursor: Total number of entries skipped to position the history store cursor",
  "cursor: Total number of in-memory deleted pages skipped during tree walk",
  "cursor: Total number of on-disk deleted pages skipped during tree walk",
  "cursor: Total number of times a search near has exited due to prefix config",
  "cursor: Total number of times cursor fails to temporarily release pinned page to encourage "
  "eviction of hot or large page",
  "cursor: Total number of times cursor temporarily releases pinned page to encourage eviction of "
  "hot or large page",
  "cursor: bulk loaded cursor insert calls",
  "cursor: cache cursors reuse count",
  "cursor: close calls that result in cache",
  "cursor: create calls",
  "cursor: cursor bound calls that return an error",
  "cursor: cursor bounds cleared from reset",
  "cursor: cursor bounds comparisons performed",
  "cursor: cursor bounds next called on an unpositioned cursor",
  "cursor: cursor bounds next early exit",
  "cursor: cursor bounds prev called on an unpositioned cursor",
  "cursor: cursor bounds prev early exit",
  "cursor: cursor bounds search early exit",
  "cursor: cursor bounds search near call repositioned cursor",
  "cursor: cursor cache calls that return an error",
  "cursor: cursor close calls that return an error",
  "cursor: cursor compare calls that return an error",
  "cursor: cursor equals calls that return an error",
  "cursor: cursor get key calls that return an error",
  "cursor: cursor get value calls that return an error",
  "cursor: cursor insert calls that return an error",
  "cursor: cursor insert check calls that return an error",
  "cursor: cursor largest key calls that return an error",
  "cursor: cursor modify calls that return an error",
  "cursor: cursor next calls that return an error",
  "cursor: cursor next calls that skip due to a globally visible history store tombstone",
  "cursor: cursor next calls that skip greater than 1 and fewer than 100 entries",
  "cursor: cursor next calls that skip greater than or equal to 100 entries",
  "cursor: cursor next random calls that return an error",
  "cursor: cursor prev calls that return an error",
  "cursor: cursor prev calls that skip due to a globally visible history store tombstone",
  "cursor: cursor prev calls that skip greater than or equal to 100 entries",
  "cursor: cursor prev calls that skip less than 100 entries",
  "cursor: cursor reconfigure calls that return an error",
  "cursor: cursor remove calls that return an error",
  "cursor: cursor reopen calls that return an error",
  "cursor: cursor reserve calls that return an error",
  "cursor: cursor reset calls that return an error",
  "cursor: cursor search calls that return an error",
  "cursor: cursor search near calls that return an error",
  "cursor: cursor update calls that return an error",
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
  "reconciliation: VLCS pages explicitly reconciled as empty",
  "reconciliation: approximate byte size of timestamps in pages written",
  "reconciliation: approximate byte size of transaction IDs in pages written",
  "reconciliation: cursor next/prev calls during HS wrapup search_near",
  "reconciliation: dictionary matches",
  "reconciliation: fast-path pages deleted",
  "reconciliation: internal page key bytes discarded using suffix compression",
  "reconciliation: internal page multi-block writes",
  "reconciliation: leaf page key bytes discarded using prefix compression",
  "reconciliation: leaf page multi-block writes",
  "reconciliation: leaf-page overflow keys",
  "reconciliation: maximum blocks required for a page",
  "reconciliation: overflow values written",
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
  "transaction: a reader raced with a prepared transaction commit and skipped an update or updates",
  "transaction: number of times overflow removed value is read",
  "transaction: race to read prepared update retry",
  "transaction: rollback to stable history store keys that would have been swept in non-dryrun "
  "mode",
  "transaction: rollback to stable history store records with stop timestamps older than newer "
  "records",
  "transaction: rollback to stable inconsistent checkpoint",
  "transaction: rollback to stable keys removed",
  "transaction: rollback to stable keys restored",
  "transaction: rollback to stable keys that would have been removed in non-dryrun mode",
  "transaction: rollback to stable keys that would have been restored in non-dryrun mode",
  "transaction: rollback to stable restored tombstones from history store",
  "transaction: rollback to stable restored updates from history store",
  "transaction: rollback to stable skipping delete rle",
  "transaction: rollback to stable skipping stable rle",
  "transaction: rollback to stable sweeping history store keys",
  "transaction: rollback to stable tombstones from history store that would have been restored in "
  "non-dryrun mode",
  "transaction: rollback to stable updates from history store that would have been restored in "
  "non-dryrun mode",
  "transaction: rollback to stable updates removed from history store",
  "transaction: rollback to stable updates that would have been removed from history store in "
  "non-dryrun mode",
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
    stats->autocommit_readonly_retry = 0;
    stats->autocommit_update_retry = 0;
    stats->backup_blocks_compressed = 0;
    stats->backup_blocks_uncompressed = 0;
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
    /* not clearing btree_compact_bytes_rewritten_expected */
    /* not clearing btree_compact_pages_rewritten_expected */
    /* not clearing btree_checkpoint_pages_reconciled */
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
    stats->cache_eviction_app_threads_fill_ratio_lt_25 = 0;
    stats->cache_eviction_app_threads_fill_ratio_25_50 = 0;
    stats->cache_eviction_app_threads_fill_ratio_50_75 = 0;
    stats->cache_eviction_app_threads_fill_ratio_gt_75 = 0;
    /* not clearing cache_bytes_inuse */
    /* not clearing cache_bytes_dirty_total */
    stats->cache_bytes_read = 0;
    stats->cache_bytes_write = 0;
    stats->cache_eviction_blocked_checkpoint = 0;
    stats->cache_eviction_blocked_checkpoint_hs = 0;
    stats->cache_eviction_fail = 0;
    stats->cache_eviction_blocked_no_ts_checkpoint_race_1 = 0;
    stats->cache_eviction_blocked_no_ts_checkpoint_race_2 = 0;
    stats->cache_eviction_blocked_no_ts_checkpoint_race_3 = 0;
    stats->cache_eviction_blocked_no_ts_checkpoint_race_4 = 0;
    stats->cache_eviction_blocked_remove_hs_race_with_checkpoint = 0;
    stats->cache_eviction_blocked_no_progress = 0;
    stats->cache_eviction_pages_queued_updates = 0;
    stats->cache_eviction_pages_queued_clean = 0;
    stats->cache_eviction_pages_queued_dirty = 0;
    stats->cache_eviction_pages_seen_updates = 0;
    stats->cache_eviction_pages_seen_clean = 0;
    stats->cache_eviction_pages_seen_dirty = 0;
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
    stats->cache_eviction_walk_random_returns_null_position = 0;
    stats->cache_eviction_walks_ended = 0;
    stats->cache_eviction_walk_restart = 0;
    stats->cache_eviction_walk_from_root = 0;
    stats->cache_eviction_walk_saved_pos = 0;
    stats->cache_eviction_blocked_hazard = 0;
    stats->cache_hs_insert = 0;
    stats->cache_hs_insert_restart = 0;
    stats->cache_hs_read = 0;
    stats->cache_hs_read_miss = 0;
    stats->cache_hs_read_squash = 0;
    stats->cache_hs_order_lose_durable_timestamp = 0;
    stats->cache_hs_key_truncate_rts_unstable = 0;
    stats->cache_hs_key_truncate_rts = 0;
    stats->cache_hs_btree_truncate = 0;
    stats->cache_hs_key_truncate = 0;
    stats->cache_hs_order_remove = 0;
    stats->cache_hs_key_truncate_onpage_removal = 0;
    stats->cache_hs_btree_truncate_dryrun = 0;
    stats->cache_hs_key_truncate_rts_unstable_dryrun = 0;
    stats->cache_hs_key_truncate_rts_dryrun = 0;
    stats->cache_hs_order_reinsert = 0;
    stats->cache_hs_write_squash = 0;
    stats->cache_inmem_splittable = 0;
    stats->cache_inmem_split = 0;
    stats->cache_eviction_blocked_internal_page_split = 0;
    stats->cache_eviction_internal = 0;
    stats->cache_eviction_split_internal = 0;
    stats->cache_eviction_split_leaf = 0;
    stats->cache_eviction_random_sample_inmem_root = 0;
    stats->cache_eviction_dirty = 0;
    stats->cache_eviction_blocked_multi_block_reconcilation_during_checkpoint = 0;
    stats->cache_eviction_trigger_dirty_reached = 0;
    stats->cache_eviction_trigger_reached = 0;
    stats->cache_eviction_trigger_updates_reached = 0;
    stats->cache_eviction_blocked_overflow_keys = 0;
    stats->cache_read_overflow = 0;
    stats->cache_eviction_deepen = 0;
    stats->cache_write_hs = 0;
    stats->cache_eviction_dirty_obsolete_tw = 0;
    stats->cache_read = 0;
    stats->cache_read_deleted = 0;
    stats->cache_read_deleted_prepared = 0;
    stats->cache_read_checkpoint = 0;
    stats->cache_pages_requested = 0;
    stats->cache_pages_prefetch = 0;
    stats->cache_eviction_pages_seen = 0;
    stats->cache_write = 0;
    stats->cache_write_restore = 0;
    stats->cache_eviction_blocked_recently_modified = 0;
    stats->cache_reverse_splits = 0;
    stats->cache_reverse_splits_skipped_vlcs = 0;
    stats->cache_hs_insert_full_update = 0;
    stats->cache_hs_insert_reverse_modify = 0;
    /* not clearing cache_bytes_dirty */
    /* not clearing cache_bytes_dirty_internal */
    /* not clearing cache_bytes_dirty_leaf */
    stats->cache_eviction_blocked_uncommitted_truncate = 0;
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
    stats->checkpoint_snapshot_acquired = 0;
    stats->checkpoint_cleanup_pages_evict = 0;
    stats->checkpoint_cleanup_pages_obsolete_tw = 0;
    stats->checkpoint_cleanup_pages_read_reclaim_space = 0;
    stats->checkpoint_cleanup_pages_read_obsolete_tw = 0;
    stats->checkpoint_cleanup_pages_removed = 0;
    stats->checkpoint_cleanup_pages_walk_skipped = 0;
    stats->checkpoint_cleanup_pages_visited = 0;
    stats->checkpoint_obsolete_applied = 0;
    /* not clearing compress_precomp_intl_max_page_size */
    /* not clearing compress_precomp_leaf_max_page_size */
    stats->compress_write_fail = 0;
    stats->compress_write_too_small = 0;
    stats->compress_read = 0;
    stats->compress_read_ratio_hist_max = 0;
    stats->compress_read_ratio_hist_2 = 0;
    stats->compress_read_ratio_hist_4 = 0;
    stats->compress_read_ratio_hist_8 = 0;
    stats->compress_read_ratio_hist_16 = 0;
    stats->compress_read_ratio_hist_32 = 0;
    stats->compress_read_ratio_hist_64 = 0;
    stats->compress_write = 0;
    stats->compress_write_ratio_hist_max = 0;
    stats->compress_write_ratio_hist_2 = 0;
    stats->compress_write_ratio_hist_4 = 0;
    stats->compress_write_ratio_hist_8 = 0;
    stats->compress_write_ratio_hist_16 = 0;
    stats->compress_write_ratio_hist_32 = 0;
    stats->compress_write_ratio_hist_64 = 0;
    stats->cursor_tree_walk_del_page_skip = 0;
    stats->cursor_next_skip_total = 0;
    stats->cursor_prev_skip_total = 0;
    stats->cursor_skip_hs_cur_position = 0;
    stats->cursor_tree_walk_inmem_del_page_skip = 0;
    stats->cursor_tree_walk_ondisk_del_page_skip = 0;
    stats->cursor_search_near_prefix_fast_paths = 0;
    stats->cursor_reposition_failed = 0;
    stats->cursor_reposition = 0;
    stats->cursor_insert_bulk = 0;
    stats->cursor_reopen = 0;
    stats->cursor_cache = 0;
    stats->cursor_create = 0;
    stats->cursor_bound_error = 0;
    stats->cursor_bounds_reset = 0;
    stats->cursor_bounds_comparisons = 0;
    stats->cursor_bounds_next_unpositioned = 0;
    stats->cursor_bounds_next_early_exit = 0;
    stats->cursor_bounds_prev_unpositioned = 0;
    stats->cursor_bounds_prev_early_exit = 0;
    stats->cursor_bounds_search_early_exit = 0;
    stats->cursor_bounds_search_near_repositioned_cursor = 0;
    stats->cursor_cache_error = 0;
    stats->cursor_close_error = 0;
    stats->cursor_compare_error = 0;
    stats->cursor_equals_error = 0;
    stats->cursor_get_key_error = 0;
    stats->cursor_get_value_error = 0;
    stats->cursor_insert_error = 0;
    stats->cursor_insert_check_error = 0;
    stats->cursor_largest_key_error = 0;
    stats->cursor_modify_error = 0;
    stats->cursor_next_error = 0;
    stats->cursor_next_hs_tombstone = 0;
    stats->cursor_next_skip_lt_100 = 0;
    stats->cursor_next_skip_ge_100 = 0;
    stats->cursor_next_random_error = 0;
    stats->cursor_prev_error = 0;
    stats->cursor_prev_hs_tombstone = 0;
    stats->cursor_prev_skip_ge_100 = 0;
    stats->cursor_prev_skip_lt_100 = 0;
    stats->cursor_reconfigure_error = 0;
    stats->cursor_remove_error = 0;
    stats->cursor_reopen_error = 0;
    stats->cursor_reserve_error = 0;
    stats->cursor_reset_error = 0;
    stats->cursor_search_error = 0;
    stats->cursor_search_near_error = 0;
    stats->cursor_update_error = 0;
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
    stats->rec_vlcs_emptied_pages = 0;
    stats->rec_time_window_bytes_ts = 0;
    stats->rec_time_window_bytes_txn = 0;
    stats->rec_hs_wrapup_next_prev_calls = 0;
    stats->rec_dictionary = 0;
    stats->rec_page_delete_fast = 0;
    stats->rec_suffix_compression = 0;
    stats->rec_multiblock_internal = 0;
    stats->rec_prefix_compression = 0;
    stats->rec_multiblock_leaf = 0;
    stats->rec_overflow_key_leaf = 0;
    stats->rec_multiblock_max = 0;
    stats->rec_overflow_value = 0;
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
    stats->txn_read_race_prepare_commit = 0;
    stats->txn_read_overflow_remove = 0;
    stats->txn_read_race_prepare_update = 0;
    stats->txn_rts_sweep_hs_keys_dryrun = 0;
    stats->txn_rts_hs_stop_older_than_newer_start = 0;
    stats->txn_rts_inconsistent_ckpt = 0;
    stats->txn_rts_keys_removed = 0;
    stats->txn_rts_keys_restored = 0;
    stats->txn_rts_keys_removed_dryrun = 0;
    stats->txn_rts_keys_restored_dryrun = 0;
    stats->txn_rts_hs_restore_tombstones = 0;
    stats->txn_rts_hs_restore_updates = 0;
    stats->txn_rts_delete_rle_skipped = 0;
    stats->txn_rts_stable_rle_skipped = 0;
    stats->txn_rts_sweep_hs_keys = 0;
    stats->txn_rts_hs_restore_tombstones_dryrun = 0;
    stats->txn_rts_hs_restore_updates_dryrun = 0;
    stats->txn_rts_hs_removed = 0;
    stats->txn_rts_hs_removed_dryrun = 0;
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
    to->autocommit_readonly_retry += from->autocommit_readonly_retry;
    to->autocommit_update_retry += from->autocommit_update_retry;
    to->backup_blocks_compressed += from->backup_blocks_compressed;
    to->backup_blocks_uncompressed += from->backup_blocks_uncompressed;
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
    to->btree_compact_bytes_rewritten_expected += from->btree_compact_bytes_rewritten_expected;
    to->btree_compact_pages_rewritten_expected += from->btree_compact_pages_rewritten_expected;
    to->btree_checkpoint_pages_reconciled += from->btree_checkpoint_pages_reconciled;
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
    to->cache_eviction_app_threads_fill_ratio_lt_25 +=
      from->cache_eviction_app_threads_fill_ratio_lt_25;
    to->cache_eviction_app_threads_fill_ratio_25_50 +=
      from->cache_eviction_app_threads_fill_ratio_25_50;
    to->cache_eviction_app_threads_fill_ratio_50_75 +=
      from->cache_eviction_app_threads_fill_ratio_50_75;
    to->cache_eviction_app_threads_fill_ratio_gt_75 +=
      from->cache_eviction_app_threads_fill_ratio_gt_75;
    to->cache_bytes_inuse += from->cache_bytes_inuse;
    to->cache_bytes_dirty_total += from->cache_bytes_dirty_total;
    to->cache_bytes_read += from->cache_bytes_read;
    to->cache_bytes_write += from->cache_bytes_write;
    to->cache_eviction_blocked_checkpoint += from->cache_eviction_blocked_checkpoint;
    to->cache_eviction_blocked_checkpoint_hs += from->cache_eviction_blocked_checkpoint_hs;
    to->cache_eviction_fail += from->cache_eviction_fail;
    to->cache_eviction_blocked_no_ts_checkpoint_race_1 +=
      from->cache_eviction_blocked_no_ts_checkpoint_race_1;
    to->cache_eviction_blocked_no_ts_checkpoint_race_2 +=
      from->cache_eviction_blocked_no_ts_checkpoint_race_2;
    to->cache_eviction_blocked_no_ts_checkpoint_race_3 +=
      from->cache_eviction_blocked_no_ts_checkpoint_race_3;
    to->cache_eviction_blocked_no_ts_checkpoint_race_4 +=
      from->cache_eviction_blocked_no_ts_checkpoint_race_4;
    to->cache_eviction_blocked_remove_hs_race_with_checkpoint +=
      from->cache_eviction_blocked_remove_hs_race_with_checkpoint;
    to->cache_eviction_blocked_no_progress += from->cache_eviction_blocked_no_progress;
    to->cache_eviction_pages_queued_updates += from->cache_eviction_pages_queued_updates;
    to->cache_eviction_pages_queued_clean += from->cache_eviction_pages_queued_clean;
    to->cache_eviction_pages_queued_dirty += from->cache_eviction_pages_queued_dirty;
    to->cache_eviction_pages_seen_updates += from->cache_eviction_pages_seen_updates;
    to->cache_eviction_pages_seen_clean += from->cache_eviction_pages_seen_clean;
    to->cache_eviction_pages_seen_dirty += from->cache_eviction_pages_seen_dirty;
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
    to->cache_eviction_walk_random_returns_null_position +=
      from->cache_eviction_walk_random_returns_null_position;
    to->cache_eviction_walks_ended += from->cache_eviction_walks_ended;
    to->cache_eviction_walk_restart += from->cache_eviction_walk_restart;
    to->cache_eviction_walk_from_root += from->cache_eviction_walk_from_root;
    to->cache_eviction_walk_saved_pos += from->cache_eviction_walk_saved_pos;
    to->cache_eviction_blocked_hazard += from->cache_eviction_blocked_hazard;
    to->cache_hs_insert += from->cache_hs_insert;
    to->cache_hs_insert_restart += from->cache_hs_insert_restart;
    to->cache_hs_read += from->cache_hs_read;
    to->cache_hs_read_miss += from->cache_hs_read_miss;
    to->cache_hs_read_squash += from->cache_hs_read_squash;
    to->cache_hs_order_lose_durable_timestamp += from->cache_hs_order_lose_durable_timestamp;
    to->cache_hs_key_truncate_rts_unstable += from->cache_hs_key_truncate_rts_unstable;
    to->cache_hs_key_truncate_rts += from->cache_hs_key_truncate_rts;
    to->cache_hs_btree_truncate += from->cache_hs_btree_truncate;
    to->cache_hs_key_truncate += from->cache_hs_key_truncate;
    to->cache_hs_order_remove += from->cache_hs_order_remove;
    to->cache_hs_key_truncate_onpage_removal += from->cache_hs_key_truncate_onpage_removal;
    to->cache_hs_btree_truncate_dryrun += from->cache_hs_btree_truncate_dryrun;
    to->cache_hs_key_truncate_rts_unstable_dryrun +=
      from->cache_hs_key_truncate_rts_unstable_dryrun;
    to->cache_hs_key_truncate_rts_dryrun += from->cache_hs_key_truncate_rts_dryrun;
    to->cache_hs_order_reinsert += from->cache_hs_order_reinsert;
    to->cache_hs_write_squash += from->cache_hs_write_squash;
    to->cache_inmem_splittable += from->cache_inmem_splittable;
    to->cache_inmem_split += from->cache_inmem_split;
    to->cache_eviction_blocked_internal_page_split +=
      from->cache_eviction_blocked_internal_page_split;
    to->cache_eviction_internal += from->cache_eviction_internal;
    to->cache_eviction_split_internal += from->cache_eviction_split_internal;
    to->cache_eviction_split_leaf += from->cache_eviction_split_leaf;
    to->cache_eviction_random_sample_inmem_root += from->cache_eviction_random_sample_inmem_root;
    to->cache_eviction_dirty += from->cache_eviction_dirty;
    to->cache_eviction_blocked_multi_block_reconcilation_during_checkpoint +=
      from->cache_eviction_blocked_multi_block_reconcilation_during_checkpoint;
    to->cache_eviction_trigger_dirty_reached += from->cache_eviction_trigger_dirty_reached;
    to->cache_eviction_trigger_reached += from->cache_eviction_trigger_reached;
    to->cache_eviction_trigger_updates_reached += from->cache_eviction_trigger_updates_reached;
    to->cache_eviction_blocked_overflow_keys += from->cache_eviction_blocked_overflow_keys;
    to->cache_read_overflow += from->cache_read_overflow;
    to->cache_eviction_deepen += from->cache_eviction_deepen;
    to->cache_write_hs += from->cache_write_hs;
    to->cache_eviction_dirty_obsolete_tw += from->cache_eviction_dirty_obsolete_tw;
    to->cache_read += from->cache_read;
    to->cache_read_deleted += from->cache_read_deleted;
    to->cache_read_deleted_prepared += from->cache_read_deleted_prepared;
    to->cache_read_checkpoint += from->cache_read_checkpoint;
    to->cache_pages_requested += from->cache_pages_requested;
    to->cache_pages_prefetch += from->cache_pages_prefetch;
    to->cache_eviction_pages_seen += from->cache_eviction_pages_seen;
    to->cache_write += from->cache_write;
    to->cache_write_restore += from->cache_write_restore;
    to->cache_eviction_blocked_recently_modified += from->cache_eviction_blocked_recently_modified;
    to->cache_reverse_splits += from->cache_reverse_splits;
    to->cache_reverse_splits_skipped_vlcs += from->cache_reverse_splits_skipped_vlcs;
    to->cache_hs_insert_full_update += from->cache_hs_insert_full_update;
    to->cache_hs_insert_reverse_modify += from->cache_hs_insert_reverse_modify;
    to->cache_bytes_dirty += from->cache_bytes_dirty;
    to->cache_bytes_dirty_internal += from->cache_bytes_dirty_internal;
    to->cache_bytes_dirty_leaf += from->cache_bytes_dirty_leaf;
    to->cache_eviction_blocked_uncommitted_truncate +=
      from->cache_eviction_blocked_uncommitted_truncate;
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
    to->checkpoint_snapshot_acquired += from->checkpoint_snapshot_acquired;
    to->checkpoint_cleanup_pages_evict += from->checkpoint_cleanup_pages_evict;
    to->checkpoint_cleanup_pages_obsolete_tw += from->checkpoint_cleanup_pages_obsolete_tw;
    to->checkpoint_cleanup_pages_read_reclaim_space +=
      from->checkpoint_cleanup_pages_read_reclaim_space;
    to->checkpoint_cleanup_pages_read_obsolete_tw +=
      from->checkpoint_cleanup_pages_read_obsolete_tw;
    to->checkpoint_cleanup_pages_removed += from->checkpoint_cleanup_pages_removed;
    to->checkpoint_cleanup_pages_walk_skipped += from->checkpoint_cleanup_pages_walk_skipped;
    to->checkpoint_cleanup_pages_visited += from->checkpoint_cleanup_pages_visited;
    to->checkpoint_obsolete_applied += from->checkpoint_obsolete_applied;
    to->compress_precomp_intl_max_page_size += from->compress_precomp_intl_max_page_size;
    to->compress_precomp_leaf_max_page_size += from->compress_precomp_leaf_max_page_size;
    to->compress_write_fail += from->compress_write_fail;
    to->compress_write_too_small += from->compress_write_too_small;
    to->compress_read += from->compress_read;
    to->compress_read_ratio_hist_max += from->compress_read_ratio_hist_max;
    to->compress_read_ratio_hist_2 += from->compress_read_ratio_hist_2;
    to->compress_read_ratio_hist_4 += from->compress_read_ratio_hist_4;
    to->compress_read_ratio_hist_8 += from->compress_read_ratio_hist_8;
    to->compress_read_ratio_hist_16 += from->compress_read_ratio_hist_16;
    to->compress_read_ratio_hist_32 += from->compress_read_ratio_hist_32;
    to->compress_read_ratio_hist_64 += from->compress_read_ratio_hist_64;
    to->compress_write += from->compress_write;
    to->compress_write_ratio_hist_max += from->compress_write_ratio_hist_max;
    to->compress_write_ratio_hist_2 += from->compress_write_ratio_hist_2;
    to->compress_write_ratio_hist_4 += from->compress_write_ratio_hist_4;
    to->compress_write_ratio_hist_8 += from->compress_write_ratio_hist_8;
    to->compress_write_ratio_hist_16 += from->compress_write_ratio_hist_16;
    to->compress_write_ratio_hist_32 += from->compress_write_ratio_hist_32;
    to->compress_write_ratio_hist_64 += from->compress_write_ratio_hist_64;
    to->cursor_tree_walk_del_page_skip += from->cursor_tree_walk_del_page_skip;
    to->cursor_next_skip_total += from->cursor_next_skip_total;
    to->cursor_prev_skip_total += from->cursor_prev_skip_total;
    to->cursor_skip_hs_cur_position += from->cursor_skip_hs_cur_position;
    to->cursor_tree_walk_inmem_del_page_skip += from->cursor_tree_walk_inmem_del_page_skip;
    to->cursor_tree_walk_ondisk_del_page_skip += from->cursor_tree_walk_ondisk_del_page_skip;
    to->cursor_search_near_prefix_fast_paths += from->cursor_search_near_prefix_fast_paths;
    to->cursor_reposition_failed += from->cursor_reposition_failed;
    to->cursor_reposition += from->cursor_reposition;
    to->cursor_insert_bulk += from->cursor_insert_bulk;
    to->cursor_reopen += from->cursor_reopen;
    to->cursor_cache += from->cursor_cache;
    to->cursor_create += from->cursor_create;
    to->cursor_bound_error += from->cursor_bound_error;
    to->cursor_bounds_reset += from->cursor_bounds_reset;
    to->cursor_bounds_comparisons += from->cursor_bounds_comparisons;
    to->cursor_bounds_next_unpositioned += from->cursor_bounds_next_unpositioned;
    to->cursor_bounds_next_early_exit += from->cursor_bounds_next_early_exit;
    to->cursor_bounds_prev_unpositioned += from->cursor_bounds_prev_unpositioned;
    to->cursor_bounds_prev_early_exit += from->cursor_bounds_prev_early_exit;
    to->cursor_bounds_search_early_exit += from->cursor_bounds_search_early_exit;
    to->cursor_bounds_search_near_repositioned_cursor +=
      from->cursor_bounds_search_near_repositioned_cursor;
    to->cursor_cache_error += from->cursor_cache_error;
    to->cursor_close_error += from->cursor_close_error;
    to->cursor_compare_error += from->cursor_compare_error;
    to->cursor_equals_error += from->cursor_equals_error;
    to->cursor_get_key_error += from->cursor_get_key_error;
    to->cursor_get_value_error += from->cursor_get_value_error;
    to->cursor_insert_error += from->cursor_insert_error;
    to->cursor_insert_check_error += from->cursor_insert_check_error;
    to->cursor_largest_key_error += from->cursor_largest_key_error;
    to->cursor_modify_error += from->cursor_modify_error;
    to->cursor_next_error += from->cursor_next_error;
    to->cursor_next_hs_tombstone += from->cursor_next_hs_tombstone;
    to->cursor_next_skip_lt_100 += from->cursor_next_skip_lt_100;
    to->cursor_next_skip_ge_100 += from->cursor_next_skip_ge_100;
    to->cursor_next_random_error += from->cursor_next_random_error;
    to->cursor_prev_error += from->cursor_prev_error;
    to->cursor_prev_hs_tombstone += from->cursor_prev_hs_tombstone;
    to->cursor_prev_skip_ge_100 += from->cursor_prev_skip_ge_100;
    to->cursor_prev_skip_lt_100 += from->cursor_prev_skip_lt_100;
    to->cursor_reconfigure_error += from->cursor_reconfigure_error;
    to->cursor_remove_error += from->cursor_remove_error;
    to->cursor_reopen_error += from->cursor_reopen_error;
    to->cursor_reserve_error += from->cursor_reserve_error;
    to->cursor_reset_error += from->cursor_reset_error;
    to->cursor_search_error += from->cursor_search_error;
    to->cursor_search_near_error += from->cursor_search_near_error;
    to->cursor_update_error += from->cursor_update_error;
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
    to->rec_vlcs_emptied_pages += from->rec_vlcs_emptied_pages;
    to->rec_time_window_bytes_ts += from->rec_time_window_bytes_ts;
    to->rec_time_window_bytes_txn += from->rec_time_window_bytes_txn;
    to->rec_hs_wrapup_next_prev_calls += from->rec_hs_wrapup_next_prev_calls;
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
    to->txn_read_race_prepare_commit += from->txn_read_race_prepare_commit;
    to->txn_read_overflow_remove += from->txn_read_overflow_remove;
    to->txn_read_race_prepare_update += from->txn_read_race_prepare_update;
    to->txn_rts_sweep_hs_keys_dryrun += from->txn_rts_sweep_hs_keys_dryrun;
    to->txn_rts_hs_stop_older_than_newer_start += from->txn_rts_hs_stop_older_than_newer_start;
    to->txn_rts_inconsistent_ckpt += from->txn_rts_inconsistent_ckpt;
    to->txn_rts_keys_removed += from->txn_rts_keys_removed;
    to->txn_rts_keys_restored += from->txn_rts_keys_restored;
    to->txn_rts_keys_removed_dryrun += from->txn_rts_keys_removed_dryrun;
    to->txn_rts_keys_restored_dryrun += from->txn_rts_keys_restored_dryrun;
    to->txn_rts_hs_restore_tombstones += from->txn_rts_hs_restore_tombstones;
    to->txn_rts_hs_restore_updates += from->txn_rts_hs_restore_updates;
    to->txn_rts_delete_rle_skipped += from->txn_rts_delete_rle_skipped;
    to->txn_rts_stable_rle_skipped += from->txn_rts_stable_rle_skipped;
    to->txn_rts_sweep_hs_keys += from->txn_rts_sweep_hs_keys;
    to->txn_rts_hs_restore_tombstones_dryrun += from->txn_rts_hs_restore_tombstones_dryrun;
    to->txn_rts_hs_restore_updates_dryrun += from->txn_rts_hs_restore_updates_dryrun;
    to->txn_rts_hs_removed += from->txn_rts_hs_removed;
    to->txn_rts_hs_removed_dryrun += from->txn_rts_hs_removed_dryrun;
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
    to->autocommit_readonly_retry += WT_STAT_READ(from, autocommit_readonly_retry);
    to->autocommit_update_retry += WT_STAT_READ(from, autocommit_update_retry);
    to->backup_blocks_compressed += WT_STAT_READ(from, backup_blocks_compressed);
    to->backup_blocks_uncompressed += WT_STAT_READ(from, backup_blocks_uncompressed);
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
    to->btree_compact_bytes_rewritten_expected +=
      WT_STAT_READ(from, btree_compact_bytes_rewritten_expected);
    to->btree_compact_pages_rewritten_expected +=
      WT_STAT_READ(from, btree_compact_pages_rewritten_expected);
    to->btree_checkpoint_pages_reconciled += WT_STAT_READ(from, btree_checkpoint_pages_reconciled);
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
    to->cache_eviction_app_threads_fill_ratio_lt_25 +=
      WT_STAT_READ(from, cache_eviction_app_threads_fill_ratio_lt_25);
    to->cache_eviction_app_threads_fill_ratio_25_50 +=
      WT_STAT_READ(from, cache_eviction_app_threads_fill_ratio_25_50);
    to->cache_eviction_app_threads_fill_ratio_50_75 +=
      WT_STAT_READ(from, cache_eviction_app_threads_fill_ratio_50_75);
    to->cache_eviction_app_threads_fill_ratio_gt_75 +=
      WT_STAT_READ(from, cache_eviction_app_threads_fill_ratio_gt_75);
    to->cache_bytes_inuse += WT_STAT_READ(from, cache_bytes_inuse);
    to->cache_bytes_dirty_total += WT_STAT_READ(from, cache_bytes_dirty_total);
    to->cache_bytes_read += WT_STAT_READ(from, cache_bytes_read);
    to->cache_bytes_write += WT_STAT_READ(from, cache_bytes_write);
    to->cache_eviction_blocked_checkpoint += WT_STAT_READ(from, cache_eviction_blocked_checkpoint);
    to->cache_eviction_blocked_checkpoint_hs +=
      WT_STAT_READ(from, cache_eviction_blocked_checkpoint_hs);
    to->cache_eviction_fail += WT_STAT_READ(from, cache_eviction_fail);
    to->cache_eviction_blocked_no_ts_checkpoint_race_1 +=
      WT_STAT_READ(from, cache_eviction_blocked_no_ts_checkpoint_race_1);
    to->cache_eviction_blocked_no_ts_checkpoint_race_2 +=
      WT_STAT_READ(from, cache_eviction_blocked_no_ts_checkpoint_race_2);
    to->cache_eviction_blocked_no_ts_checkpoint_race_3 +=
      WT_STAT_READ(from, cache_eviction_blocked_no_ts_checkpoint_race_3);
    to->cache_eviction_blocked_no_ts_checkpoint_race_4 +=
      WT_STAT_READ(from, cache_eviction_blocked_no_ts_checkpoint_race_4);
    to->cache_eviction_blocked_remove_hs_race_with_checkpoint +=
      WT_STAT_READ(from, cache_eviction_blocked_remove_hs_race_with_checkpoint);
    to->cache_eviction_blocked_no_progress +=
      WT_STAT_READ(from, cache_eviction_blocked_no_progress);
    to->cache_eviction_pages_queued_updates +=
      WT_STAT_READ(from, cache_eviction_pages_queued_updates);
    to->cache_eviction_pages_queued_clean += WT_STAT_READ(from, cache_eviction_pages_queued_clean);
    to->cache_eviction_pages_queued_dirty += WT_STAT_READ(from, cache_eviction_pages_queued_dirty);
    to->cache_eviction_pages_seen_updates += WT_STAT_READ(from, cache_eviction_pages_seen_updates);
    to->cache_eviction_pages_seen_clean += WT_STAT_READ(from, cache_eviction_pages_seen_clean);
    to->cache_eviction_pages_seen_dirty += WT_STAT_READ(from, cache_eviction_pages_seen_dirty);
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
    to->cache_eviction_walk_random_returns_null_position +=
      WT_STAT_READ(from, cache_eviction_walk_random_returns_null_position);
    to->cache_eviction_walks_ended += WT_STAT_READ(from, cache_eviction_walks_ended);
    to->cache_eviction_walk_restart += WT_STAT_READ(from, cache_eviction_walk_restart);
    to->cache_eviction_walk_from_root += WT_STAT_READ(from, cache_eviction_walk_from_root);
    to->cache_eviction_walk_saved_pos += WT_STAT_READ(from, cache_eviction_walk_saved_pos);
    to->cache_eviction_blocked_hazard += WT_STAT_READ(from, cache_eviction_blocked_hazard);
    to->cache_hs_insert += WT_STAT_READ(from, cache_hs_insert);
    to->cache_hs_insert_restart += WT_STAT_READ(from, cache_hs_insert_restart);
    to->cache_hs_read += WT_STAT_READ(from, cache_hs_read);
    to->cache_hs_read_miss += WT_STAT_READ(from, cache_hs_read_miss);
    to->cache_hs_read_squash += WT_STAT_READ(from, cache_hs_read_squash);
    to->cache_hs_order_lose_durable_timestamp +=
      WT_STAT_READ(from, cache_hs_order_lose_durable_timestamp);
    to->cache_hs_key_truncate_rts_unstable +=
      WT_STAT_READ(from, cache_hs_key_truncate_rts_unstable);
    to->cache_hs_key_truncate_rts += WT_STAT_READ(from, cache_hs_key_truncate_rts);
    to->cache_hs_btree_truncate += WT_STAT_READ(from, cache_hs_btree_truncate);
    to->cache_hs_key_truncate += WT_STAT_READ(from, cache_hs_key_truncate);
    to->cache_hs_order_remove += WT_STAT_READ(from, cache_hs_order_remove);
    to->cache_hs_key_truncate_onpage_removal +=
      WT_STAT_READ(from, cache_hs_key_truncate_onpage_removal);
    to->cache_hs_btree_truncate_dryrun += WT_STAT_READ(from, cache_hs_btree_truncate_dryrun);
    to->cache_hs_key_truncate_rts_unstable_dryrun +=
      WT_STAT_READ(from, cache_hs_key_truncate_rts_unstable_dryrun);
    to->cache_hs_key_truncate_rts_dryrun += WT_STAT_READ(from, cache_hs_key_truncate_rts_dryrun);
    to->cache_hs_order_reinsert += WT_STAT_READ(from, cache_hs_order_reinsert);
    to->cache_hs_write_squash += WT_STAT_READ(from, cache_hs_write_squash);
    to->cache_inmem_splittable += WT_STAT_READ(from, cache_inmem_splittable);
    to->cache_inmem_split += WT_STAT_READ(from, cache_inmem_split);
    to->cache_eviction_blocked_internal_page_split +=
      WT_STAT_READ(from, cache_eviction_blocked_internal_page_split);
    to->cache_eviction_internal += WT_STAT_READ(from, cache_eviction_internal);
    to->cache_eviction_split_internal += WT_STAT_READ(from, cache_eviction_split_internal);
    to->cache_eviction_split_leaf += WT_STAT_READ(from, cache_eviction_split_leaf);
    to->cache_eviction_random_sample_inmem_root +=
      WT_STAT_READ(from, cache_eviction_random_sample_inmem_root);
    to->cache_eviction_dirty += WT_STAT_READ(from, cache_eviction_dirty);
    to->cache_eviction_blocked_multi_block_reconcilation_during_checkpoint +=
      WT_STAT_READ(from, cache_eviction_blocked_multi_block_reconcilation_during_checkpoint);
    to->cache_eviction_trigger_dirty_reached +=
      WT_STAT_READ(from, cache_eviction_trigger_dirty_reached);
    to->cache_eviction_trigger_reached += WT_STAT_READ(from, cache_eviction_trigger_reached);
    to->cache_eviction_trigger_updates_reached +=
      WT_STAT_READ(from, cache_eviction_trigger_updates_reached);
    to->cache_eviction_blocked_overflow_keys +=
      WT_STAT_READ(from, cache_eviction_blocked_overflow_keys);
    to->cache_read_overflow += WT_STAT_READ(from, cache_read_overflow);
    to->cache_eviction_deepen += WT_STAT_READ(from, cache_eviction_deepen);
    to->cache_write_hs += WT_STAT_READ(from, cache_write_hs);
    to->cache_eviction_dirty_obsolete_tw += WT_STAT_READ(from, cache_eviction_dirty_obsolete_tw);
    to->cache_read += WT_STAT_READ(from, cache_read);
    to->cache_read_deleted += WT_STAT_READ(from, cache_read_deleted);
    to->cache_read_deleted_prepared += WT_STAT_READ(from, cache_read_deleted_prepared);
    to->cache_read_checkpoint += WT_STAT_READ(from, cache_read_checkpoint);
    to->cache_pages_requested += WT_STAT_READ(from, cache_pages_requested);
    to->cache_pages_prefetch += WT_STAT_READ(from, cache_pages_prefetch);
    to->cache_eviction_pages_seen += WT_STAT_READ(from, cache_eviction_pages_seen);
    to->cache_write += WT_STAT_READ(from, cache_write);
    to->cache_write_restore += WT_STAT_READ(from, cache_write_restore);
    to->cache_eviction_blocked_recently_modified +=
      WT_STAT_READ(from, cache_eviction_blocked_recently_modified);
    to->cache_reverse_splits += WT_STAT_READ(from, cache_reverse_splits);
    to->cache_reverse_splits_skipped_vlcs += WT_STAT_READ(from, cache_reverse_splits_skipped_vlcs);
    to->cache_hs_insert_full_update += WT_STAT_READ(from, cache_hs_insert_full_update);
    to->cache_hs_insert_reverse_modify += WT_STAT_READ(from, cache_hs_insert_reverse_modify);
    to->cache_bytes_dirty += WT_STAT_READ(from, cache_bytes_dirty);
    to->cache_bytes_dirty_internal += WT_STAT_READ(from, cache_bytes_dirty_internal);
    to->cache_bytes_dirty_leaf += WT_STAT_READ(from, cache_bytes_dirty_leaf);
    to->cache_eviction_blocked_uncommitted_truncate +=
      WT_STAT_READ(from, cache_eviction_blocked_uncommitted_truncate);
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
    to->checkpoint_snapshot_acquired += WT_STAT_READ(from, checkpoint_snapshot_acquired);
    to->checkpoint_cleanup_pages_evict += WT_STAT_READ(from, checkpoint_cleanup_pages_evict);
    to->checkpoint_cleanup_pages_obsolete_tw +=
      WT_STAT_READ(from, checkpoint_cleanup_pages_obsolete_tw);
    to->checkpoint_cleanup_pages_read_reclaim_space +=
      WT_STAT_READ(from, checkpoint_cleanup_pages_read_reclaim_space);
    to->checkpoint_cleanup_pages_read_obsolete_tw +=
      WT_STAT_READ(from, checkpoint_cleanup_pages_read_obsolete_tw);
    to->checkpoint_cleanup_pages_removed += WT_STAT_READ(from, checkpoint_cleanup_pages_removed);
    to->checkpoint_cleanup_pages_walk_skipped +=
      WT_STAT_READ(from, checkpoint_cleanup_pages_walk_skipped);
    to->checkpoint_cleanup_pages_visited += WT_STAT_READ(from, checkpoint_cleanup_pages_visited);
    to->checkpoint_obsolete_applied += WT_STAT_READ(from, checkpoint_obsolete_applied);
    to->compress_precomp_intl_max_page_size +=
      WT_STAT_READ(from, compress_precomp_intl_max_page_size);
    to->compress_precomp_leaf_max_page_size +=
      WT_STAT_READ(from, compress_precomp_leaf_max_page_size);
    to->compress_write_fail += WT_STAT_READ(from, compress_write_fail);
    to->compress_write_too_small += WT_STAT_READ(from, compress_write_too_small);
    to->compress_read += WT_STAT_READ(from, compress_read);
    to->compress_read_ratio_hist_max += WT_STAT_READ(from, compress_read_ratio_hist_max);
    to->compress_read_ratio_hist_2 += WT_STAT_READ(from, compress_read_ratio_hist_2);
    to->compress_read_ratio_hist_4 += WT_STAT_READ(from, compress_read_ratio_hist_4);
    to->compress_read_ratio_hist_8 += WT_STAT_READ(from, compress_read_ratio_hist_8);
    to->compress_read_ratio_hist_16 += WT_STAT_READ(from, compress_read_ratio_hist_16);
    to->compress_read_ratio_hist_32 += WT_STAT_READ(from, compress_read_ratio_hist_32);
    to->compress_read_ratio_hist_64 += WT_STAT_READ(from, compress_read_ratio_hist_64);
    to->compress_write += WT_STAT_READ(from, compress_write);
    to->compress_write_ratio_hist_max += WT_STAT_READ(from, compress_write_ratio_hist_max);
    to->compress_write_ratio_hist_2 += WT_STAT_READ(from, compress_write_ratio_hist_2);
    to->compress_write_ratio_hist_4 += WT_STAT_READ(from, compress_write_ratio_hist_4);
    to->compress_write_ratio_hist_8 += WT_STAT_READ(from, compress_write_ratio_hist_8);
    to->compress_write_ratio_hist_16 += WT_STAT_READ(from, compress_write_ratio_hist_16);
    to->compress_write_ratio_hist_32 += WT_STAT_READ(from, compress_write_ratio_hist_32);
    to->compress_write_ratio_hist_64 += WT_STAT_READ(from, compress_write_ratio_hist_64);
    to->cursor_tree_walk_del_page_skip += WT_STAT_READ(from, cursor_tree_walk_del_page_skip);
    to->cursor_next_skip_total += WT_STAT_READ(from, cursor_next_skip_total);
    to->cursor_prev_skip_total += WT_STAT_READ(from, cursor_prev_skip_total);
    to->cursor_skip_hs_cur_position += WT_STAT_READ(from, cursor_skip_hs_cur_position);
    to->cursor_tree_walk_inmem_del_page_skip +=
      WT_STAT_READ(from, cursor_tree_walk_inmem_del_page_skip);
    to->cursor_tree_walk_ondisk_del_page_skip +=
      WT_STAT_READ(from, cursor_tree_walk_ondisk_del_page_skip);
    to->cursor_search_near_prefix_fast_paths +=
      WT_STAT_READ(from, cursor_search_near_prefix_fast_paths);
    to->cursor_reposition_failed += WT_STAT_READ(from, cursor_reposition_failed);
    to->cursor_reposition += WT_STAT_READ(from, cursor_reposition);
    to->cursor_insert_bulk += WT_STAT_READ(from, cursor_insert_bulk);
    to->cursor_reopen += WT_STAT_READ(from, cursor_reopen);
    to->cursor_cache += WT_STAT_READ(from, cursor_cache);
    to->cursor_create += WT_STAT_READ(from, cursor_create);
    to->cursor_bound_error += WT_STAT_READ(from, cursor_bound_error);
    to->cursor_bounds_reset += WT_STAT_READ(from, cursor_bounds_reset);
    to->cursor_bounds_comparisons += WT_STAT_READ(from, cursor_bounds_comparisons);
    to->cursor_bounds_next_unpositioned += WT_STAT_READ(from, cursor_bounds_next_unpositioned);
    to->cursor_bounds_next_early_exit += WT_STAT_READ(from, cursor_bounds_next_early_exit);
    to->cursor_bounds_prev_unpositioned += WT_STAT_READ(from, cursor_bounds_prev_unpositioned);
    to->cursor_bounds_prev_early_exit += WT_STAT_READ(from, cursor_bounds_prev_early_exit);
    to->cursor_bounds_search_early_exit += WT_STAT_READ(from, cursor_bounds_search_early_exit);
    to->cursor_bounds_search_near_repositioned_cursor +=
      WT_STAT_READ(from, cursor_bounds_search_near_repositioned_cursor);
    to->cursor_cache_error += WT_STAT_READ(from, cursor_cache_error);
    to->cursor_close_error += WT_STAT_READ(from, cursor_close_error);
    to->cursor_compare_error += WT_STAT_READ(from, cursor_compare_error);
    to->cursor_equals_error += WT_STAT_READ(from, cursor_equals_error);
    to->cursor_get_key_error += WT_STAT_READ(from, cursor_get_key_error);
    to->cursor_get_value_error += WT_STAT_READ(from, cursor_get_value_error);
    to->cursor_insert_error += WT_STAT_READ(from, cursor_insert_error);
    to->cursor_insert_check_error += WT_STAT_READ(from, cursor_insert_check_error);
    to->cursor_largest_key_error += WT_STAT_READ(from, cursor_largest_key_error);
    to->cursor_modify_error += WT_STAT_READ(from, cursor_modify_error);
    to->cursor_next_error += WT_STAT_READ(from, cursor_next_error);
    to->cursor_next_hs_tombstone += WT_STAT_READ(from, cursor_next_hs_tombstone);
    to->cursor_next_skip_lt_100 += WT_STAT_READ(from, cursor_next_skip_lt_100);
    to->cursor_next_skip_ge_100 += WT_STAT_READ(from, cursor_next_skip_ge_100);
    to->cursor_next_random_error += WT_STAT_READ(from, cursor_next_random_error);
    to->cursor_prev_error += WT_STAT_READ(from, cursor_prev_error);
    to->cursor_prev_hs_tombstone += WT_STAT_READ(from, cursor_prev_hs_tombstone);
    to->cursor_prev_skip_ge_100 += WT_STAT_READ(from, cursor_prev_skip_ge_100);
    to->cursor_prev_skip_lt_100 += WT_STAT_READ(from, cursor_prev_skip_lt_100);
    to->cursor_reconfigure_error += WT_STAT_READ(from, cursor_reconfigure_error);
    to->cursor_remove_error += WT_STAT_READ(from, cursor_remove_error);
    to->cursor_reopen_error += WT_STAT_READ(from, cursor_reopen_error);
    to->cursor_reserve_error += WT_STAT_READ(from, cursor_reserve_error);
    to->cursor_reset_error += WT_STAT_READ(from, cursor_reset_error);
    to->cursor_search_error += WT_STAT_READ(from, cursor_search_error);
    to->cursor_search_near_error += WT_STAT_READ(from, cursor_search_near_error);
    to->cursor_update_error += WT_STAT_READ(from, cursor_update_error);
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
    to->rec_vlcs_emptied_pages += WT_STAT_READ(from, rec_vlcs_emptied_pages);
    to->rec_time_window_bytes_ts += WT_STAT_READ(from, rec_time_window_bytes_ts);
    to->rec_time_window_bytes_txn += WT_STAT_READ(from, rec_time_window_bytes_txn);
    to->rec_hs_wrapup_next_prev_calls += WT_STAT_READ(from, rec_hs_wrapup_next_prev_calls);
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
    to->txn_read_race_prepare_commit += WT_STAT_READ(from, txn_read_race_prepare_commit);
    to->txn_read_overflow_remove += WT_STAT_READ(from, txn_read_overflow_remove);
    to->txn_read_race_prepare_update += WT_STAT_READ(from, txn_read_race_prepare_update);
    to->txn_rts_sweep_hs_keys_dryrun += WT_STAT_READ(from, txn_rts_sweep_hs_keys_dryrun);
    to->txn_rts_hs_stop_older_than_newer_start +=
      WT_STAT_READ(from, txn_rts_hs_stop_older_than_newer_start);
    to->txn_rts_inconsistent_ckpt += WT_STAT_READ(from, txn_rts_inconsistent_ckpt);
    to->txn_rts_keys_removed += WT_STAT_READ(from, txn_rts_keys_removed);
    to->txn_rts_keys_restored += WT_STAT_READ(from, txn_rts_keys_restored);
    to->txn_rts_keys_removed_dryrun += WT_STAT_READ(from, txn_rts_keys_removed_dryrun);
    to->txn_rts_keys_restored_dryrun += WT_STAT_READ(from, txn_rts_keys_restored_dryrun);
    to->txn_rts_hs_restore_tombstones += WT_STAT_READ(from, txn_rts_hs_restore_tombstones);
    to->txn_rts_hs_restore_updates += WT_STAT_READ(from, txn_rts_hs_restore_updates);
    to->txn_rts_delete_rle_skipped += WT_STAT_READ(from, txn_rts_delete_rle_skipped);
    to->txn_rts_stable_rle_skipped += WT_STAT_READ(from, txn_rts_stable_rle_skipped);
    to->txn_rts_sweep_hs_keys += WT_STAT_READ(from, txn_rts_sweep_hs_keys);
    to->txn_rts_hs_restore_tombstones_dryrun +=
      WT_STAT_READ(from, txn_rts_hs_restore_tombstones_dryrun);
    to->txn_rts_hs_restore_updates_dryrun += WT_STAT_READ(from, txn_rts_hs_restore_updates_dryrun);
    to->txn_rts_hs_removed += WT_STAT_READ(from, txn_rts_hs_removed);
    to->txn_rts_hs_removed_dryrun += WT_STAT_READ(from, txn_rts_hs_removed_dryrun);
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
  "autocommit: retries for readonly operations",
  "autocommit: retries for update operations",
  "background-compact: background compact failed calls",
  "background-compact: background compact failed calls due to cache pressure",
  "background-compact: background compact interrupted",
  "background-compact: background compact moving average of bytes rewritten",
  "background-compact: background compact recovered bytes",
  "background-compact: background compact running",
  "background-compact: background compact skipped file as it is part of the exclude list",
  "background-compact: background compact skipped file as not meeting requirements for compaction",
  "background-compact: background compact sleeps due to cache pressure",
  "background-compact: background compact successful calls",
  "background-compact: background compact timeout",
  "background-compact: number of files tracked by background compaction",
  "backup: backup cursor open",
  "backup: backup duplicate cursor open",
  "backup: backup granularity size",
  "backup: incremental backup enabled",
  "backup: opening the backup cursor in progress",
  "backup: total modified incremental blocks",
  "backup: total modified incremental blocks with compressed data",
  "backup: total modified incremental blocks without compressed data",
  "block-cache: cached blocks updated",
  "block-cache: cached bytes updated",
  "block-cache: evicted blocks",
  "block-cache: file size causing bypass",
  "block-cache: lookups",
  "block-cache: number of blocks not evicted due to overhead",
  "block-cache: number of bypasses because no-write-allocate setting was on",
  "block-cache: number of bypasses due to overhead on put",
  "block-cache: number of bypasses on get",
  "block-cache: number of bypasses on put because file is too small",
  "block-cache: number of eviction passes",
  "block-cache: number of hits",
  "block-cache: number of misses",
  "block-cache: number of put bypasses on checkpoint I/O",
  "block-cache: removed blocks",
  "block-cache: time sleeping to remove block (usecs)",
  "block-cache: total blocks",
  "block-cache: total blocks inserted on read path",
  "block-cache: total blocks inserted on write path",
  "block-cache: total bytes",
  "block-cache: total bytes inserted on read path",
  "block-cache: total bytes inserted on write path",
  "block-manager: blocks pre-loaded",
  "block-manager: blocks read",
  "block-manager: blocks written",
  "block-manager: bytes read",
  "block-manager: bytes read via memory map API",
  "block-manager: bytes read via system call API",
  "block-manager: bytes written",
  "block-manager: bytes written by compaction",
  "block-manager: bytes written for checkpoint",
  "block-manager: bytes written via memory map API",
  "block-manager: bytes written via system call API",
  "block-manager: mapped blocks read",
  "block-manager: mapped bytes read",
  "block-manager: number of times the file was remapped because it changed size via fallocate or "
  "truncate",
  "block-manager: number of times the region was remapped via write",
  "cache: application thread time evicting (usecs)",
  "cache: application threads eviction requested with cache fill ratio < 25%",
  "cache: application threads eviction requested with cache fill ratio >= 25% and < 50%",
  "cache: application threads eviction requested with cache fill ratio >= 50% and < 75%",
  "cache: application threads eviction requested with cache fill ratio >= 75%",
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
  "cache: checkpoint blocked page eviction",
  "cache: checkpoint of history store file blocked non-history store page eviction",
  "cache: evict page attempts by eviction server",
  "cache: evict page attempts by eviction worker threads",
  "cache: evict page failures by eviction server",
  "cache: evict page failures by eviction worker threads",
  "cache: eviction calls to get a page",
  "cache: eviction calls to get a page found queue empty",
  "cache: eviction calls to get a page found queue empty after locking",
  "cache: eviction currently operating in aggressive mode",
  "cache: eviction empty score",
  "cache: eviction gave up due to detecting a disk value without a timestamp behind the last "
  "update on the chain",
  "cache: eviction gave up due to detecting a tombstone without a timestamp ahead of the selected "
  "on disk update",
  "cache: eviction gave up due to detecting a tombstone without a timestamp ahead of the selected "
  "on disk update after validating the update chain",
  "cache: eviction gave up due to detecting update chain entries without timestamps after the "
  "selected on disk update",
  "cache: eviction gave up due to needing to remove a record from the history store but checkpoint "
  "is running",
  "cache: eviction gave up due to no progress being made",
  "cache: eviction passes of a file",
  "cache: eviction server candidate queue empty when topping up",
  "cache: eviction server candidate queue not empty when topping up",
  "cache: eviction server skips dirty pages during a running checkpoint",
  "cache: eviction server skips internal pages as it has an active child.",
  "cache: eviction server skips metadata pages with history",
  "cache: eviction server skips pages that are written with transactions greater than the last "
  "running",
  "cache: eviction server skips pages that previously failed eviction and likely will again",
  "cache: eviction server skips pages that we do not want to evict",
  "cache: eviction server skips tree that we do not want to evict",
  "cache: eviction server skips trees because there are too many active walks",
  "cache: eviction server skips trees that are being checkpointed",
  "cache: eviction server skips trees that are configured to stick in cache",
  "cache: eviction server skips trees that disable eviction",
  "cache: eviction server skips trees that were not useful before",
  "cache: eviction server slept, because we did not make progress with eviction",
  "cache: eviction server unable to reach eviction goal",
  "cache: eviction server waiting for a leaf page",
  "cache: eviction state",
  "cache: eviction walk most recent sleeps for checkpoint handle gathering",
  "cache: eviction walk pages queued that had updates",
  "cache: eviction walk pages queued that were clean",
  "cache: eviction walk pages queued that were dirty",
  "cache: eviction walk pages seen that had updates",
  "cache: eviction walk pages seen that were clean",
  "cache: eviction walk pages seen that were dirty",
  "cache: eviction walk target pages histogram - 0-9",
  "cache: eviction walk target pages histogram - 10-31",
  "cache: eviction walk target pages histogram - 128 and higher",
  "cache: eviction walk target pages histogram - 32-63",
  "cache: eviction walk target pages histogram - 64-128",
  "cache: eviction walk target pages reduced due to history store cache pressure",
  "cache: eviction walk target strategy only clean pages",
  "cache: eviction walk target strategy only dirty pages",
  "cache: eviction walk target strategy pages with updates",
  "cache: eviction walks abandoned",
  "cache: eviction walks gave up because they restarted their walk twice",
  "cache: eviction walks gave up because they saw too many pages and found no candidates",
  "cache: eviction walks gave up because they saw too many pages and found too few candidates",
  "cache: eviction walks random search fails to locate a page, results in a null position",
  "cache: eviction walks reached end of tree",
  "cache: eviction walks restarted",
  "cache: eviction walks started from root of tree",
  "cache: eviction walks started from saved location in tree",
  "cache: eviction worker thread active",
  "cache: eviction worker thread created",
  "cache: eviction worker thread removed",
  "cache: eviction worker thread stable number",
  "cache: files with active eviction walks",
  "cache: files with new eviction walks started",
  "cache: force re-tuning of eviction workers once in a while",
  "cache: forced eviction - do not retry count to evict pages selected to evict during "
  "reconciliation",
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
  "cache: history store table insert calls",
  "cache: history store table insert calls that returned restart",
  "cache: history store table max on-disk size",
  "cache: history store table on-disk size",
  "cache: history store table reads",
  "cache: history store table reads missed",
  "cache: history store table reads requiring squashed modifies",
  "cache: history store table resolved updates without timestamps that lose their durable "
  "timestamp",
  "cache: history store table truncation by rollback to stable to remove an unstable update",
  "cache: history store table truncation by rollback to stable to remove an update",
  "cache: history store table truncation to remove all the keys of a btree",
  "cache: history store table truncation to remove an update",
  "cache: history store table truncation to remove range of updates due to an update without a "
  "timestamp on data page",
  "cache: history store table truncation to remove range of updates due to key being removed from "
  "the data page during reconciliation",
  "cache: history store table truncations that would have happened in non-dryrun mode",
  "cache: history store table truncations to remove an unstable update that would have happened in "
  "non-dryrun mode",
  "cache: history store table truncations to remove an update that would have happened in "
  "non-dryrun mode",
  "cache: history store table updates without timestamps fixed up by reinserting with the fixed "
  "timestamp",
  "cache: history store table writes requiring squashed modifies",
  "cache: in-memory page passed criteria to be split",
  "cache: in-memory page splits",
  "cache: internal page split blocked its eviction",
  "cache: internal pages evicted",
  "cache: internal pages queued for eviction",
  "cache: internal pages seen by eviction walk",
  "cache: internal pages seen by eviction walk that are already queued",
  "cache: internal pages split during eviction",
  "cache: leaf pages split during eviction",
  "cache: locate a random in-mem ref by examining all entries on the root page",
  "cache: maximum bytes configured",
  "cache: maximum gap between page and connection evict pass generation seen at eviction",
  "cache: maximum milliseconds spent at a single eviction",
  "cache: maximum page size seen at eviction",
  "cache: modified page evict attempts by application threads",
  "cache: modified page evict failures by application threads",
  "cache: modified pages evicted",
  "cache: multi-block reconciliation blocked whilst checkpoint is running",
  "cache: number of times dirty trigger was reached",
  "cache: number of times eviction trigger was reached",
  "cache: number of times updates trigger was reached",
  "cache: operations timed out waiting for space in cache",
  "cache: overflow keys on a multiblock row-store page blocked its eviction",
  "cache: overflow pages read into cache",
  "cache: page evict attempts by application threads",
  "cache: page evict failures by application threads",
  "cache: page split during eviction deepened the tree",
  "cache: page written requiring history store records",
  "cache: pages considered for eviction that were brought in by pre-fetch",
  "cache: pages currently held in the cache",
  "cache: pages dirtied due to obsolete time window by eviction",
  "cache: pages evicted in parallel with checkpoint",
  "cache: pages queued for eviction",
  "cache: pages queued for eviction post lru sorting",
  "cache: pages queued for urgent eviction",
  "cache: pages queued for urgent eviction during walk",
  "cache: pages queued for urgent eviction from history store due to high dirty content",
  "cache: pages read into cache",
  "cache: pages read into cache after truncate",
  "cache: pages read into cache after truncate in prepare state",
  "cache: pages read into cache by checkpoint",
  "cache: pages removed from the ordinary queue to be queued for urgent eviction",
  "cache: pages requested from the cache",
  "cache: pages requested from the cache due to pre-fetch",
  "cache: pages seen by eviction walk",
  "cache: pages seen by eviction walk that are already queued",
  "cache: pages selected for eviction unable to be evicted",
  "cache: pages selected for eviction unable to be evicted because of active children on an "
  "internal page",
  "cache: pages selected for eviction unable to be evicted because of failure in reconciliation",
  "cache: pages selected for eviction unable to be evicted because of race between checkpoint and "
  "updates without timestamps",
  "cache: pages walked for eviction",
  "cache: pages written from cache",
  "cache: pages written requiring in-memory restoration",
  "cache: percentage overhead",
  "cache: recent modification of a page blocked its eviction",
  "cache: reverse splits performed",
  "cache: reverse splits skipped because of VLCS namespace gap restrictions",
  "cache: the number of times full update inserted to history store",
  "cache: the number of times reverse modify inserted to history store",
  "cache: total milliseconds spent inside reentrant history store evictions in a reconciliation",
  "cache: tracked bytes belonging to internal pages in the cache",
  "cache: tracked bytes belonging to leaf pages in the cache",
  "cache: tracked dirty bytes in the cache",
  "cache: tracked dirty internal page bytes in the cache",
  "cache: tracked dirty leaf page bytes in the cache",
  "cache: tracked dirty pages in the cache",
  "cache: uncommitted truncate blocked page eviction",
  "cache: unmodified pages evicted",
  "capacity: background fsync file handles considered",
  "capacity: background fsync file handles synced",
  "capacity: background fsync time (msecs)",
  "capacity: bytes read",
  "capacity: bytes written for checkpoint",
  "capacity: bytes written for chunk cache",
  "capacity: bytes written for eviction",
  "capacity: bytes written for log",
  "capacity: bytes written total",
  "capacity: threshold to call fsync",
  "capacity: time waiting due to total capacity (usecs)",
  "capacity: time waiting during checkpoint (usecs)",
  "capacity: time waiting during eviction (usecs)",
  "capacity: time waiting during logging (usecs)",
  "capacity: time waiting during read (usecs)",
  "capacity: time waiting for chunk cache IO bandwidth (usecs)",
  "checkpoint: checkpoint cleanup successful calls",
  "checkpoint: checkpoint has acquired a snapshot for its transaction",
  "checkpoint: checkpoints skipped because database was clean",
  "checkpoint: fsync calls after allocating the transaction ID",
  "checkpoint: fsync duration after allocating the transaction ID (usecs)",
  "checkpoint: generation",
  "checkpoint: max time (msecs)",
  "checkpoint: min time (msecs)",
  "checkpoint: most recent duration for checkpoint dropping all handles (usecs)",
  "checkpoint: most recent duration for gathering all handles (usecs)",
  "checkpoint: most recent duration for gathering applied handles (usecs)",
  "checkpoint: most recent duration for gathering skipped handles (usecs)",
  "checkpoint: most recent duration for handles metadata checked (usecs)",
  "checkpoint: most recent duration for locking the handles (usecs)",
  "checkpoint: most recent handles applied",
  "checkpoint: most recent handles checkpoint dropped",
  "checkpoint: most recent handles metadata checked",
  "checkpoint: most recent handles metadata locked",
  "checkpoint: most recent handles skipped",
  "checkpoint: most recent handles walked",
  "checkpoint: most recent time (msecs)",
  "checkpoint: number of checkpoints started by api",
  "checkpoint: number of checkpoints started by compaction",
  "checkpoint: number of files synced",
  "checkpoint: number of handles visited after writes complete",
  "checkpoint: number of history store pages caused to be reconciled",
  "checkpoint: number of internal pages visited",
  "checkpoint: number of leaf pages visited",
  "checkpoint: number of pages caused to be reconciled",
  "checkpoint: pages added for eviction during checkpoint cleanup",
  "checkpoint: pages dirtied due to obsolete time window by checkpoint cleanup",
  "checkpoint: pages read into cache during checkpoint cleanup (reclaim_space)",
  "checkpoint: pages read into cache during checkpoint cleanup due to obsolete time window",
  "checkpoint: pages removed during checkpoint cleanup",
  "checkpoint: pages skipped during checkpoint cleanup tree walk",
  "checkpoint: pages visited during checkpoint cleanup",
  "checkpoint: prepare currently running",
  "checkpoint: prepare max time (msecs)",
  "checkpoint: prepare min time (msecs)",
  "checkpoint: prepare most recent time (msecs)",
  "checkpoint: prepare total time (msecs)",
  "checkpoint: progress state",
  "checkpoint: scrub dirty target",
  "checkpoint: scrub max time (msecs)",
  "checkpoint: scrub min time (msecs)",
  "checkpoint: scrub most recent time (msecs)",
  "checkpoint: scrub total time (msecs)",
  "checkpoint: stop timing stress active",
  "checkpoint: time spent on per-tree checkpoint work (usecs)",
  "checkpoint: total failed number of checkpoints",
  "checkpoint: total succeed number of checkpoints",
  "checkpoint: total time (msecs)",
  "checkpoint: transaction checkpoints due to obsolete pages",
  "checkpoint: wait cycles while cache dirty level is decreasing",
  "chunk-cache: aggregate number of spanned chunks on read",
  "chunk-cache: chunks evicted",
  "chunk-cache: could not allocate due to exceeding bitmap capacity",
  "chunk-cache: could not allocate due to exceeding capacity",
  "chunk-cache: lookups",
  "chunk-cache: number of chunks loaded from flushed tables in chunk cache",
  "chunk-cache: number of metadata entries inserted",
  "chunk-cache: number of metadata entries removed",
  "chunk-cache: number of metadata inserts/deletes dropped by the worker thread",
  "chunk-cache: number of metadata inserts/deletes pushed to the worker thread",
  "chunk-cache: number of metadata inserts/deletes read by the worker thread",
  "chunk-cache: number of misses",
  "chunk-cache: number of times a read from storage failed",
  "chunk-cache: retried accessing a chunk while I/O was in progress",
  "chunk-cache: retries from a chunk cache checksum mismatch",
  "chunk-cache: timed out due to too many retries",
  "chunk-cache: total bytes read from persistent content",
  "chunk-cache: total bytes used by the cache",
  "chunk-cache: total bytes used by the cache for pinned chunks",
  "chunk-cache: total chunks held by the chunk cache",
  "chunk-cache: total number of chunks inserted on startup from persisted metadata.",
  "chunk-cache: total pinned chunks held by the chunk cache",
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
  "connection: number of sessions without a sweep for 5+ minutes",
  "connection: number of sessions without a sweep for 60+ minutes",
  "connection: pthread mutex condition wait calls",
  "connection: pthread mutex shared lock read-lock calls",
  "connection: pthread mutex shared lock write-lock calls",
  "connection: total fsync I/Os",
  "connection: total read I/Os",
  "connection: total write I/Os",
  "cursor: Total number of deleted pages skipped during tree walk",
  "cursor: Total number of entries skipped by cursor next calls",
  "cursor: Total number of entries skipped by cursor prev calls",
  "cursor: Total number of entries skipped to position the history store cursor",
  "cursor: Total number of in-memory deleted pages skipped during tree walk",
  "cursor: Total number of on-disk deleted pages skipped during tree walk",
  "cursor: Total number of times a search near has exited due to prefix config",
  "cursor: Total number of times cursor fails to temporarily release pinned page to encourage "
  "eviction of hot or large page",
  "cursor: Total number of times cursor temporarily releases pinned page to encourage eviction of "
  "hot or large page",
  "cursor: bulk cursor count",
  "cursor: cached cursor count",
  "cursor: cursor bound calls that return an error",
  "cursor: cursor bounds cleared from reset",
  "cursor: cursor bounds comparisons performed",
  "cursor: cursor bounds next called on an unpositioned cursor",
  "cursor: cursor bounds next early exit",
  "cursor: cursor bounds prev called on an unpositioned cursor",
  "cursor: cursor bounds prev early exit",
  "cursor: cursor bounds search early exit",
  "cursor: cursor bounds search near call repositioned cursor",
  "cursor: cursor bulk loaded cursor insert calls",
  "cursor: cursor cache calls that return an error",
  "cursor: cursor close calls that result in cache",
  "cursor: cursor close calls that return an error",
  "cursor: cursor compare calls that return an error",
  "cursor: cursor create calls",
  "cursor: cursor equals calls that return an error",
  "cursor: cursor get key calls that return an error",
  "cursor: cursor get value calls that return an error",
  "cursor: cursor insert calls",
  "cursor: cursor insert calls that return an error",
  "cursor: cursor insert check calls that return an error",
  "cursor: cursor insert key and value bytes",
  "cursor: cursor largest key calls that return an error",
  "cursor: cursor modify calls",
  "cursor: cursor modify calls that return an error",
  "cursor: cursor modify key and value bytes affected",
  "cursor: cursor modify value bytes modified",
  "cursor: cursor next calls",
  "cursor: cursor next calls that return an error",
  "cursor: cursor next calls that skip due to a globally visible history store tombstone",
  "cursor: cursor next calls that skip greater than 1 and fewer than 100 entries",
  "cursor: cursor next calls that skip greater than or equal to 100 entries",
  "cursor: cursor next random calls that return an error",
  "cursor: cursor operation restarted",
  "cursor: cursor prev calls",
  "cursor: cursor prev calls that return an error",
  "cursor: cursor prev calls that skip due to a globally visible history store tombstone",
  "cursor: cursor prev calls that skip greater than or equal to 100 entries",
  "cursor: cursor prev calls that skip less than 100 entries",
  "cursor: cursor reconfigure calls that return an error",
  "cursor: cursor remove calls",
  "cursor: cursor remove calls that return an error",
  "cursor: cursor remove key bytes removed",
  "cursor: cursor reopen calls that return an error",
  "cursor: cursor reserve calls",
  "cursor: cursor reserve calls that return an error",
  "cursor: cursor reset calls",
  "cursor: cursor reset calls that return an error",
  "cursor: cursor search calls",
  "cursor: cursor search calls that return an error",
  "cursor: cursor search history store calls",
  "cursor: cursor search near calls",
  "cursor: cursor search near calls that return an error",
  "cursor: cursor sweep buckets",
  "cursor: cursor sweep cursors closed",
  "cursor: cursor sweep cursors examined",
  "cursor: cursor sweeps",
  "cursor: cursor truncate calls",
  "cursor: cursor truncates performed on individual keys",
  "cursor: cursor update calls",
  "cursor: cursor update calls that return an error",
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
  "lock: metadata lock acquisitions",
  "lock: metadata lock application thread wait time (usecs)",
  "lock: metadata lock internal thread wait time (usecs)",
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
  "log: force log remove time sleeping (usecs)",
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
  "perf: file system read latency histogram (bucket 1) - 0-10ms",
  "perf: file system read latency histogram (bucket 2) - 10-49ms",
  "perf: file system read latency histogram (bucket 3) - 50-99ms",
  "perf: file system read latency histogram (bucket 4) - 100-249ms",
  "perf: file system read latency histogram (bucket 5) - 250-499ms",
  "perf: file system read latency histogram (bucket 6) - 500-999ms",
  "perf: file system read latency histogram (bucket 7) - 1000ms+",
  "perf: file system read latency histogram total (msecs)",
  "perf: file system write latency histogram (bucket 1) - 0-10ms",
  "perf: file system write latency histogram (bucket 2) - 10-49ms",
  "perf: file system write latency histogram (bucket 3) - 50-99ms",
  "perf: file system write latency histogram (bucket 4) - 100-249ms",
  "perf: file system write latency histogram (bucket 5) - 250-499ms",
  "perf: file system write latency histogram (bucket 6) - 500-999ms",
  "perf: file system write latency histogram (bucket 7) - 1000ms+",
  "perf: file system write latency histogram total (msecs)",
  "perf: operation read latency histogram (bucket 1) - 0-100us",
  "perf: operation read latency histogram (bucket 2) - 100-249us",
  "perf: operation read latency histogram (bucket 3) - 250-499us",
  "perf: operation read latency histogram (bucket 4) - 500-999us",
  "perf: operation read latency histogram (bucket 5) - 1000-9999us",
  "perf: operation read latency histogram (bucket 6) - 10000us+",
  "perf: operation read latency histogram total (usecs)",
  "perf: operation write latency histogram (bucket 1) - 0-100us",
  "perf: operation write latency histogram (bucket 2) - 100-249us",
  "perf: operation write latency histogram (bucket 3) - 250-499us",
  "perf: operation write latency histogram (bucket 4) - 500-999us",
  "perf: operation write latency histogram (bucket 5) - 1000-9999us",
  "perf: operation write latency histogram (bucket 6) - 10000us+",
  "perf: operation write latency histogram total (usecs)",
  "prefetch: could not perform pre-fetch on internal page",
  "prefetch: could not perform pre-fetch on ref without the pre-fetch flag set",
  "prefetch: number of times pre-fetch failed to start",
  "prefetch: pre-fetch not repeating for recently pre-fetched ref",
  "prefetch: pre-fetch not triggered after single disk read",
  "prefetch: pre-fetch not triggered as there is no valid dhandle",
  "prefetch: pre-fetch not triggered by page read",
  "prefetch: pre-fetch not triggered due to disk read count",
  "prefetch: pre-fetch not triggered due to internal session",
  "prefetch: pre-fetch not triggered due to special btree handle",
  "prefetch: pre-fetch page not on disk when reading",
  "prefetch: pre-fetch pages queued",
  "prefetch: pre-fetch pages read in background",
  "prefetch: pre-fetch skipped reading in a page due to harmless error",
  "prefetch: pre-fetch triggered by page read",
  "reconciliation: VLCS pages explicitly reconciled as empty",
  "reconciliation: approximate byte size of timestamps in pages written",
  "reconciliation: approximate byte size of transaction IDs in pages written",
  "reconciliation: cursor next/prev calls during HS wrapup search_near",
  "reconciliation: fast-path pages deleted",
  "reconciliation: leaf-page overflow keys",
  "reconciliation: maximum milliseconds spent in a reconciliation call",
  "reconciliation: maximum milliseconds spent in building a disk image in a reconciliation",
  "reconciliation: maximum milliseconds spent in moving updates to the history store in a "
  "reconciliation",
  "reconciliation: overflow values written",
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
  "session: flush_tier failed calls",
  "session: flush_tier operation calls",
  "session: flush_tier tables skipped due to no checkpoint",
  "session: flush_tier tables switched",
  "session: local objects removed",
  "session: open session count",
  "session: session query timestamp calls",
  "session: table alter failed calls",
  "session: table alter successful calls",
  "session: table alter triggering checkpoint calls",
  "session: table alter unchanged and skipped",
  "session: table compact conflicted with checkpoint",
  "session: table compact dhandle successful calls",
  "session: table compact failed calls",
  "session: table compact failed calls due to cache pressure",
  "session: table compact passes",
  "session: table compact pulled into eviction",
  "session: table compact running",
  "session: table compact skipped as process would not reduce file size",
  "session: table compact successful calls",
  "session: table compact timeout",
  "session: table create failed calls",
  "session: table create successful calls",
  "session: table create with import failed calls",
  "session: table create with import repair calls",
  "session: table create with import successful calls",
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
  "session: tiered operations removed without processing",
  "session: tiered operations scheduled",
  "session: tiered storage local retention time (secs)",
  "thread-state: active filesystem fsync calls",
  "thread-state: active filesystem read calls",
  "thread-state: active filesystem write calls",
  "thread-yield: application thread operations waiting for cache",
  "thread-yield: application thread snapshot refreshed for eviction",
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
  "thread-yield: page split and restart read",
  "thread-yield: pages skipped during read due to deleted state",
  "transaction: Number of prepared updates",
  "transaction: Number of prepared updates committed",
  "transaction: Number of prepared updates repeated on the same key",
  "transaction: Number of prepared updates rolled back",
  "transaction: a reader raced with a prepared transaction commit and skipped an update or updates",
  "transaction: number of times overflow removed value is read",
  "transaction: oldest pinned transaction ID rolled back for eviction",
  "transaction: oldest transaction ID rolled back for eviction",
  "transaction: prepared transactions",
  "transaction: prepared transactions committed",
  "transaction: prepared transactions currently active",
  "transaction: prepared transactions rolled back",
  "transaction: query timestamp calls",
  "transaction: race to read prepared update retry",
  "transaction: rollback to stable calls",
  "transaction: rollback to stable history store keys that would have been swept in non-dryrun "
  "mode",
  "transaction: rollback to stable history store records with stop timestamps older than newer "
  "records",
  "transaction: rollback to stable inconsistent checkpoint",
  "transaction: rollback to stable keys removed",
  "transaction: rollback to stable keys restored",
  "transaction: rollback to stable keys that would have been removed in non-dryrun mode",
  "transaction: rollback to stable keys that would have been restored in non-dryrun mode",
  "transaction: rollback to stable pages visited",
  "transaction: rollback to stable restored tombstones from history store",
  "transaction: rollback to stable restored updates from history store",
  "transaction: rollback to stable skipping delete rle",
  "transaction: rollback to stable skipping stable rle",
  "transaction: rollback to stable sweeping history store keys",
  "transaction: rollback to stable tombstones from history store that would have been restored in "
  "non-dryrun mode",
  "transaction: rollback to stable tree walk skipping pages",
  "transaction: rollback to stable updates aborted",
  "transaction: rollback to stable updates from history store that would have been restored in "
  "non-dryrun mode",
  "transaction: rollback to stable updates removed from history store",
  "transaction: rollback to stable updates that would have been aborted in non-dryrun mode",
  "transaction: rollback to stable updates that would have been removed from history store in "
  "non-dryrun mode",
  "transaction: sessions scanned in each walk of concurrent sessions",
  "transaction: set timestamp calls",
  "transaction: set timestamp durable calls",
  "transaction: set timestamp durable updates",
  "transaction: set timestamp force calls",
  "transaction: set timestamp global oldest timestamp set to be more recent than the global stable "
  "timestamp",
  "transaction: set timestamp oldest calls",
  "transaction: set timestamp oldest updates",
  "transaction: set timestamp stable calls",
  "transaction: set timestamp stable updates",
  "transaction: transaction begins",
  "transaction: transaction checkpoint history store file duration (usecs)",
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
    stats->autocommit_readonly_retry = 0;
    stats->autocommit_update_retry = 0;
    stats->background_compact_fail = 0;
    stats->background_compact_fail_cache_pressure = 0;
    stats->background_compact_interrupted = 0;
    stats->background_compact_ema = 0;
    stats->background_compact_bytes_recovered = 0;
    stats->background_compact_running = 0;
    stats->background_compact_exclude = 0;
    stats->background_compact_skipped = 0;
    stats->background_compact_sleep_cache_pressure = 0;
    stats->background_compact_success = 0;
    stats->background_compact_timeout = 0;
    stats->background_compact_files_tracked = 0;
    /* not clearing backup_cursor_open */
    /* not clearing backup_dup_open */
    stats->backup_granularity = 0;
    /* not clearing backup_incremental */
    /* not clearing backup_start */
    stats->backup_blocks = 0;
    stats->backup_blocks_compressed = 0;
    stats->backup_blocks_uncompressed = 0;
    stats->block_cache_blocks_update = 0;
    stats->block_cache_bytes_update = 0;
    stats->block_cache_blocks_evicted = 0;
    stats->block_cache_bypass_filesize = 0;
    stats->block_cache_lookups = 0;
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
    stats->block_cache_blocks_removed_blocked = 0;
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
    stats->block_byte_write_compact = 0;
    stats->block_byte_write_checkpoint = 0;
    stats->block_byte_write_mmap = 0;
    stats->block_byte_write_syscall = 0;
    stats->block_map_read = 0;
    stats->block_byte_map_read = 0;
    stats->block_remap_file_resize = 0;
    stats->block_remap_file_write = 0;
    stats->cache_eviction_app_time = 0;
    stats->cache_eviction_app_threads_fill_ratio_lt_25 = 0;
    stats->cache_eviction_app_threads_fill_ratio_25_50 = 0;
    stats->cache_eviction_app_threads_fill_ratio_50_75 = 0;
    stats->cache_eviction_app_threads_fill_ratio_gt_75 = 0;
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
    stats->cache_eviction_blocked_checkpoint = 0;
    stats->cache_eviction_blocked_checkpoint_hs = 0;
    stats->eviction_server_evict_attempt = 0;
    stats->eviction_worker_evict_attempt = 0;
    stats->eviction_server_evict_fail = 0;
    stats->eviction_worker_evict_fail = 0;
    stats->cache_eviction_get_ref = 0;
    stats->cache_eviction_get_ref_empty = 0;
    stats->cache_eviction_get_ref_empty2 = 0;
    /* not clearing cache_eviction_aggressive_set */
    /* not clearing cache_eviction_empty_score */
    stats->cache_eviction_blocked_no_ts_checkpoint_race_1 = 0;
    stats->cache_eviction_blocked_no_ts_checkpoint_race_2 = 0;
    stats->cache_eviction_blocked_no_ts_checkpoint_race_3 = 0;
    stats->cache_eviction_blocked_no_ts_checkpoint_race_4 = 0;
    stats->cache_eviction_blocked_remove_hs_race_with_checkpoint = 0;
    stats->cache_eviction_blocked_no_progress = 0;
    stats->cache_eviction_walk_passes = 0;
    stats->cache_eviction_queue_empty = 0;
    stats->cache_eviction_queue_not_empty = 0;
    stats->cache_eviction_server_skip_dirty_pages_during_checkpoint = 0;
    stats->cache_eviction_server_skip_intl_page_with_active_child = 0;
    stats->cache_eviction_server_skip_metatdata_with_history = 0;
    stats->cache_eviction_server_skip_pages_last_running = 0;
    stats->cache_eviction_server_skip_pages_retry = 0;
    stats->cache_eviction_server_skip_unwanted_pages = 0;
    stats->cache_eviction_server_skip_unwanted_tree = 0;
    stats->cache_eviction_server_skip_trees_too_many_active_walks = 0;
    stats->cache_eviction_server_skip_checkpointing_trees = 0;
    stats->cache_eviction_server_skip_trees_stick_in_cache = 0;
    stats->cache_eviction_server_skip_trees_eviction_disabled = 0;
    stats->cache_eviction_server_skip_trees_not_useful_before = 0;
    stats->cache_eviction_server_slept = 0;
    stats->cache_eviction_slow = 0;
    stats->cache_eviction_walk_leaf_notfound = 0;
    /* not clearing cache_eviction_state */
    stats->cache_eviction_walk_sleeps = 0;
    stats->cache_eviction_pages_queued_updates = 0;
    stats->cache_eviction_pages_queued_clean = 0;
    stats->cache_eviction_pages_queued_dirty = 0;
    stats->cache_eviction_pages_seen_updates = 0;
    stats->cache_eviction_pages_seen_clean = 0;
    stats->cache_eviction_pages_seen_dirty = 0;
    stats->cache_eviction_target_page_lt10 = 0;
    stats->cache_eviction_target_page_lt32 = 0;
    stats->cache_eviction_target_page_ge128 = 0;
    stats->cache_eviction_target_page_lt64 = 0;
    stats->cache_eviction_target_page_lt128 = 0;
    stats->cache_eviction_target_page_reduced = 0;
    stats->cache_eviction_target_strategy_clean = 0;
    stats->cache_eviction_target_strategy_dirty = 0;
    stats->cache_eviction_target_strategy_updates = 0;
    stats->cache_eviction_walks_abandoned = 0;
    stats->cache_eviction_walks_stopped = 0;
    stats->cache_eviction_walks_gave_up_no_targets = 0;
    stats->cache_eviction_walks_gave_up_ratio = 0;
    stats->cache_eviction_walk_random_returns_null_position = 0;
    stats->cache_eviction_walks_ended = 0;
    stats->cache_eviction_walk_restart = 0;
    stats->cache_eviction_walk_from_root = 0;
    stats->cache_eviction_walk_saved_pos = 0;
    /* not clearing cache_eviction_active_workers */
    stats->cache_eviction_worker_created = 0;
    stats->cache_eviction_worker_removed = 0;
    /* not clearing cache_eviction_stable_state_workers */
    /* not clearing cache_eviction_walks_active */
    stats->cache_eviction_walks_started = 0;
    stats->cache_eviction_force_retune = 0;
    stats->cache_eviction_force_no_retry = 0;
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
    stats->cache_eviction_blocked_hazard = 0;
    stats->cache_hazard_checks = 0;
    stats->cache_hazard_walks = 0;
    stats->cache_hazard_max = 0;
    stats->cache_hs_insert = 0;
    stats->cache_hs_insert_restart = 0;
    /* not clearing cache_hs_ondisk_max */
    /* not clearing cache_hs_ondisk */
    stats->cache_hs_read = 0;
    stats->cache_hs_read_miss = 0;
    stats->cache_hs_read_squash = 0;
    stats->cache_hs_order_lose_durable_timestamp = 0;
    stats->cache_hs_key_truncate_rts_unstable = 0;
    stats->cache_hs_key_truncate_rts = 0;
    stats->cache_hs_btree_truncate = 0;
    stats->cache_hs_key_truncate = 0;
    stats->cache_hs_order_remove = 0;
    stats->cache_hs_key_truncate_onpage_removal = 0;
    stats->cache_hs_btree_truncate_dryrun = 0;
    stats->cache_hs_key_truncate_rts_unstable_dryrun = 0;
    stats->cache_hs_key_truncate_rts_dryrun = 0;
    stats->cache_hs_order_reinsert = 0;
    stats->cache_hs_write_squash = 0;
    stats->cache_inmem_splittable = 0;
    stats->cache_inmem_split = 0;
    stats->cache_eviction_blocked_internal_page_split = 0;
    stats->cache_eviction_internal = 0;
    stats->cache_eviction_internal_pages_queued = 0;
    stats->cache_eviction_internal_pages_seen = 0;
    stats->cache_eviction_internal_pages_already_queued = 0;
    stats->cache_eviction_split_internal = 0;
    stats->cache_eviction_split_leaf = 0;
    stats->cache_eviction_random_sample_inmem_root = 0;
    /* not clearing cache_bytes_max */
    /* not clearing cache_eviction_maximum_gen_gap */
    /* not clearing cache_eviction_maximum_milliseconds */
    /* not clearing cache_eviction_maximum_page_size */
    stats->eviction_app_dirty_attempt = 0;
    stats->eviction_app_dirty_fail = 0;
    stats->cache_eviction_dirty = 0;
    stats->cache_eviction_blocked_multi_block_reconcilation_during_checkpoint = 0;
    stats->cache_eviction_trigger_dirty_reached = 0;
    stats->cache_eviction_trigger_reached = 0;
    stats->cache_eviction_trigger_updates_reached = 0;
    stats->cache_timed_out_ops = 0;
    stats->cache_eviction_blocked_overflow_keys = 0;
    stats->cache_read_overflow = 0;
    stats->eviction_app_attempt = 0;
    stats->eviction_app_fail = 0;
    stats->cache_eviction_deepen = 0;
    stats->cache_write_hs = 0;
    /* not clearing cache_eviction_consider_prefetch */
    /* not clearing cache_pages_inuse */
    stats->cache_eviction_dirty_obsolete_tw = 0;
    stats->cache_eviction_pages_in_parallel_with_checkpoint = 0;
    stats->cache_eviction_pages_queued = 0;
    stats->cache_eviction_pages_queued_post_lru = 0;
    stats->cache_eviction_pages_queued_urgent = 0;
    stats->cache_eviction_pages_queued_oldest = 0;
    stats->cache_eviction_pages_queued_urgent_hs_dirty = 0;
    stats->cache_read = 0;
    stats->cache_read_deleted = 0;
    stats->cache_read_deleted_prepared = 0;
    stats->cache_read_checkpoint = 0;
    stats->cache_eviction_clear_ordinary = 0;
    stats->cache_pages_requested = 0;
    stats->cache_pages_prefetch = 0;
    stats->cache_eviction_pages_seen = 0;
    stats->cache_eviction_pages_already_queued = 0;
    stats->cache_eviction_fail = 0;
    stats->cache_eviction_fail_active_children_on_an_internal_page = 0;
    stats->cache_eviction_fail_in_reconciliation = 0;
    stats->cache_eviction_fail_checkpoint_no_ts = 0;
    stats->cache_eviction_walk = 0;
    stats->cache_write = 0;
    stats->cache_write_restore = 0;
    /* not clearing cache_overhead */
    stats->cache_eviction_blocked_recently_modified = 0;
    stats->cache_reverse_splits = 0;
    stats->cache_reverse_splits_skipped_vlcs = 0;
    stats->cache_hs_insert_full_update = 0;
    stats->cache_hs_insert_reverse_modify = 0;
    /* not clearing cache_reentry_hs_eviction_milliseconds */
    /* not clearing cache_bytes_internal */
    /* not clearing cache_bytes_leaf */
    /* not clearing cache_bytes_dirty */
    /* not clearing cache_bytes_dirty_internal */
    /* not clearing cache_bytes_dirty_leaf */
    /* not clearing cache_pages_dirty */
    stats->cache_eviction_blocked_uncommitted_truncate = 0;
    stats->cache_eviction_clean = 0;
    stats->fsync_all_fh_total = 0;
    stats->fsync_all_fh = 0;
    /* not clearing fsync_all_time */
    stats->capacity_bytes_read = 0;
    stats->capacity_bytes_ckpt = 0;
    stats->capacity_bytes_chunkcache = 0;
    stats->capacity_bytes_evict = 0;
    stats->capacity_bytes_log = 0;
    stats->capacity_bytes_written = 0;
    stats->capacity_threshold = 0;
    stats->capacity_time_total = 0;
    stats->capacity_time_ckpt = 0;
    stats->capacity_time_evict = 0;
    stats->capacity_time_log = 0;
    stats->capacity_time_read = 0;
    stats->capacity_time_chunkcache = 0;
    stats->checkpoint_cleanup_success = 0;
    stats->checkpoint_snapshot_acquired = 0;
    stats->checkpoint_skipped = 0;
    stats->checkpoint_fsync_post = 0;
    /* not clearing checkpoint_fsync_post_duration */
    /* not clearing checkpoint_generation */
    /* not clearing checkpoint_time_max */
    /* not clearing checkpoint_time_min */
    /* not clearing checkpoint_handle_drop_duration */
    /* not clearing checkpoint_handle_duration */
    /* not clearing checkpoint_handle_apply_duration */
    /* not clearing checkpoint_handle_skip_duration */
    /* not clearing checkpoint_handle_meta_check_duration */
    /* not clearing checkpoint_handle_lock_duration */
    stats->checkpoint_handle_applied = 0;
    stats->checkpoint_handle_dropped = 0;
    stats->checkpoint_handle_meta_checked = 0;
    stats->checkpoint_handle_locked = 0;
    stats->checkpoint_handle_skipped = 0;
    stats->checkpoint_handle_walked = 0;
    /* not clearing checkpoint_time_recent */
    stats->checkpoints_api = 0;
    stats->checkpoints_compact = 0;
    stats->checkpoint_sync = 0;
    stats->checkpoint_presync = 0;
    stats->checkpoint_hs_pages_reconciled = 0;
    stats->checkpoint_pages_visited_internal = 0;
    stats->checkpoint_pages_visited_leaf = 0;
    stats->checkpoint_pages_reconciled = 0;
    stats->checkpoint_cleanup_pages_evict = 0;
    stats->checkpoint_cleanup_pages_obsolete_tw = 0;
    stats->checkpoint_cleanup_pages_read_reclaim_space = 0;
    stats->checkpoint_cleanup_pages_read_obsolete_tw = 0;
    stats->checkpoint_cleanup_pages_removed = 0;
    stats->checkpoint_cleanup_pages_walk_skipped = 0;
    stats->checkpoint_cleanup_pages_visited = 0;
    /* not clearing checkpoint_prep_running */
    /* not clearing checkpoint_prep_max */
    /* not clearing checkpoint_prep_min */
    /* not clearing checkpoint_prep_recent */
    /* not clearing checkpoint_prep_total */
    /* not clearing checkpoint_state */
    /* not clearing checkpoint_scrub_target */
    /* not clearing checkpoint_scrub_max */
    /* not clearing checkpoint_scrub_min */
    /* not clearing checkpoint_scrub_recent */
    /* not clearing checkpoint_scrub_total */
    /* not clearing checkpoint_stop_stress_active */
    stats->checkpoint_tree_duration = 0;
    stats->checkpoints_total_failed = 0;
    stats->checkpoints_total_succeed = 0;
    /* not clearing checkpoint_time_total */
    stats->checkpoint_obsolete_applied = 0;
    stats->checkpoint_wait_reduce_dirty = 0;
    stats->chunkcache_spans_chunks_read = 0;
    stats->chunkcache_chunks_evicted = 0;
    stats->chunkcache_exceeded_bitmap_capacity = 0;
    stats->chunkcache_exceeded_capacity = 0;
    stats->chunkcache_lookups = 0;
    stats->chunkcache_chunks_loaded_from_flushed_tables = 0;
    stats->chunkcache_metadata_inserted = 0;
    stats->chunkcache_metadata_removed = 0;
    stats->chunkcache_metadata_work_units_dropped = 0;
    stats->chunkcache_metadata_work_units_created = 0;
    stats->chunkcache_metadata_work_units_dequeued = 0;
    stats->chunkcache_misses = 0;
    stats->chunkcache_io_failed = 0;
    stats->chunkcache_retries = 0;
    stats->chunkcache_retries_checksum_mismatch = 0;
    stats->chunkcache_toomany_retries = 0;
    stats->chunkcache_bytes_read_persistent = 0;
    stats->chunkcache_bytes_inuse = 0;
    stats->chunkcache_bytes_inuse_pinned = 0;
    stats->chunkcache_chunks_inuse = 0;
    stats->chunkcache_created_from_metadata = 0;
    stats->chunkcache_chunks_pinned = 0;
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
    stats->no_session_sweep_5min = 0;
    stats->no_session_sweep_60min = 0;
    stats->cond_wait = 0;
    stats->rwlock_read = 0;
    stats->rwlock_write = 0;
    stats->fsync_io = 0;
    stats->read_io = 0;
    stats->write_io = 0;
    stats->cursor_tree_walk_del_page_skip = 0;
    stats->cursor_next_skip_total = 0;
    stats->cursor_prev_skip_total = 0;
    stats->cursor_skip_hs_cur_position = 0;
    stats->cursor_tree_walk_inmem_del_page_skip = 0;
    stats->cursor_tree_walk_ondisk_del_page_skip = 0;
    stats->cursor_search_near_prefix_fast_paths = 0;
    stats->cursor_reposition_failed = 0;
    stats->cursor_reposition = 0;
    /* not clearing cursor_bulk_count */
    /* not clearing cursor_cached_count */
    stats->cursor_bound_error = 0;
    stats->cursor_bounds_reset = 0;
    stats->cursor_bounds_comparisons = 0;
    stats->cursor_bounds_next_unpositioned = 0;
    stats->cursor_bounds_next_early_exit = 0;
    stats->cursor_bounds_prev_unpositioned = 0;
    stats->cursor_bounds_prev_early_exit = 0;
    stats->cursor_bounds_search_early_exit = 0;
    stats->cursor_bounds_search_near_repositioned_cursor = 0;
    stats->cursor_insert_bulk = 0;
    stats->cursor_cache_error = 0;
    stats->cursor_cache = 0;
    stats->cursor_close_error = 0;
    stats->cursor_compare_error = 0;
    stats->cursor_create = 0;
    stats->cursor_equals_error = 0;
    stats->cursor_get_key_error = 0;
    stats->cursor_get_value_error = 0;
    stats->cursor_insert = 0;
    stats->cursor_insert_error = 0;
    stats->cursor_insert_check_error = 0;
    stats->cursor_insert_bytes = 0;
    stats->cursor_largest_key_error = 0;
    stats->cursor_modify = 0;
    stats->cursor_modify_error = 0;
    stats->cursor_modify_bytes = 0;
    stats->cursor_modify_bytes_touch = 0;
    stats->cursor_next = 0;
    stats->cursor_next_error = 0;
    stats->cursor_next_hs_tombstone = 0;
    stats->cursor_next_skip_lt_100 = 0;
    stats->cursor_next_skip_ge_100 = 0;
    stats->cursor_next_random_error = 0;
    stats->cursor_restart = 0;
    stats->cursor_prev = 0;
    stats->cursor_prev_error = 0;
    stats->cursor_prev_hs_tombstone = 0;
    stats->cursor_prev_skip_ge_100 = 0;
    stats->cursor_prev_skip_lt_100 = 0;
    stats->cursor_reconfigure_error = 0;
    stats->cursor_remove = 0;
    stats->cursor_remove_error = 0;
    stats->cursor_remove_bytes = 0;
    stats->cursor_reopen_error = 0;
    stats->cursor_reserve = 0;
    stats->cursor_reserve_error = 0;
    stats->cursor_reset = 0;
    stats->cursor_reset_error = 0;
    stats->cursor_search = 0;
    stats->cursor_search_error = 0;
    stats->cursor_search_hs = 0;
    stats->cursor_search_near = 0;
    stats->cursor_search_near_error = 0;
    stats->cursor_sweep_buckets = 0;
    stats->cursor_sweep_closed = 0;
    stats->cursor_sweep_examined = 0;
    stats->cursor_sweep = 0;
    stats->cursor_truncate = 0;
    stats->cursor_truncate_keys_deleted = 0;
    stats->cursor_update = 0;
    stats->cursor_update_error = 0;
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
    stats->lock_metadata_count = 0;
    stats->lock_metadata_wait_application = 0;
    stats->lock_metadata_wait_internal = 0;
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
    stats->log_force_remove_sleep = 0;
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
    stats->perf_hist_fsread_latency_lt10 = 0;
    stats->perf_hist_fsread_latency_lt50 = 0;
    stats->perf_hist_fsread_latency_lt100 = 0;
    stats->perf_hist_fsread_latency_lt250 = 0;
    stats->perf_hist_fsread_latency_lt500 = 0;
    stats->perf_hist_fsread_latency_lt1000 = 0;
    stats->perf_hist_fsread_latency_gt1000 = 0;
    stats->perf_hist_fsread_latency_total_msecs = 0;
    stats->perf_hist_fswrite_latency_lt10 = 0;
    stats->perf_hist_fswrite_latency_lt50 = 0;
    stats->perf_hist_fswrite_latency_lt100 = 0;
    stats->perf_hist_fswrite_latency_lt250 = 0;
    stats->perf_hist_fswrite_latency_lt500 = 0;
    stats->perf_hist_fswrite_latency_lt1000 = 0;
    stats->perf_hist_fswrite_latency_gt1000 = 0;
    stats->perf_hist_fswrite_latency_total_msecs = 0;
    stats->perf_hist_opread_latency_lt100 = 0;
    stats->perf_hist_opread_latency_lt250 = 0;
    stats->perf_hist_opread_latency_lt500 = 0;
    stats->perf_hist_opread_latency_lt1000 = 0;
    stats->perf_hist_opread_latency_lt10000 = 0;
    stats->perf_hist_opread_latency_gt10000 = 0;
    stats->perf_hist_opread_latency_total_usecs = 0;
    stats->perf_hist_opwrite_latency_lt100 = 0;
    stats->perf_hist_opwrite_latency_lt250 = 0;
    stats->perf_hist_opwrite_latency_lt500 = 0;
    stats->perf_hist_opwrite_latency_lt1000 = 0;
    stats->perf_hist_opwrite_latency_lt10000 = 0;
    stats->perf_hist_opwrite_latency_gt10000 = 0;
    stats->perf_hist_opwrite_latency_total_usecs = 0;
    stats->prefetch_skipped_internal_page = 0;
    stats->prefetch_skipped_no_flag_set = 0;
    stats->prefetch_failed_start = 0;
    stats->prefetch_skipped_same_ref = 0;
    stats->prefetch_disk_one = 0;
    stats->prefetch_skipped_no_valid_dhandle = 0;
    stats->prefetch_skipped = 0;
    stats->prefetch_skipped_disk_read_count = 0;
    stats->prefetch_skipped_internal_session = 0;
    stats->prefetch_skipped_special_handle = 0;
    stats->prefetch_pages_fail = 0;
    stats->prefetch_pages_queued = 0;
    stats->prefetch_pages_read = 0;
    stats->prefetch_skipped_error_ok = 0;
    stats->prefetch_attempts = 0;
    stats->rec_vlcs_emptied_pages = 0;
    stats->rec_time_window_bytes_ts = 0;
    stats->rec_time_window_bytes_txn = 0;
    stats->rec_hs_wrapup_next_prev_calls = 0;
    stats->rec_page_delete_fast = 0;
    stats->rec_overflow_key_leaf = 0;
    /* not clearing rec_maximum_milliseconds */
    /* not clearing rec_maximum_image_build_milliseconds */
    /* not clearing rec_maximum_hs_wrapup_milliseconds */
    stats->rec_overflow_value = 0;
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
    stats->flush_tier_fail = 0;
    stats->flush_tier = 0;
    stats->flush_tier_skipped = 0;
    stats->flush_tier_switched = 0;
    stats->local_objects_removed = 0;
    /* not clearing session_open */
    stats->session_query_ts = 0;
    /* not clearing session_table_alter_fail */
    /* not clearing session_table_alter_success */
    /* not clearing session_table_alter_trigger_checkpoint */
    /* not clearing session_table_alter_skip */
    /* not clearing session_table_compact_conflicting_checkpoint */
    stats->session_table_compact_dhandle_success = 0;
    /* not clearing session_table_compact_fail */
    /* not clearing session_table_compact_fail_cache_pressure */
    stats->session_table_compact_passes = 0;
    /* not clearing session_table_compact_eviction */
    /* not clearing session_table_compact_running */
    /* not clearing session_table_compact_skipped */
    /* not clearing session_table_compact_success */
    /* not clearing session_table_compact_timeout */
    /* not clearing session_table_create_fail */
    /* not clearing session_table_create_success */
    /* not clearing session_table_create_import_fail */
    /* not clearing session_table_create_import_repair */
    /* not clearing session_table_create_import_success */
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
    stats->tiered_work_units_removed = 0;
    stats->tiered_work_units_created = 0;
    /* not clearing tiered_retention */
    /* not clearing thread_fsync_active */
    /* not clearing thread_read_active */
    /* not clearing thread_write_active */
    stats->application_cache_ops = 0;
    stats->application_evict_snapshot_refreshed = 0;
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
    stats->page_split_restart = 0;
    stats->page_read_skip_deleted = 0;
    stats->txn_prepared_updates = 0;
    stats->txn_prepared_updates_committed = 0;
    stats->txn_prepared_updates_key_repeated = 0;
    stats->txn_prepared_updates_rolledback = 0;
    stats->txn_read_race_prepare_commit = 0;
    stats->txn_read_overflow_remove = 0;
    stats->txn_rollback_oldest_pinned = 0;
    stats->txn_rollback_oldest_id = 0;
    stats->txn_prepare = 0;
    stats->txn_prepare_commit = 0;
    stats->txn_prepare_active = 0;
    stats->txn_prepare_rollback = 0;
    stats->txn_query_ts = 0;
    stats->txn_read_race_prepare_update = 0;
    stats->txn_rts = 0;
    stats->txn_rts_sweep_hs_keys_dryrun = 0;
    stats->txn_rts_hs_stop_older_than_newer_start = 0;
    stats->txn_rts_inconsistent_ckpt = 0;
    stats->txn_rts_keys_removed = 0;
    stats->txn_rts_keys_restored = 0;
    stats->txn_rts_keys_removed_dryrun = 0;
    stats->txn_rts_keys_restored_dryrun = 0;
    stats->txn_rts_pages_visited = 0;
    stats->txn_rts_hs_restore_tombstones = 0;
    stats->txn_rts_hs_restore_updates = 0;
    stats->txn_rts_delete_rle_skipped = 0;
    stats->txn_rts_stable_rle_skipped = 0;
    stats->txn_rts_sweep_hs_keys = 0;
    stats->txn_rts_hs_restore_tombstones_dryrun = 0;
    stats->txn_rts_tree_walk_skip_pages = 0;
    stats->txn_rts_upd_aborted = 0;
    stats->txn_rts_hs_restore_updates_dryrun = 0;
    stats->txn_rts_hs_removed = 0;
    stats->txn_rts_upd_aborted_dryrun = 0;
    stats->txn_rts_hs_removed_dryrun = 0;
    stats->txn_sessions_walked = 0;
    stats->txn_set_ts = 0;
    stats->txn_set_ts_durable = 0;
    stats->txn_set_ts_durable_upd = 0;
    stats->txn_set_ts_force = 0;
    stats->txn_set_ts_out_of_order = 0;
    stats->txn_set_ts_oldest = 0;
    stats->txn_set_ts_oldest_upd = 0;
    stats->txn_set_ts_stable = 0;
    stats->txn_set_ts_stable_upd = 0;
    stats->txn_begin = 0;
    stats->txn_hs_ckpt_duration = 0;
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
    to->autocommit_readonly_retry += WT_STAT_READ(from, autocommit_readonly_retry);
    to->autocommit_update_retry += WT_STAT_READ(from, autocommit_update_retry);
    to->background_compact_fail += WT_STAT_READ(from, background_compact_fail);
    to->background_compact_fail_cache_pressure +=
      WT_STAT_READ(from, background_compact_fail_cache_pressure);
    to->background_compact_interrupted += WT_STAT_READ(from, background_compact_interrupted);
    to->background_compact_ema += WT_STAT_READ(from, background_compact_ema);
    to->background_compact_bytes_recovered +=
      WT_STAT_READ(from, background_compact_bytes_recovered);
    to->background_compact_running += WT_STAT_READ(from, background_compact_running);
    to->background_compact_exclude += WT_STAT_READ(from, background_compact_exclude);
    to->background_compact_skipped += WT_STAT_READ(from, background_compact_skipped);
    to->background_compact_sleep_cache_pressure +=
      WT_STAT_READ(from, background_compact_sleep_cache_pressure);
    to->background_compact_success += WT_STAT_READ(from, background_compact_success);
    to->background_compact_timeout += WT_STAT_READ(from, background_compact_timeout);
    to->background_compact_files_tracked += WT_STAT_READ(from, background_compact_files_tracked);
    to->backup_cursor_open += WT_STAT_READ(from, backup_cursor_open);
    to->backup_dup_open += WT_STAT_READ(from, backup_dup_open);
    to->backup_granularity += WT_STAT_READ(from, backup_granularity);
    to->backup_incremental += WT_STAT_READ(from, backup_incremental);
    to->backup_start += WT_STAT_READ(from, backup_start);
    to->backup_blocks += WT_STAT_READ(from, backup_blocks);
    to->backup_blocks_compressed += WT_STAT_READ(from, backup_blocks_compressed);
    to->backup_blocks_uncompressed += WT_STAT_READ(from, backup_blocks_uncompressed);
    to->block_cache_blocks_update += WT_STAT_READ(from, block_cache_blocks_update);
    to->block_cache_bytes_update += WT_STAT_READ(from, block_cache_bytes_update);
    to->block_cache_blocks_evicted += WT_STAT_READ(from, block_cache_blocks_evicted);
    to->block_cache_bypass_filesize += WT_STAT_READ(from, block_cache_bypass_filesize);
    to->block_cache_lookups += WT_STAT_READ(from, block_cache_lookups);
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
    to->block_cache_blocks_removed_blocked +=
      WT_STAT_READ(from, block_cache_blocks_removed_blocked);
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
    to->block_byte_write_compact += WT_STAT_READ(from, block_byte_write_compact);
    to->block_byte_write_checkpoint += WT_STAT_READ(from, block_byte_write_checkpoint);
    to->block_byte_write_mmap += WT_STAT_READ(from, block_byte_write_mmap);
    to->block_byte_write_syscall += WT_STAT_READ(from, block_byte_write_syscall);
    to->block_map_read += WT_STAT_READ(from, block_map_read);
    to->block_byte_map_read += WT_STAT_READ(from, block_byte_map_read);
    to->block_remap_file_resize += WT_STAT_READ(from, block_remap_file_resize);
    to->block_remap_file_write += WT_STAT_READ(from, block_remap_file_write);
    to->cache_eviction_app_time += WT_STAT_READ(from, cache_eviction_app_time);
    to->cache_eviction_app_threads_fill_ratio_lt_25 +=
      WT_STAT_READ(from, cache_eviction_app_threads_fill_ratio_lt_25);
    to->cache_eviction_app_threads_fill_ratio_25_50 +=
      WT_STAT_READ(from, cache_eviction_app_threads_fill_ratio_25_50);
    to->cache_eviction_app_threads_fill_ratio_50_75 +=
      WT_STAT_READ(from, cache_eviction_app_threads_fill_ratio_50_75);
    to->cache_eviction_app_threads_fill_ratio_gt_75 +=
      WT_STAT_READ(from, cache_eviction_app_threads_fill_ratio_gt_75);
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
    to->cache_eviction_blocked_checkpoint += WT_STAT_READ(from, cache_eviction_blocked_checkpoint);
    to->cache_eviction_blocked_checkpoint_hs +=
      WT_STAT_READ(from, cache_eviction_blocked_checkpoint_hs);
    to->eviction_server_evict_attempt += WT_STAT_READ(from, eviction_server_evict_attempt);
    to->eviction_worker_evict_attempt += WT_STAT_READ(from, eviction_worker_evict_attempt);
    to->eviction_server_evict_fail += WT_STAT_READ(from, eviction_server_evict_fail);
    to->eviction_worker_evict_fail += WT_STAT_READ(from, eviction_worker_evict_fail);
    to->cache_eviction_get_ref += WT_STAT_READ(from, cache_eviction_get_ref);
    to->cache_eviction_get_ref_empty += WT_STAT_READ(from, cache_eviction_get_ref_empty);
    to->cache_eviction_get_ref_empty2 += WT_STAT_READ(from, cache_eviction_get_ref_empty2);
    to->cache_eviction_aggressive_set += WT_STAT_READ(from, cache_eviction_aggressive_set);
    to->cache_eviction_empty_score += WT_STAT_READ(from, cache_eviction_empty_score);
    to->cache_eviction_blocked_no_ts_checkpoint_race_1 +=
      WT_STAT_READ(from, cache_eviction_blocked_no_ts_checkpoint_race_1);
    to->cache_eviction_blocked_no_ts_checkpoint_race_2 +=
      WT_STAT_READ(from, cache_eviction_blocked_no_ts_checkpoint_race_2);
    to->cache_eviction_blocked_no_ts_checkpoint_race_3 +=
      WT_STAT_READ(from, cache_eviction_blocked_no_ts_checkpoint_race_3);
    to->cache_eviction_blocked_no_ts_checkpoint_race_4 +=
      WT_STAT_READ(from, cache_eviction_blocked_no_ts_checkpoint_race_4);
    to->cache_eviction_blocked_remove_hs_race_with_checkpoint +=
      WT_STAT_READ(from, cache_eviction_blocked_remove_hs_race_with_checkpoint);
    to->cache_eviction_blocked_no_progress +=
      WT_STAT_READ(from, cache_eviction_blocked_no_progress);
    to->cache_eviction_walk_passes += WT_STAT_READ(from, cache_eviction_walk_passes);
    to->cache_eviction_queue_empty += WT_STAT_READ(from, cache_eviction_queue_empty);
    to->cache_eviction_queue_not_empty += WT_STAT_READ(from, cache_eviction_queue_not_empty);
    to->cache_eviction_server_skip_dirty_pages_during_checkpoint +=
      WT_STAT_READ(from, cache_eviction_server_skip_dirty_pages_during_checkpoint);
    to->cache_eviction_server_skip_intl_page_with_active_child +=
      WT_STAT_READ(from, cache_eviction_server_skip_intl_page_with_active_child);
    to->cache_eviction_server_skip_metatdata_with_history +=
      WT_STAT_READ(from, cache_eviction_server_skip_metatdata_with_history);
    to->cache_eviction_server_skip_pages_last_running +=
      WT_STAT_READ(from, cache_eviction_server_skip_pages_last_running);
    to->cache_eviction_server_skip_pages_retry +=
      WT_STAT_READ(from, cache_eviction_server_skip_pages_retry);
    to->cache_eviction_server_skip_unwanted_pages +=
      WT_STAT_READ(from, cache_eviction_server_skip_unwanted_pages);
    to->cache_eviction_server_skip_unwanted_tree +=
      WT_STAT_READ(from, cache_eviction_server_skip_unwanted_tree);
    to->cache_eviction_server_skip_trees_too_many_active_walks +=
      WT_STAT_READ(from, cache_eviction_server_skip_trees_too_many_active_walks);
    to->cache_eviction_server_skip_checkpointing_trees +=
      WT_STAT_READ(from, cache_eviction_server_skip_checkpointing_trees);
    to->cache_eviction_server_skip_trees_stick_in_cache +=
      WT_STAT_READ(from, cache_eviction_server_skip_trees_stick_in_cache);
    to->cache_eviction_server_skip_trees_eviction_disabled +=
      WT_STAT_READ(from, cache_eviction_server_skip_trees_eviction_disabled);
    to->cache_eviction_server_skip_trees_not_useful_before +=
      WT_STAT_READ(from, cache_eviction_server_skip_trees_not_useful_before);
    to->cache_eviction_server_slept += WT_STAT_READ(from, cache_eviction_server_slept);
    to->cache_eviction_slow += WT_STAT_READ(from, cache_eviction_slow);
    to->cache_eviction_walk_leaf_notfound += WT_STAT_READ(from, cache_eviction_walk_leaf_notfound);
    to->cache_eviction_state += WT_STAT_READ(from, cache_eviction_state);
    to->cache_eviction_walk_sleeps += WT_STAT_READ(from, cache_eviction_walk_sleeps);
    to->cache_eviction_pages_queued_updates +=
      WT_STAT_READ(from, cache_eviction_pages_queued_updates);
    to->cache_eviction_pages_queued_clean += WT_STAT_READ(from, cache_eviction_pages_queued_clean);
    to->cache_eviction_pages_queued_dirty += WT_STAT_READ(from, cache_eviction_pages_queued_dirty);
    to->cache_eviction_pages_seen_updates += WT_STAT_READ(from, cache_eviction_pages_seen_updates);
    to->cache_eviction_pages_seen_clean += WT_STAT_READ(from, cache_eviction_pages_seen_clean);
    to->cache_eviction_pages_seen_dirty += WT_STAT_READ(from, cache_eviction_pages_seen_dirty);
    to->cache_eviction_target_page_lt10 += WT_STAT_READ(from, cache_eviction_target_page_lt10);
    to->cache_eviction_target_page_lt32 += WT_STAT_READ(from, cache_eviction_target_page_lt32);
    to->cache_eviction_target_page_ge128 += WT_STAT_READ(from, cache_eviction_target_page_ge128);
    to->cache_eviction_target_page_lt64 += WT_STAT_READ(from, cache_eviction_target_page_lt64);
    to->cache_eviction_target_page_lt128 += WT_STAT_READ(from, cache_eviction_target_page_lt128);
    to->cache_eviction_target_page_reduced +=
      WT_STAT_READ(from, cache_eviction_target_page_reduced);
    to->cache_eviction_target_strategy_clean +=
      WT_STAT_READ(from, cache_eviction_target_strategy_clean);
    to->cache_eviction_target_strategy_dirty +=
      WT_STAT_READ(from, cache_eviction_target_strategy_dirty);
    to->cache_eviction_target_strategy_updates +=
      WT_STAT_READ(from, cache_eviction_target_strategy_updates);
    to->cache_eviction_walks_abandoned += WT_STAT_READ(from, cache_eviction_walks_abandoned);
    to->cache_eviction_walks_stopped += WT_STAT_READ(from, cache_eviction_walks_stopped);
    to->cache_eviction_walks_gave_up_no_targets +=
      WT_STAT_READ(from, cache_eviction_walks_gave_up_no_targets);
    to->cache_eviction_walks_gave_up_ratio +=
      WT_STAT_READ(from, cache_eviction_walks_gave_up_ratio);
    to->cache_eviction_walk_random_returns_null_position +=
      WT_STAT_READ(from, cache_eviction_walk_random_returns_null_position);
    to->cache_eviction_walks_ended += WT_STAT_READ(from, cache_eviction_walks_ended);
    to->cache_eviction_walk_restart += WT_STAT_READ(from, cache_eviction_walk_restart);
    to->cache_eviction_walk_from_root += WT_STAT_READ(from, cache_eviction_walk_from_root);
    to->cache_eviction_walk_saved_pos += WT_STAT_READ(from, cache_eviction_walk_saved_pos);
    to->cache_eviction_active_workers += WT_STAT_READ(from, cache_eviction_active_workers);
    to->cache_eviction_worker_created += WT_STAT_READ(from, cache_eviction_worker_created);
    to->cache_eviction_worker_removed += WT_STAT_READ(from, cache_eviction_worker_removed);
    to->cache_eviction_stable_state_workers +=
      WT_STAT_READ(from, cache_eviction_stable_state_workers);
    to->cache_eviction_walks_active += WT_STAT_READ(from, cache_eviction_walks_active);
    to->cache_eviction_walks_started += WT_STAT_READ(from, cache_eviction_walks_started);
    to->cache_eviction_force_retune += WT_STAT_READ(from, cache_eviction_force_retune);
    to->cache_eviction_force_no_retry += WT_STAT_READ(from, cache_eviction_force_no_retry);
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
    to->cache_eviction_blocked_hazard += WT_STAT_READ(from, cache_eviction_blocked_hazard);
    to->cache_hazard_checks += WT_STAT_READ(from, cache_hazard_checks);
    to->cache_hazard_walks += WT_STAT_READ(from, cache_hazard_walks);
    if ((v = WT_STAT_READ(from, cache_hazard_max)) > to->cache_hazard_max)
        to->cache_hazard_max = v;
    to->cache_hs_insert += WT_STAT_READ(from, cache_hs_insert);
    to->cache_hs_insert_restart += WT_STAT_READ(from, cache_hs_insert_restart);
    to->cache_hs_ondisk_max += WT_STAT_READ(from, cache_hs_ondisk_max);
    to->cache_hs_ondisk += WT_STAT_READ(from, cache_hs_ondisk);
    to->cache_hs_read += WT_STAT_READ(from, cache_hs_read);
    to->cache_hs_read_miss += WT_STAT_READ(from, cache_hs_read_miss);
    to->cache_hs_read_squash += WT_STAT_READ(from, cache_hs_read_squash);
    to->cache_hs_order_lose_durable_timestamp +=
      WT_STAT_READ(from, cache_hs_order_lose_durable_timestamp);
    to->cache_hs_key_truncate_rts_unstable +=
      WT_STAT_READ(from, cache_hs_key_truncate_rts_unstable);
    to->cache_hs_key_truncate_rts += WT_STAT_READ(from, cache_hs_key_truncate_rts);
    to->cache_hs_btree_truncate += WT_STAT_READ(from, cache_hs_btree_truncate);
    to->cache_hs_key_truncate += WT_STAT_READ(from, cache_hs_key_truncate);
    to->cache_hs_order_remove += WT_STAT_READ(from, cache_hs_order_remove);
    to->cache_hs_key_truncate_onpage_removal +=
      WT_STAT_READ(from, cache_hs_key_truncate_onpage_removal);
    to->cache_hs_btree_truncate_dryrun += WT_STAT_READ(from, cache_hs_btree_truncate_dryrun);
    to->cache_hs_key_truncate_rts_unstable_dryrun +=
      WT_STAT_READ(from, cache_hs_key_truncate_rts_unstable_dryrun);
    to->cache_hs_key_truncate_rts_dryrun += WT_STAT_READ(from, cache_hs_key_truncate_rts_dryrun);
    to->cache_hs_order_reinsert += WT_STAT_READ(from, cache_hs_order_reinsert);
    to->cache_hs_write_squash += WT_STAT_READ(from, cache_hs_write_squash);
    to->cache_inmem_splittable += WT_STAT_READ(from, cache_inmem_splittable);
    to->cache_inmem_split += WT_STAT_READ(from, cache_inmem_split);
    to->cache_eviction_blocked_internal_page_split +=
      WT_STAT_READ(from, cache_eviction_blocked_internal_page_split);
    to->cache_eviction_internal += WT_STAT_READ(from, cache_eviction_internal);
    to->cache_eviction_internal_pages_queued +=
      WT_STAT_READ(from, cache_eviction_internal_pages_queued);
    to->cache_eviction_internal_pages_seen +=
      WT_STAT_READ(from, cache_eviction_internal_pages_seen);
    to->cache_eviction_internal_pages_already_queued +=
      WT_STAT_READ(from, cache_eviction_internal_pages_already_queued);
    to->cache_eviction_split_internal += WT_STAT_READ(from, cache_eviction_split_internal);
    to->cache_eviction_split_leaf += WT_STAT_READ(from, cache_eviction_split_leaf);
    to->cache_eviction_random_sample_inmem_root +=
      WT_STAT_READ(from, cache_eviction_random_sample_inmem_root);
    to->cache_bytes_max += WT_STAT_READ(from, cache_bytes_max);
    to->cache_eviction_maximum_gen_gap += WT_STAT_READ(from, cache_eviction_maximum_gen_gap);
    to->cache_eviction_maximum_milliseconds +=
      WT_STAT_READ(from, cache_eviction_maximum_milliseconds);
    to->cache_eviction_maximum_page_size += WT_STAT_READ(from, cache_eviction_maximum_page_size);
    to->eviction_app_dirty_attempt += WT_STAT_READ(from, eviction_app_dirty_attempt);
    to->eviction_app_dirty_fail += WT_STAT_READ(from, eviction_app_dirty_fail);
    to->cache_eviction_dirty += WT_STAT_READ(from, cache_eviction_dirty);
    to->cache_eviction_blocked_multi_block_reconcilation_during_checkpoint +=
      WT_STAT_READ(from, cache_eviction_blocked_multi_block_reconcilation_during_checkpoint);
    to->cache_eviction_trigger_dirty_reached +=
      WT_STAT_READ(from, cache_eviction_trigger_dirty_reached);
    to->cache_eviction_trigger_reached += WT_STAT_READ(from, cache_eviction_trigger_reached);
    to->cache_eviction_trigger_updates_reached +=
      WT_STAT_READ(from, cache_eviction_trigger_updates_reached);
    to->cache_timed_out_ops += WT_STAT_READ(from, cache_timed_out_ops);
    to->cache_eviction_blocked_overflow_keys +=
      WT_STAT_READ(from, cache_eviction_blocked_overflow_keys);
    to->cache_read_overflow += WT_STAT_READ(from, cache_read_overflow);
    to->eviction_app_attempt += WT_STAT_READ(from, eviction_app_attempt);
    to->eviction_app_fail += WT_STAT_READ(from, eviction_app_fail);
    to->cache_eviction_deepen += WT_STAT_READ(from, cache_eviction_deepen);
    to->cache_write_hs += WT_STAT_READ(from, cache_write_hs);
    to->cache_eviction_consider_prefetch += WT_STAT_READ(from, cache_eviction_consider_prefetch);
    to->cache_pages_inuse += WT_STAT_READ(from, cache_pages_inuse);
    to->cache_eviction_dirty_obsolete_tw += WT_STAT_READ(from, cache_eviction_dirty_obsolete_tw);
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
    to->cache_read_checkpoint += WT_STAT_READ(from, cache_read_checkpoint);
    to->cache_eviction_clear_ordinary += WT_STAT_READ(from, cache_eviction_clear_ordinary);
    to->cache_pages_requested += WT_STAT_READ(from, cache_pages_requested);
    to->cache_pages_prefetch += WT_STAT_READ(from, cache_pages_prefetch);
    to->cache_eviction_pages_seen += WT_STAT_READ(from, cache_eviction_pages_seen);
    to->cache_eviction_pages_already_queued +=
      WT_STAT_READ(from, cache_eviction_pages_already_queued);
    to->cache_eviction_fail += WT_STAT_READ(from, cache_eviction_fail);
    to->cache_eviction_fail_active_children_on_an_internal_page +=
      WT_STAT_READ(from, cache_eviction_fail_active_children_on_an_internal_page);
    to->cache_eviction_fail_in_reconciliation +=
      WT_STAT_READ(from, cache_eviction_fail_in_reconciliation);
    to->cache_eviction_fail_checkpoint_no_ts +=
      WT_STAT_READ(from, cache_eviction_fail_checkpoint_no_ts);
    to->cache_eviction_walk += WT_STAT_READ(from, cache_eviction_walk);
    to->cache_write += WT_STAT_READ(from, cache_write);
    to->cache_write_restore += WT_STAT_READ(from, cache_write_restore);
    to->cache_overhead += WT_STAT_READ(from, cache_overhead);
    to->cache_eviction_blocked_recently_modified +=
      WT_STAT_READ(from, cache_eviction_blocked_recently_modified);
    to->cache_reverse_splits += WT_STAT_READ(from, cache_reverse_splits);
    to->cache_reverse_splits_skipped_vlcs += WT_STAT_READ(from, cache_reverse_splits_skipped_vlcs);
    to->cache_hs_insert_full_update += WT_STAT_READ(from, cache_hs_insert_full_update);
    to->cache_hs_insert_reverse_modify += WT_STAT_READ(from, cache_hs_insert_reverse_modify);
    to->cache_reentry_hs_eviction_milliseconds +=
      WT_STAT_READ(from, cache_reentry_hs_eviction_milliseconds);
    to->cache_bytes_internal += WT_STAT_READ(from, cache_bytes_internal);
    to->cache_bytes_leaf += WT_STAT_READ(from, cache_bytes_leaf);
    to->cache_bytes_dirty += WT_STAT_READ(from, cache_bytes_dirty);
    to->cache_bytes_dirty_internal += WT_STAT_READ(from, cache_bytes_dirty_internal);
    to->cache_bytes_dirty_leaf += WT_STAT_READ(from, cache_bytes_dirty_leaf);
    to->cache_pages_dirty += WT_STAT_READ(from, cache_pages_dirty);
    to->cache_eviction_blocked_uncommitted_truncate +=
      WT_STAT_READ(from, cache_eviction_blocked_uncommitted_truncate);
    to->cache_eviction_clean += WT_STAT_READ(from, cache_eviction_clean);
    to->fsync_all_fh_total += WT_STAT_READ(from, fsync_all_fh_total);
    to->fsync_all_fh += WT_STAT_READ(from, fsync_all_fh);
    to->fsync_all_time += WT_STAT_READ(from, fsync_all_time);
    to->capacity_bytes_read += WT_STAT_READ(from, capacity_bytes_read);
    to->capacity_bytes_ckpt += WT_STAT_READ(from, capacity_bytes_ckpt);
    to->capacity_bytes_chunkcache += WT_STAT_READ(from, capacity_bytes_chunkcache);
    to->capacity_bytes_evict += WT_STAT_READ(from, capacity_bytes_evict);
    to->capacity_bytes_log += WT_STAT_READ(from, capacity_bytes_log);
    to->capacity_bytes_written += WT_STAT_READ(from, capacity_bytes_written);
    to->capacity_threshold += WT_STAT_READ(from, capacity_threshold);
    to->capacity_time_total += WT_STAT_READ(from, capacity_time_total);
    to->capacity_time_ckpt += WT_STAT_READ(from, capacity_time_ckpt);
    to->capacity_time_evict += WT_STAT_READ(from, capacity_time_evict);
    to->capacity_time_log += WT_STAT_READ(from, capacity_time_log);
    to->capacity_time_read += WT_STAT_READ(from, capacity_time_read);
    to->capacity_time_chunkcache += WT_STAT_READ(from, capacity_time_chunkcache);
    to->checkpoint_cleanup_success += WT_STAT_READ(from, checkpoint_cleanup_success);
    to->checkpoint_snapshot_acquired += WT_STAT_READ(from, checkpoint_snapshot_acquired);
    to->checkpoint_skipped += WT_STAT_READ(from, checkpoint_skipped);
    to->checkpoint_fsync_post += WT_STAT_READ(from, checkpoint_fsync_post);
    to->checkpoint_fsync_post_duration += WT_STAT_READ(from, checkpoint_fsync_post_duration);
    to->checkpoint_generation += WT_STAT_READ(from, checkpoint_generation);
    to->checkpoint_time_max += WT_STAT_READ(from, checkpoint_time_max);
    to->checkpoint_time_min += WT_STAT_READ(from, checkpoint_time_min);
    to->checkpoint_handle_drop_duration += WT_STAT_READ(from, checkpoint_handle_drop_duration);
    to->checkpoint_handle_duration += WT_STAT_READ(from, checkpoint_handle_duration);
    to->checkpoint_handle_apply_duration += WT_STAT_READ(from, checkpoint_handle_apply_duration);
    to->checkpoint_handle_skip_duration += WT_STAT_READ(from, checkpoint_handle_skip_duration);
    to->checkpoint_handle_meta_check_duration +=
      WT_STAT_READ(from, checkpoint_handle_meta_check_duration);
    to->checkpoint_handle_lock_duration += WT_STAT_READ(from, checkpoint_handle_lock_duration);
    to->checkpoint_handle_applied += WT_STAT_READ(from, checkpoint_handle_applied);
    to->checkpoint_handle_dropped += WT_STAT_READ(from, checkpoint_handle_dropped);
    to->checkpoint_handle_meta_checked += WT_STAT_READ(from, checkpoint_handle_meta_checked);
    to->checkpoint_handle_locked += WT_STAT_READ(from, checkpoint_handle_locked);
    to->checkpoint_handle_skipped += WT_STAT_READ(from, checkpoint_handle_skipped);
    to->checkpoint_handle_walked += WT_STAT_READ(from, checkpoint_handle_walked);
    to->checkpoint_time_recent += WT_STAT_READ(from, checkpoint_time_recent);
    to->checkpoints_api += WT_STAT_READ(from, checkpoints_api);
    to->checkpoints_compact += WT_STAT_READ(from, checkpoints_compact);
    to->checkpoint_sync += WT_STAT_READ(from, checkpoint_sync);
    to->checkpoint_presync += WT_STAT_READ(from, checkpoint_presync);
    to->checkpoint_hs_pages_reconciled += WT_STAT_READ(from, checkpoint_hs_pages_reconciled);
    to->checkpoint_pages_visited_internal += WT_STAT_READ(from, checkpoint_pages_visited_internal);
    to->checkpoint_pages_visited_leaf += WT_STAT_READ(from, checkpoint_pages_visited_leaf);
    to->checkpoint_pages_reconciled += WT_STAT_READ(from, checkpoint_pages_reconciled);
    to->checkpoint_cleanup_pages_evict += WT_STAT_READ(from, checkpoint_cleanup_pages_evict);
    to->checkpoint_cleanup_pages_obsolete_tw +=
      WT_STAT_READ(from, checkpoint_cleanup_pages_obsolete_tw);
    to->checkpoint_cleanup_pages_read_reclaim_space +=
      WT_STAT_READ(from, checkpoint_cleanup_pages_read_reclaim_space);
    to->checkpoint_cleanup_pages_read_obsolete_tw +=
      WT_STAT_READ(from, checkpoint_cleanup_pages_read_obsolete_tw);
    to->checkpoint_cleanup_pages_removed += WT_STAT_READ(from, checkpoint_cleanup_pages_removed);
    to->checkpoint_cleanup_pages_walk_skipped +=
      WT_STAT_READ(from, checkpoint_cleanup_pages_walk_skipped);
    to->checkpoint_cleanup_pages_visited += WT_STAT_READ(from, checkpoint_cleanup_pages_visited);
    to->checkpoint_prep_running += WT_STAT_READ(from, checkpoint_prep_running);
    to->checkpoint_prep_max += WT_STAT_READ(from, checkpoint_prep_max);
    to->checkpoint_prep_min += WT_STAT_READ(from, checkpoint_prep_min);
    to->checkpoint_prep_recent += WT_STAT_READ(from, checkpoint_prep_recent);
    to->checkpoint_prep_total += WT_STAT_READ(from, checkpoint_prep_total);
    to->checkpoint_state += WT_STAT_READ(from, checkpoint_state);
    to->checkpoint_scrub_target += WT_STAT_READ(from, checkpoint_scrub_target);
    to->checkpoint_scrub_max += WT_STAT_READ(from, checkpoint_scrub_max);
    to->checkpoint_scrub_min += WT_STAT_READ(from, checkpoint_scrub_min);
    to->checkpoint_scrub_recent += WT_STAT_READ(from, checkpoint_scrub_recent);
    to->checkpoint_scrub_total += WT_STAT_READ(from, checkpoint_scrub_total);
    to->checkpoint_stop_stress_active += WT_STAT_READ(from, checkpoint_stop_stress_active);
    to->checkpoint_tree_duration += WT_STAT_READ(from, checkpoint_tree_duration);
    to->checkpoints_total_failed += WT_STAT_READ(from, checkpoints_total_failed);
    to->checkpoints_total_succeed += WT_STAT_READ(from, checkpoints_total_succeed);
    to->checkpoint_time_total += WT_STAT_READ(from, checkpoint_time_total);
    to->checkpoint_obsolete_applied += WT_STAT_READ(from, checkpoint_obsolete_applied);
    to->checkpoint_wait_reduce_dirty += WT_STAT_READ(from, checkpoint_wait_reduce_dirty);
    to->chunkcache_spans_chunks_read += WT_STAT_READ(from, chunkcache_spans_chunks_read);
    to->chunkcache_chunks_evicted += WT_STAT_READ(from, chunkcache_chunks_evicted);
    to->chunkcache_exceeded_bitmap_capacity +=
      WT_STAT_READ(from, chunkcache_exceeded_bitmap_capacity);
    to->chunkcache_exceeded_capacity += WT_STAT_READ(from, chunkcache_exceeded_capacity);
    to->chunkcache_lookups += WT_STAT_READ(from, chunkcache_lookups);
    to->chunkcache_chunks_loaded_from_flushed_tables +=
      WT_STAT_READ(from, chunkcache_chunks_loaded_from_flushed_tables);
    to->chunkcache_metadata_inserted += WT_STAT_READ(from, chunkcache_metadata_inserted);
    to->chunkcache_metadata_removed += WT_STAT_READ(from, chunkcache_metadata_removed);
    to->chunkcache_metadata_work_units_dropped +=
      WT_STAT_READ(from, chunkcache_metadata_work_units_dropped);
    to->chunkcache_metadata_work_units_created +=
      WT_STAT_READ(from, chunkcache_metadata_work_units_created);
    to->chunkcache_metadata_work_units_dequeued +=
      WT_STAT_READ(from, chunkcache_metadata_work_units_dequeued);
    to->chunkcache_misses += WT_STAT_READ(from, chunkcache_misses);
    to->chunkcache_io_failed += WT_STAT_READ(from, chunkcache_io_failed);
    to->chunkcache_retries += WT_STAT_READ(from, chunkcache_retries);
    to->chunkcache_retries_checksum_mismatch +=
      WT_STAT_READ(from, chunkcache_retries_checksum_mismatch);
    to->chunkcache_toomany_retries += WT_STAT_READ(from, chunkcache_toomany_retries);
    to->chunkcache_bytes_read_persistent += WT_STAT_READ(from, chunkcache_bytes_read_persistent);
    to->chunkcache_bytes_inuse += WT_STAT_READ(from, chunkcache_bytes_inuse);
    to->chunkcache_bytes_inuse_pinned += WT_STAT_READ(from, chunkcache_bytes_inuse_pinned);
    to->chunkcache_chunks_inuse += WT_STAT_READ(from, chunkcache_chunks_inuse);
    to->chunkcache_created_from_metadata += WT_STAT_READ(from, chunkcache_created_from_metadata);
    to->chunkcache_chunks_pinned += WT_STAT_READ(from, chunkcache_chunks_pinned);
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
    to->no_session_sweep_5min += WT_STAT_READ(from, no_session_sweep_5min);
    to->no_session_sweep_60min += WT_STAT_READ(from, no_session_sweep_60min);
    to->cond_wait += WT_STAT_READ(from, cond_wait);
    to->rwlock_read += WT_STAT_READ(from, rwlock_read);
    to->rwlock_write += WT_STAT_READ(from, rwlock_write);
    to->fsync_io += WT_STAT_READ(from, fsync_io);
    to->read_io += WT_STAT_READ(from, read_io);
    to->write_io += WT_STAT_READ(from, write_io);
    to->cursor_tree_walk_del_page_skip += WT_STAT_READ(from, cursor_tree_walk_del_page_skip);
    to->cursor_next_skip_total += WT_STAT_READ(from, cursor_next_skip_total);
    to->cursor_prev_skip_total += WT_STAT_READ(from, cursor_prev_skip_total);
    to->cursor_skip_hs_cur_position += WT_STAT_READ(from, cursor_skip_hs_cur_position);
    to->cursor_tree_walk_inmem_del_page_skip +=
      WT_STAT_READ(from, cursor_tree_walk_inmem_del_page_skip);
    to->cursor_tree_walk_ondisk_del_page_skip +=
      WT_STAT_READ(from, cursor_tree_walk_ondisk_del_page_skip);
    to->cursor_search_near_prefix_fast_paths +=
      WT_STAT_READ(from, cursor_search_near_prefix_fast_paths);
    to->cursor_reposition_failed += WT_STAT_READ(from, cursor_reposition_failed);
    to->cursor_reposition += WT_STAT_READ(from, cursor_reposition);
    to->cursor_bulk_count += WT_STAT_READ(from, cursor_bulk_count);
    to->cursor_cached_count += WT_STAT_READ(from, cursor_cached_count);
    to->cursor_bound_error += WT_STAT_READ(from, cursor_bound_error);
    to->cursor_bounds_reset += WT_STAT_READ(from, cursor_bounds_reset);
    to->cursor_bounds_comparisons += WT_STAT_READ(from, cursor_bounds_comparisons);
    to->cursor_bounds_next_unpositioned += WT_STAT_READ(from, cursor_bounds_next_unpositioned);
    to->cursor_bounds_next_early_exit += WT_STAT_READ(from, cursor_bounds_next_early_exit);
    to->cursor_bounds_prev_unpositioned += WT_STAT_READ(from, cursor_bounds_prev_unpositioned);
    to->cursor_bounds_prev_early_exit += WT_STAT_READ(from, cursor_bounds_prev_early_exit);
    to->cursor_bounds_search_early_exit += WT_STAT_READ(from, cursor_bounds_search_early_exit);
    to->cursor_bounds_search_near_repositioned_cursor +=
      WT_STAT_READ(from, cursor_bounds_search_near_repositioned_cursor);
    to->cursor_insert_bulk += WT_STAT_READ(from, cursor_insert_bulk);
    to->cursor_cache_error += WT_STAT_READ(from, cursor_cache_error);
    to->cursor_cache += WT_STAT_READ(from, cursor_cache);
    to->cursor_close_error += WT_STAT_READ(from, cursor_close_error);
    to->cursor_compare_error += WT_STAT_READ(from, cursor_compare_error);
    to->cursor_create += WT_STAT_READ(from, cursor_create);
    to->cursor_equals_error += WT_STAT_READ(from, cursor_equals_error);
    to->cursor_get_key_error += WT_STAT_READ(from, cursor_get_key_error);
    to->cursor_get_value_error += WT_STAT_READ(from, cursor_get_value_error);
    to->cursor_insert += WT_STAT_READ(from, cursor_insert);
    to->cursor_insert_error += WT_STAT_READ(from, cursor_insert_error);
    to->cursor_insert_check_error += WT_STAT_READ(from, cursor_insert_check_error);
    to->cursor_insert_bytes += WT_STAT_READ(from, cursor_insert_bytes);
    to->cursor_largest_key_error += WT_STAT_READ(from, cursor_largest_key_error);
    to->cursor_modify += WT_STAT_READ(from, cursor_modify);
    to->cursor_modify_error += WT_STAT_READ(from, cursor_modify_error);
    to->cursor_modify_bytes += WT_STAT_READ(from, cursor_modify_bytes);
    to->cursor_modify_bytes_touch += WT_STAT_READ(from, cursor_modify_bytes_touch);
    to->cursor_next += WT_STAT_READ(from, cursor_next);
    to->cursor_next_error += WT_STAT_READ(from, cursor_next_error);
    to->cursor_next_hs_tombstone += WT_STAT_READ(from, cursor_next_hs_tombstone);
    to->cursor_next_skip_lt_100 += WT_STAT_READ(from, cursor_next_skip_lt_100);
    to->cursor_next_skip_ge_100 += WT_STAT_READ(from, cursor_next_skip_ge_100);
    to->cursor_next_random_error += WT_STAT_READ(from, cursor_next_random_error);
    to->cursor_restart += WT_STAT_READ(from, cursor_restart);
    to->cursor_prev += WT_STAT_READ(from, cursor_prev);
    to->cursor_prev_error += WT_STAT_READ(from, cursor_prev_error);
    to->cursor_prev_hs_tombstone += WT_STAT_READ(from, cursor_prev_hs_tombstone);
    to->cursor_prev_skip_ge_100 += WT_STAT_READ(from, cursor_prev_skip_ge_100);
    to->cursor_prev_skip_lt_100 += WT_STAT_READ(from, cursor_prev_skip_lt_100);
    to->cursor_reconfigure_error += WT_STAT_READ(from, cursor_reconfigure_error);
    to->cursor_remove += WT_STAT_READ(from, cursor_remove);
    to->cursor_remove_error += WT_STAT_READ(from, cursor_remove_error);
    to->cursor_remove_bytes += WT_STAT_READ(from, cursor_remove_bytes);
    to->cursor_reopen_error += WT_STAT_READ(from, cursor_reopen_error);
    to->cursor_reserve += WT_STAT_READ(from, cursor_reserve);
    to->cursor_reserve_error += WT_STAT_READ(from, cursor_reserve_error);
    to->cursor_reset += WT_STAT_READ(from, cursor_reset);
    to->cursor_reset_error += WT_STAT_READ(from, cursor_reset_error);
    to->cursor_search += WT_STAT_READ(from, cursor_search);
    to->cursor_search_error += WT_STAT_READ(from, cursor_search_error);
    to->cursor_search_hs += WT_STAT_READ(from, cursor_search_hs);
    to->cursor_search_near += WT_STAT_READ(from, cursor_search_near);
    to->cursor_search_near_error += WT_STAT_READ(from, cursor_search_near_error);
    to->cursor_sweep_buckets += WT_STAT_READ(from, cursor_sweep_buckets);
    to->cursor_sweep_closed += WT_STAT_READ(from, cursor_sweep_closed);
    to->cursor_sweep_examined += WT_STAT_READ(from, cursor_sweep_examined);
    to->cursor_sweep += WT_STAT_READ(from, cursor_sweep);
    to->cursor_truncate += WT_STAT_READ(from, cursor_truncate);
    to->cursor_truncate_keys_deleted += WT_STAT_READ(from, cursor_truncate_keys_deleted);
    to->cursor_update += WT_STAT_READ(from, cursor_update);
    to->cursor_update_error += WT_STAT_READ(from, cursor_update_error);
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
    to->lock_metadata_count += WT_STAT_READ(from, lock_metadata_count);
    to->lock_metadata_wait_application += WT_STAT_READ(from, lock_metadata_wait_application);
    to->lock_metadata_wait_internal += WT_STAT_READ(from, lock_metadata_wait_internal);
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
    to->log_force_remove_sleep += WT_STAT_READ(from, log_force_remove_sleep);
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
    to->perf_hist_fsread_latency_lt10 += WT_STAT_READ(from, perf_hist_fsread_latency_lt10);
    to->perf_hist_fsread_latency_lt50 += WT_STAT_READ(from, perf_hist_fsread_latency_lt50);
    to->perf_hist_fsread_latency_lt100 += WT_STAT_READ(from, perf_hist_fsread_latency_lt100);
    to->perf_hist_fsread_latency_lt250 += WT_STAT_READ(from, perf_hist_fsread_latency_lt250);
    to->perf_hist_fsread_latency_lt500 += WT_STAT_READ(from, perf_hist_fsread_latency_lt500);
    to->perf_hist_fsread_latency_lt1000 += WT_STAT_READ(from, perf_hist_fsread_latency_lt1000);
    to->perf_hist_fsread_latency_gt1000 += WT_STAT_READ(from, perf_hist_fsread_latency_gt1000);
    to->perf_hist_fsread_latency_total_msecs +=
      WT_STAT_READ(from, perf_hist_fsread_latency_total_msecs);
    to->perf_hist_fswrite_latency_lt10 += WT_STAT_READ(from, perf_hist_fswrite_latency_lt10);
    to->perf_hist_fswrite_latency_lt50 += WT_STAT_READ(from, perf_hist_fswrite_latency_lt50);
    to->perf_hist_fswrite_latency_lt100 += WT_STAT_READ(from, perf_hist_fswrite_latency_lt100);
    to->perf_hist_fswrite_latency_lt250 += WT_STAT_READ(from, perf_hist_fswrite_latency_lt250);
    to->perf_hist_fswrite_latency_lt500 += WT_STAT_READ(from, perf_hist_fswrite_latency_lt500);
    to->perf_hist_fswrite_latency_lt1000 += WT_STAT_READ(from, perf_hist_fswrite_latency_lt1000);
    to->perf_hist_fswrite_latency_gt1000 += WT_STAT_READ(from, perf_hist_fswrite_latency_gt1000);
    to->perf_hist_fswrite_latency_total_msecs +=
      WT_STAT_READ(from, perf_hist_fswrite_latency_total_msecs);
    to->perf_hist_opread_latency_lt100 += WT_STAT_READ(from, perf_hist_opread_latency_lt100);
    to->perf_hist_opread_latency_lt250 += WT_STAT_READ(from, perf_hist_opread_latency_lt250);
    to->perf_hist_opread_latency_lt500 += WT_STAT_READ(from, perf_hist_opread_latency_lt500);
    to->perf_hist_opread_latency_lt1000 += WT_STAT_READ(from, perf_hist_opread_latency_lt1000);
    to->perf_hist_opread_latency_lt10000 += WT_STAT_READ(from, perf_hist_opread_latency_lt10000);
    to->perf_hist_opread_latency_gt10000 += WT_STAT_READ(from, perf_hist_opread_latency_gt10000);
    to->perf_hist_opread_latency_total_usecs +=
      WT_STAT_READ(from, perf_hist_opread_latency_total_usecs);
    to->perf_hist_opwrite_latency_lt100 += WT_STAT_READ(from, perf_hist_opwrite_latency_lt100);
    to->perf_hist_opwrite_latency_lt250 += WT_STAT_READ(from, perf_hist_opwrite_latency_lt250);
    to->perf_hist_opwrite_latency_lt500 += WT_STAT_READ(from, perf_hist_opwrite_latency_lt500);
    to->perf_hist_opwrite_latency_lt1000 += WT_STAT_READ(from, perf_hist_opwrite_latency_lt1000);
    to->perf_hist_opwrite_latency_lt10000 += WT_STAT_READ(from, perf_hist_opwrite_latency_lt10000);
    to->perf_hist_opwrite_latency_gt10000 += WT_STAT_READ(from, perf_hist_opwrite_latency_gt10000);
    to->perf_hist_opwrite_latency_total_usecs +=
      WT_STAT_READ(from, perf_hist_opwrite_latency_total_usecs);
    to->prefetch_skipped_internal_page += WT_STAT_READ(from, prefetch_skipped_internal_page);
    to->prefetch_skipped_no_flag_set += WT_STAT_READ(from, prefetch_skipped_no_flag_set);
    to->prefetch_failed_start += WT_STAT_READ(from, prefetch_failed_start);
    to->prefetch_skipped_same_ref += WT_STAT_READ(from, prefetch_skipped_same_ref);
    to->prefetch_disk_one += WT_STAT_READ(from, prefetch_disk_one);
    to->prefetch_skipped_no_valid_dhandle += WT_STAT_READ(from, prefetch_skipped_no_valid_dhandle);
    to->prefetch_skipped += WT_STAT_READ(from, prefetch_skipped);
    to->prefetch_skipped_disk_read_count += WT_STAT_READ(from, prefetch_skipped_disk_read_count);
    to->prefetch_skipped_internal_session += WT_STAT_READ(from, prefetch_skipped_internal_session);
    to->prefetch_skipped_special_handle += WT_STAT_READ(from, prefetch_skipped_special_handle);
    to->prefetch_pages_fail += WT_STAT_READ(from, prefetch_pages_fail);
    to->prefetch_pages_queued += WT_STAT_READ(from, prefetch_pages_queued);
    to->prefetch_pages_read += WT_STAT_READ(from, prefetch_pages_read);
    to->prefetch_skipped_error_ok += WT_STAT_READ(from, prefetch_skipped_error_ok);
    to->prefetch_attempts += WT_STAT_READ(from, prefetch_attempts);
    to->rec_vlcs_emptied_pages += WT_STAT_READ(from, rec_vlcs_emptied_pages);
    to->rec_time_window_bytes_ts += WT_STAT_READ(from, rec_time_window_bytes_ts);
    to->rec_time_window_bytes_txn += WT_STAT_READ(from, rec_time_window_bytes_txn);
    to->rec_hs_wrapup_next_prev_calls += WT_STAT_READ(from, rec_hs_wrapup_next_prev_calls);
    to->rec_page_delete_fast += WT_STAT_READ(from, rec_page_delete_fast);
    to->rec_overflow_key_leaf += WT_STAT_READ(from, rec_overflow_key_leaf);
    to->rec_maximum_milliseconds += WT_STAT_READ(from, rec_maximum_milliseconds);
    to->rec_maximum_image_build_milliseconds +=
      WT_STAT_READ(from, rec_maximum_image_build_milliseconds);
    to->rec_maximum_hs_wrapup_milliseconds +=
      WT_STAT_READ(from, rec_maximum_hs_wrapup_milliseconds);
    to->rec_overflow_value += WT_STAT_READ(from, rec_overflow_value);
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
    to->flush_tier_fail += WT_STAT_READ(from, flush_tier_fail);
    to->flush_tier += WT_STAT_READ(from, flush_tier);
    to->flush_tier_skipped += WT_STAT_READ(from, flush_tier_skipped);
    to->flush_tier_switched += WT_STAT_READ(from, flush_tier_switched);
    to->local_objects_removed += WT_STAT_READ(from, local_objects_removed);
    to->session_open += WT_STAT_READ(from, session_open);
    to->session_query_ts += WT_STAT_READ(from, session_query_ts);
    to->session_table_alter_fail += WT_STAT_READ(from, session_table_alter_fail);
    to->session_table_alter_success += WT_STAT_READ(from, session_table_alter_success);
    to->session_table_alter_trigger_checkpoint +=
      WT_STAT_READ(from, session_table_alter_trigger_checkpoint);
    to->session_table_alter_skip += WT_STAT_READ(from, session_table_alter_skip);
    to->session_table_compact_conflicting_checkpoint +=
      WT_STAT_READ(from, session_table_compact_conflicting_checkpoint);
    to->session_table_compact_dhandle_success +=
      WT_STAT_READ(from, session_table_compact_dhandle_success);
    to->session_table_compact_fail += WT_STAT_READ(from, session_table_compact_fail);
    to->session_table_compact_fail_cache_pressure +=
      WT_STAT_READ(from, session_table_compact_fail_cache_pressure);
    to->session_table_compact_passes += WT_STAT_READ(from, session_table_compact_passes);
    to->session_table_compact_eviction += WT_STAT_READ(from, session_table_compact_eviction);
    to->session_table_compact_running += WT_STAT_READ(from, session_table_compact_running);
    to->session_table_compact_skipped += WT_STAT_READ(from, session_table_compact_skipped);
    to->session_table_compact_success += WT_STAT_READ(from, session_table_compact_success);
    to->session_table_compact_timeout += WT_STAT_READ(from, session_table_compact_timeout);
    to->session_table_create_fail += WT_STAT_READ(from, session_table_create_fail);
    to->session_table_create_success += WT_STAT_READ(from, session_table_create_success);
    to->session_table_create_import_fail += WT_STAT_READ(from, session_table_create_import_fail);
    to->session_table_create_import_repair +=
      WT_STAT_READ(from, session_table_create_import_repair);
    to->session_table_create_import_success +=
      WT_STAT_READ(from, session_table_create_import_success);
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
    to->tiered_work_units_removed += WT_STAT_READ(from, tiered_work_units_removed);
    to->tiered_work_units_created += WT_STAT_READ(from, tiered_work_units_created);
    to->tiered_retention += WT_STAT_READ(from, tiered_retention);
    to->thread_fsync_active += WT_STAT_READ(from, thread_fsync_active);
    to->thread_read_active += WT_STAT_READ(from, thread_read_active);
    to->thread_write_active += WT_STAT_READ(from, thread_write_active);
    to->application_cache_ops += WT_STAT_READ(from, application_cache_ops);
    to->application_evict_snapshot_refreshed +=
      WT_STAT_READ(from, application_evict_snapshot_refreshed);
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
    to->page_split_restart += WT_STAT_READ(from, page_split_restart);
    to->page_read_skip_deleted += WT_STAT_READ(from, page_read_skip_deleted);
    to->txn_prepared_updates += WT_STAT_READ(from, txn_prepared_updates);
    to->txn_prepared_updates_committed += WT_STAT_READ(from, txn_prepared_updates_committed);
    to->txn_prepared_updates_key_repeated += WT_STAT_READ(from, txn_prepared_updates_key_repeated);
    to->txn_prepared_updates_rolledback += WT_STAT_READ(from, txn_prepared_updates_rolledback);
    to->txn_read_race_prepare_commit += WT_STAT_READ(from, txn_read_race_prepare_commit);
    to->txn_read_overflow_remove += WT_STAT_READ(from, txn_read_overflow_remove);
    to->txn_rollback_oldest_pinned += WT_STAT_READ(from, txn_rollback_oldest_pinned);
    to->txn_rollback_oldest_id += WT_STAT_READ(from, txn_rollback_oldest_id);
    to->txn_prepare += WT_STAT_READ(from, txn_prepare);
    to->txn_prepare_commit += WT_STAT_READ(from, txn_prepare_commit);
    to->txn_prepare_active += WT_STAT_READ(from, txn_prepare_active);
    to->txn_prepare_rollback += WT_STAT_READ(from, txn_prepare_rollback);
    to->txn_query_ts += WT_STAT_READ(from, txn_query_ts);
    to->txn_read_race_prepare_update += WT_STAT_READ(from, txn_read_race_prepare_update);
    to->txn_rts += WT_STAT_READ(from, txn_rts);
    to->txn_rts_sweep_hs_keys_dryrun += WT_STAT_READ(from, txn_rts_sweep_hs_keys_dryrun);
    to->txn_rts_hs_stop_older_than_newer_start +=
      WT_STAT_READ(from, txn_rts_hs_stop_older_than_newer_start);
    to->txn_rts_inconsistent_ckpt += WT_STAT_READ(from, txn_rts_inconsistent_ckpt);
    to->txn_rts_keys_removed += WT_STAT_READ(from, txn_rts_keys_removed);
    to->txn_rts_keys_restored += WT_STAT_READ(from, txn_rts_keys_restored);
    to->txn_rts_keys_removed_dryrun += WT_STAT_READ(from, txn_rts_keys_removed_dryrun);
    to->txn_rts_keys_restored_dryrun += WT_STAT_READ(from, txn_rts_keys_restored_dryrun);
    to->txn_rts_pages_visited += WT_STAT_READ(from, txn_rts_pages_visited);
    to->txn_rts_hs_restore_tombstones += WT_STAT_READ(from, txn_rts_hs_restore_tombstones);
    to->txn_rts_hs_restore_updates += WT_STAT_READ(from, txn_rts_hs_restore_updates);
    to->txn_rts_delete_rle_skipped += WT_STAT_READ(from, txn_rts_delete_rle_skipped);
    to->txn_rts_stable_rle_skipped += WT_STAT_READ(from, txn_rts_stable_rle_skipped);
    to->txn_rts_sweep_hs_keys += WT_STAT_READ(from, txn_rts_sweep_hs_keys);
    to->txn_rts_hs_restore_tombstones_dryrun +=
      WT_STAT_READ(from, txn_rts_hs_restore_tombstones_dryrun);
    to->txn_rts_tree_walk_skip_pages += WT_STAT_READ(from, txn_rts_tree_walk_skip_pages);
    to->txn_rts_upd_aborted += WT_STAT_READ(from, txn_rts_upd_aborted);
    to->txn_rts_hs_restore_updates_dryrun += WT_STAT_READ(from, txn_rts_hs_restore_updates_dryrun);
    to->txn_rts_hs_removed += WT_STAT_READ(from, txn_rts_hs_removed);
    to->txn_rts_upd_aborted_dryrun += WT_STAT_READ(from, txn_rts_upd_aborted_dryrun);
    to->txn_rts_hs_removed_dryrun += WT_STAT_READ(from, txn_rts_hs_removed_dryrun);
    to->txn_sessions_walked += WT_STAT_READ(from, txn_sessions_walked);
    to->txn_set_ts += WT_STAT_READ(from, txn_set_ts);
    to->txn_set_ts_durable += WT_STAT_READ(from, txn_set_ts_durable);
    to->txn_set_ts_durable_upd += WT_STAT_READ(from, txn_set_ts_durable_upd);
    to->txn_set_ts_force += WT_STAT_READ(from, txn_set_ts_force);
    to->txn_set_ts_out_of_order += WT_STAT_READ(from, txn_set_ts_out_of_order);
    to->txn_set_ts_oldest += WT_STAT_READ(from, txn_set_ts_oldest);
    to->txn_set_ts_oldest_upd += WT_STAT_READ(from, txn_set_ts_oldest_upd);
    to->txn_set_ts_stable += WT_STAT_READ(from, txn_set_ts_stable);
    to->txn_set_ts_stable_upd += WT_STAT_READ(from, txn_set_ts_stable_upd);
    to->txn_begin += WT_STAT_READ(from, txn_begin);
    to->txn_hs_ckpt_duration += WT_STAT_READ(from, txn_hs_ckpt_duration);
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
  "join: accesses to the main table",
  "join: bloom filter false positives",
  "join: checks that conditions of membership are satisfied",
  "join: items inserted into a bloom filter",
  "join: items iterated",
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
  "session: dirty bytes in this txn",
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
    stats->txn_bytes_dirty = 0;
    stats->read_time = 0;
    stats->write_time = 0;
    stats->lock_schema_wait = 0;
    stats->cache_time = 0;
}
