#!/usr/bin/env python3

# Auto-generate statistics #defines, with initialization, clear and aggregate
# functions.
#
# NOTE: Statistics reports show individual objects as operations per second.
# All objects where that does not make sense should have the word 'currently'
# or the phrase 'in the cache' in their text description, for example, 'files
# currently open'.
# NOTE: All statistics descriptions must have a prefix string followed by ':'.
#
# Data-source statistics are normally aggregated across the set of underlying
# objects. Additional optional configuration flags are available:
#       cache_walk      Only reported when statistics=cache_walk is set
#       tree_walk       Only reported when statistics=tree_walk is set
#       max_aggregate   Take the maximum value when aggregating statistics
#       no_clear        Value not cleared when statistics cleared
#       no_scale        Don't scale value per second in the logging tool script
#       size            Used by timeseries tool, indicates value is a byte count
#
# The no_clear and no_scale flags are normally always set together (values that
# are maintained over time are normally not scaled per second).

from operator import attrgetter
import sys

class Stat:
    def __init__(self, name, tag, desc, flags=''):
        self.name = name
        self.desc = tag + ': ' + desc
        self.flags = flags

    def __cmp__(self, other):
        return cmp(self.desc.lower(), other.desc.lower())

class AutoCommitStat(Stat):
    prefix = 'autocommit'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, AutoCommitStat.prefix, desc, flags)

class BackgroundCompactStat(Stat):
    prefix = 'background-compact'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, BackgroundCompactStat.prefix, desc, flags)

class BackupStat(Stat):
    prefix = 'backup'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, BackupStat.prefix, desc, flags)

class BlockCacheStat(Stat):
    prefix = 'block-cache'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, BlockCacheStat.prefix, desc, flags)
class BlockDisaggStat(Stat):
    prefix = 'block-disagg'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, BlockDisaggStat.prefix, desc, flags)
class BlockStat(Stat):
    prefix = 'block-manager'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, BlockStat.prefix, desc, flags)
class BtreeStat(Stat):
    prefix = 'btree'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, BtreeStat.prefix, desc, flags)
class CacheStat(Stat):
    prefix = 'cache'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, CacheStat.prefix, desc, flags)
class CapacityStat(Stat):
    prefix = 'capacity'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, CapacityStat.prefix, desc, flags)
class CheckpointStat(Stat):
    prefix = 'checkpoint'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, CheckpointStat.prefix, desc, flags)
class ChunkCacheStat(Stat):
    prefix = 'chunk-cache'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, ChunkCacheStat.prefix, desc, flags)
class CompressStat(Stat):
    prefix = 'compression'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, CompressStat.prefix, desc, flags)
class ConnStat(Stat):
    prefix = 'connection'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, ConnStat.prefix, desc, flags)
class CursorErrorStat(Stat):
    prefix = 'cursor'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, CursorStat.prefix, desc, flags)
class CursorStat(Stat):
    prefix = 'cursor'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, CursorStat.prefix, desc, flags)
class CursorSweepStat(Stat):
    prefix = 'cursor'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, CursorStat.prefix, desc, flags)
class DhandleStat(Stat):
    prefix = 'data-handle'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, DhandleStat.prefix, desc, flags)
class EvictCacheWalkStat(Stat):
    prefix = 'cache_walk'
    def __init__(self, name, desc, flags=''):
        flags += ',cache_walk'
        Stat.__init__(self, name, EvictCacheWalkStat.prefix, desc, flags)
class EvictStat(Stat):
    prefix = 'cache'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, EvictStat.prefix, desc, flags)
class LiveRestoreStat(Stat):
    prefix = 'live-restore'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, LiveRestoreStat.prefix, desc, flags)
class LockStat(Stat):
    prefix = 'lock'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, LockStat.prefix, desc, flags)
class LogStat(Stat):
    prefix = 'log'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, LogStat.prefix, desc, flags)
class SessionStat(Stat):
    prefix = 'session'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, SessionStat.prefix, desc, flags)
class PerfHistStat(Stat):
    prefix = 'perf'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, PerfHistStat.prefix, desc, flags)
class PrefetchStat(Stat):
    prefix = 'prefetch'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, PrefetchStat.prefix, desc, flags)
class RecStat(Stat):
    prefix = 'reconciliation'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, RecStat.prefix, desc, flags)
class SessionOpStat(Stat):
    prefix = 'session'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, SessionOpStat.prefix, desc, flags)
class StorageStat(Stat):
    prefix = 'tiered-storage'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, SessionOpStat.prefix, desc, flags)
class ThreadStat(Stat):
    prefix = 'thread-state'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, ThreadStat.prefix, desc, flags)
class TxnStat(Stat):
    prefix = 'transaction'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, TxnStat.prefix, desc, flags)
class YieldStat(Stat):
    prefix = 'thread-yield'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, YieldStat.prefix, desc, flags)

##########################################
# CONNECTION statistics
##########################################
conn_stats = [
    ##########################################
    # System statistics
    ##########################################
    ConnStat('buckets', 'hash bucket array size general', 'no_clear,no_scale,size'),
    ConnStat('buckets_dh', 'hash bucket array size for data handles', 'no_clear,no_scale,size'),
    ConnStat('cond_auto_wait', 'auto adjusting condition wait calls'),
    ConnStat('cond_auto_wait_reset', 'auto adjusting condition resets'),
    ConnStat('cond_auto_wait_skipped', 'auto adjusting condition wait raced to update timeout and skipped updating'),
    ConnStat('cond_wait', 'pthread mutex condition wait calls'),
    ConnStat('file_open', 'files currently open', 'no_clear,no_scale'),
    ConnStat('fsync_io', 'total fsync I/Os'),
    ConnStat('memory_allocation', 'memory allocations'),
    ConnStat('memory_free', 'memory frees'),
    ConnStat('memory_grow', 'memory re-allocations'),
    ConnStat('no_session_sweep_5min', 'number of sessions without a sweep for 5+ minutes'),
    ConnStat('no_session_sweep_60min', 'number of sessions without a sweep for 60+ minutes'),
    ConnStat('read_io', 'total read I/Os'),
    ConnStat('rwlock_read', 'pthread mutex shared lock read-lock calls'),
    ConnStat('rwlock_write', 'pthread mutex shared lock write-lock calls'),
    ConnStat('time_travel', 'detected system time went backwards'),
    ConnStat('write_io', 'total write I/Os'),

    ##########################################
    # Background compaction statistics
    ##########################################
    BackgroundCompactStat('background_compact_bytes_recovered', 'background compact recovered bytes', 'no_scale'),
    BackgroundCompactStat('background_compact_ema', 'background compact moving average of bytes rewritten', 'no_scale'),
    BackgroundCompactStat('background_compact_exclude', 'background compact skipped file as it is part of the exclude list', 'no_scale'),
    BackgroundCompactStat('background_compact_fail', 'background compact failed calls', 'no_scale'),
    BackgroundCompactStat('background_compact_fail_cache_pressure', 'background compact failed calls due to cache pressure', 'no_scale'),
    BackgroundCompactStat('background_compact_files_tracked', 'number of files tracked by background compaction', 'no_scale'),
    BackgroundCompactStat('background_compact_interrupted', 'background compact interrupted', 'no_scale'),
    BackgroundCompactStat('background_compact_running', 'background compact running', 'no_scale'),
    BackgroundCompactStat('background_compact_skipped', 'background compact skipped file as not meeting requirements for compaction', 'no_scale'),
    BackgroundCompactStat('background_compact_sleep_cache_pressure', 'background compact sleeps due to cache pressure', 'no_scale'),
    BackgroundCompactStat('background_compact_success', 'background compact successful calls', 'no_scale'),
    BackgroundCompactStat('background_compact_timeout', 'background compact timeout', 'no_scale'),

    ##########################################
    # Backup statistics
    ##########################################
    BackupStat('backup_bits_clr', 'backup total bits cleared'),
    BackupStat('backup_blocks', 'total modified incremental blocks'),
    BackupStat('backup_cursor_open', 'backup cursor open', 'no_clear,no_scale'),
    BackupStat('backup_dup_open', 'backup duplicate cursor open', 'no_clear,no_scale'),
    BackupStat('backup_granularity', 'backup granularity size'),
    BackupStat('backup_incremental', 'incremental backup enabled', 'no_clear,no_scale'),
    BackupStat('backup_start', 'opening the backup cursor in progress', 'no_clear,no_scale'),

    ##########################################
    # Block cache statistics
    ##########################################
    BlockCacheStat('block_cache_blocks', 'total blocks'),
    BlockCacheStat('block_cache_blocks_evicted', 'evicted blocks'),
    BlockCacheStat('block_cache_blocks_insert_read', 'total blocks inserted on read path'),
    BlockCacheStat('block_cache_blocks_insert_write', 'total blocks inserted on write path'),
    BlockCacheStat('block_cache_blocks_removed', 'removed blocks'),
    BlockCacheStat('block_cache_blocks_removed_blocked', 'time sleeping to remove block (usecs)'),
    BlockCacheStat('block_cache_blocks_update', 'cached blocks updated'),
    BlockCacheStat('block_cache_bypass_chkpt', 'number of put bypasses on checkpoint I/O'),
    BlockCacheStat('block_cache_bypass_filesize', 'file size causing bypass'),
    BlockCacheStat('block_cache_bypass_get', 'number of bypasses on get'),
    BlockCacheStat('block_cache_bypass_overhead_put', 'number of bypasses due to overhead on put'),
    BlockCacheStat('block_cache_bypass_put', 'number of bypasses on put because file is too small'),
    BlockCacheStat('block_cache_bypass_writealloc', 'number of bypasses because no-write-allocate setting was on'),
    BlockCacheStat('block_cache_bytes', 'total bytes'),
    BlockCacheStat('block_cache_bytes_insert_read', 'total bytes inserted on read path'),
    BlockCacheStat('block_cache_bytes_insert_write', 'total bytes inserted on write path'),
    BlockCacheStat('block_cache_bytes_update', 'cached bytes updated'),
    BlockCacheStat('block_cache_eviction_passes', 'number of eviction passes'),
    BlockCacheStat('block_cache_hits', 'number of hits'),
    BlockCacheStat('block_cache_lookups', 'lookups'),
    BlockCacheStat('block_cache_misses', 'number of misses'),
    BlockCacheStat('block_cache_not_evicted_overhead', 'number of blocks not evicted due to overhead'),

    ##########################################
    # Block manager statistics
    ##########################################
    BlockStat('block_byte_map_read', 'mapped bytes read', 'size'),
    BlockStat('block_byte_read', 'bytes read', 'size'),
    BlockStat('block_byte_read_intl', 'bytes read for internal pages', 'size'),
    BlockStat('block_byte_read_intl_disk', 'bytes read for internal pages before decompression and decryption', 'size'),
    BlockStat('block_byte_read_leaf', 'bytes read for leaf pages', 'size'),
    BlockStat('block_byte_read_leaf_disk', 'bytes read for leaf pages before decompression and decryption', 'size'),
    BlockStat('block_byte_read_mmap', 'bytes read via memory map API', 'size'),
    BlockStat('block_byte_read_syscall', 'bytes read via system call API', 'size'),
    BlockStat('block_byte_write', 'bytes written', 'size'),
    BlockStat('block_byte_write_checkpoint', 'bytes written for checkpoint', 'size'),
    BlockStat('block_byte_write_compact', 'bytes written by compaction', 'size'),
    BlockStat('block_byte_write_intl', 'bytes written for internal pages before compression and encryption', 'size'),
    BlockStat('block_byte_write_intl_disk', 'bytes written for internal pages after compression and encryption', 'size'),
    BlockStat('block_byte_write_leaf', 'bytes written for leaf pages before compression and encryption', 'size'),
    BlockStat('block_byte_write_leaf_disk', 'bytes written for leaf pages after compression and encryption', 'size'),
    BlockStat('block_byte_write_mmap', 'bytes written via memory map API', 'size'),
    BlockStat('block_byte_write_syscall', 'bytes written via system call API', 'size'),
    BlockStat('block_first_srch_walk_time', 'time spent(usecs) on the most recent linear walk of extents during first-fit allocation', 'no_clear,no_scale'),
    BlockStat('block_map_read', 'mapped blocks read'),
    BlockStat('block_preload', 'blocks pre-loaded'),
    BlockStat('block_read', 'blocks read'),
    BlockStat('block_remap_file_resize', 'number of times the file was remapped because it changed size via fallocate or truncate'),
    BlockStat('block_remap_file_write', 'number of times the region was remapped via write'),
    BlockStat('block_write', 'blocks written'),

    ##########################################
    # Cache statistics
    ##########################################
    CacheStat('cache_bytes_hs', 'bytes belonging to the history store table in the cache', 'no_clear,no_scale,size'),
    CacheStat('cache_bytes_hs_dirty', 'dirty bytes belonging to the history store table in the cache', 'no_clear,no_scale,size'),
    CacheStat('cache_bytes_hs_updates', 'update bytes belonging to the history store table in the cache', 'no_clear,no_scale,size'),
    CacheStat('cache_bytes_image', 'bytes belonging to page images in the cache', 'no_clear,no_scale,size'),
    CacheStat('cache_bytes_internal', 'tracked bytes belonging to internal pages in the cache', 'no_clear,no_scale,size'),
    CacheStat('cache_bytes_leaf', 'tracked bytes belonging to leaf pages in the cache', 'no_clear,no_scale,size'),
    CacheStat('cache_bytes_max', 'maximum bytes configured', 'no_clear,no_scale,size'),
    CacheStat('cache_bytes_other', 'bytes not belonging to page images in the cache', 'no_clear,no_scale,size'),
    CacheStat('cache_bytes_updates', 'bytes allocated for updates', 'no_clear,no_scale,size'),
    CacheStat('cache_hazard_checks', 'hazard pointer check calls'),
    CacheStat('cache_hazard_max', 'hazard pointer maximum array length', 'max_aggregate,no_scale'),
    CacheStat('cache_hazard_walks', 'hazard pointer check entries walked'),
    CacheStat('cache_hs_ondisk', 'history store table on-disk size', 'no_clear,no_scale,size'),
    CacheStat('cache_hs_ondisk_max', 'history store table max on-disk size', 'no_clear,no_scale,size'),
    CacheStat('cache_overhead', 'percentage overhead', 'no_clear,no_scale'),
    CacheStat('cache_pages_dirty', 'tracked dirty pages in the cache', 'no_clear,no_scale'),
    CacheStat('cache_pages_inuse', 'pages currently held in the cache', 'no_clear,no_scale'),
    CacheStat('cache_read_app_count', 'application threads page read from disk to cache count'),
    CacheStat('cache_read_app_time', 'application threads page read from disk to cache time (usecs)'),
    CacheStat('cache_updates_txn_uncommitted_bytes', 'updates in uncommitted txn - bytes', 'no_clear,no_scale,size'),
    CacheStat('cache_updates_txn_uncommitted_count', 'updates in uncommitted txn - count', 'no_clear,no_scale,size'),
    CacheStat('cache_write_app_count', 'application threads page write from cache to disk count'),
    CacheStat('cache_write_app_time', 'application threads page write from cache to disk time (usecs)'),
    CacheStat('npos_evict_walk_max', 'eviction walk restored - had to walk this many pages', 'max_aggregate,no_scale'),
    CacheStat('npos_read_walk_max', 'npos read - had to walk this many pages', 'max_aggregate,no_scale'),

    ##########################################
    # Eviction statistics
    ##########################################
    EvictStat('eviction_active_workers', 'eviction worker thread active', 'no_clear'),
    EvictStat('eviction_aggressive_set', 'eviction currently operating in aggressive mode', 'no_clear,no_scale'),
    EvictStat('eviction_app_attempt', 'page evict attempts by application threads'),
    EvictStat('eviction_app_dirty_attempt', 'modified page evict attempts by application threads'),
    EvictStat('eviction_app_dirty_fail', 'modified page evict failures by application threads'),
    EvictStat('eviction_app_fail', 'page evict failures by application threads'),
    EvictStat('eviction_app_time', 'application thread time evicting (usecs)'),
    EvictStat('eviction_clear_ordinary', 'pages removed from the ordinary queue to be queued for urgent eviction'),
    EvictStat('eviction_consider_prefetch', 'pages considered for eviction that were brought in by pre-fetch', 'no_clear,no_scale'),
    EvictStat('eviction_empty_score', 'eviction empty score', 'no_clear,no_scale'),
    EvictStat('eviction_fail', 'pages selected for eviction unable to be evicted'),
    EvictStat('eviction_fail_active_children_on_an_internal_page', 'pages selected for eviction unable to be evicted because of active children on an internal page'),
    EvictStat('eviction_fail_checkpoint_no_ts', 'pages selected for eviction unable to be evicted because of race between checkpoint and updates without timestamps'),
    EvictStat('eviction_fail_in_reconciliation', 'pages selected for eviction unable to be evicted because of failure in reconciliation'),
    EvictStat('eviction_force', 'forced eviction - pages selected count'),
    EvictStat('eviction_force_clean', 'forced eviction - pages evicted that were clean count'),
    EvictStat('eviction_force_delete', 'forced eviction - pages selected because of too many deleted items count'),
    EvictStat('eviction_force_dirty', 'forced eviction - pages evicted that were dirty count'),
    EvictStat('eviction_force_fail', 'forced eviction - pages selected unable to be evicted count'),
    EvictStat('eviction_force_hs', 'forced eviction - history store pages selected while session has history store cursor open'),
    EvictStat('eviction_force_hs_fail', 'forced eviction - history store pages failed to evict while session has history store cursor open'),
    EvictStat('eviction_force_hs_success', 'forced eviction - history store pages successfully evicted while session has history store cursor open'),
    EvictStat('eviction_force_long_update_list', 'forced eviction - pages selected because of a large number of updates to a single item'),
    EvictStat('eviction_force_no_retry', 'forced eviction - do not retry count to evict pages selected to evict during reconciliation'),
    EvictStat('eviction_get_ref_empty', 'eviction calls to get a page found queue empty'),
    EvictStat('eviction_get_ref_empty2', 'eviction calls to get a page found queue empty after locking'),
    EvictStat('eviction_internal_pages_already_queued', 'internal pages seen by eviction walk that are already queued'),
    EvictStat('eviction_internal_pages_queued', 'internal pages queued for eviction'),
    EvictStat('eviction_internal_pages_seen', 'internal pages seen by eviction walk'),
    EvictStat('eviction_interupted_by_app', 'application requested eviction interrupt'),
    EvictStat('eviction_maximum_clean_page_size_per_checkpoint', 'maximum clean page size seen at eviction per checkpoint', 'no_clear,no_scale,size'),
    EvictStat('eviction_maximum_dirty_page_size_per_checkpoint', 'maximum dirty page size seen at eviction per checkpoint', 'no_clear,no_scale,size'),
    EvictStat('eviction_maximum_milliseconds', 'maximum milliseconds spent at a single eviction', 'no_clear,no_scale,size'),
    EvictStat('eviction_maximum_milliseconds_per_checkpoint', 'maximum milliseconds spent at a single eviction per checkpoint', 'no_clear,no_scale,size'),
    EvictStat('eviction_maximum_page_size', 'maximum page size seen at eviction', 'no_clear,no_scale,size'),
    EvictStat('eviction_maximum_unvisited_gen_gap', 'maximum gap between unvisited page and connection evict pass generation seen at eviction', 'no_clear,no_scale,size'),
    EvictStat('eviction_maximum_unvisited_gen_gap_per_checkpoint', 'maximum gap between unvisited page and connection evict pass generation seen at eviction per checkpoint', 'no_clear,no_scale,size'),
    EvictStat('eviction_maximum_updates_page_size_per_checkpoint', 'maximum updates page size seen at eviction per checkpoint', 'no_clear,no_scale,size'),
    EvictStat('eviction_maximum_visited_gen_gap', 'maximum gap between visited page and connection evict pass generation seen at eviction', 'no_clear,no_scale,size'),
    EvictStat('eviction_maximum_visited_gen_gap_per_checkpoint', 'maximum gap between visited page and connection evict pass generation seen at eviction per checkpoint', 'no_clear,no_scale,size'),
    EvictStat('eviction_pages_already_queued', 'pages seen by eviction walk that are already queued'),
    EvictStat('eviction_pages_in_parallel_with_checkpoint', 'pages evicted in parallel with checkpoint'),
    EvictStat('eviction_pages_ordinary_queued', 'pages queued for eviction'),
    EvictStat('eviction_pages_queued_oldest', 'pages queued for urgent eviction during walk'),
    EvictStat('eviction_pages_queued_post_lru', 'pages queued for eviction post lru sorting'),
    EvictStat('eviction_pages_queued_urgent', 'pages queued for urgent eviction'),
    EvictStat('eviction_pages_queued_urgent_hs_dirty', 'pages queued for urgent eviction from history store due to high dirty content'),
    EvictStat('eviction_queue_empty', 'eviction server candidate queue empty when topping up'),
    EvictStat('eviction_queue_not_empty', 'eviction server candidate queue not empty when topping up'),
    EvictStat('eviction_reentry_hs_eviction_milliseconds', 'total milliseconds spent inside reentrant history store evictions in a reconciliation', 'no_clear,no_scale,size'),
    EvictStat('eviction_restored_pos', 'eviction walk restored position'),
    EvictStat('eviction_restored_pos_differ', 'eviction walk restored position differs from the saved one'),
    EvictStat('eviction_server_evict_attempt', 'evict page attempts by eviction server'),
    EvictStat('eviction_server_evict_fail', 'evict page failures by eviction server'),
    # Note eviction_server_evict_attempt - eviction_server_evict_fail = evict page successes by eviction server.
    EvictStat('eviction_server_skip_checkpointing_trees', 'eviction server skips trees that are being checkpointed'),
    EvictStat('eviction_server_skip_dirty_pages_during_checkpoint', 'eviction server skips dirty pages during a running checkpoint'),
    EvictStat('eviction_server_skip_intl_page_with_active_child', 'eviction server skips internal pages as it has an active child.'),
    EvictStat('eviction_server_skip_metatdata_with_history', 'eviction server skips metadata pages with history'),
    EvictStat('eviction_server_skip_pages_last_running', 'eviction server skips pages that are written with transactions greater than the last running'),
    EvictStat('eviction_server_skip_pages_retry', 'eviction server skips pages that previously failed eviction and likely will again'),
    EvictStat('eviction_server_skip_trees_eviction_disabled', 'eviction server skips trees that disable eviction'),
    EvictStat('eviction_server_skip_trees_not_useful_before', 'eviction server skips trees that were not useful before'),
    EvictStat('eviction_server_skip_trees_stick_in_cache', 'eviction server skips trees that are configured to stick in cache'),
    EvictStat('eviction_server_skip_trees_too_many_active_walks', 'eviction server skips trees because there are too many active walks'),
    EvictStat('eviction_server_skip_unwanted_pages', 'eviction server skips pages that we do not want to evict'),
    EvictStat('eviction_server_skip_unwanted_tree', 'eviction server skips tree that we do not want to evict'),
    EvictStat('eviction_server_slept', 'eviction server slept, because we did not make progress with eviction'),
    EvictStat('eviction_slow', 'eviction server unable to reach eviction goal'),
    EvictStat('eviction_stable_state_workers', 'eviction worker thread stable number', 'no_clear'),
    EvictStat('eviction_state', 'eviction state', 'no_clear,no_scale'),
    EvictStat('eviction_target_strategy_clean', 'eviction walk target strategy clean pages'),
    EvictStat('eviction_target_strategy_dirty', 'eviction walk target strategy dirty pages'),
    EvictStat('eviction_target_strategy_updates', 'eviction walk target strategy pages with updates'),
    EvictStat('eviction_timed_out_ops', 'operations timed out waiting for space in cache'),
    EvictStat('eviction_walk', 'pages walked for eviction'),
    EvictStat('eviction_walk_from_root', 'eviction walks started from root of tree'),
    EvictStat('eviction_walk_leaf_notfound', 'eviction server waiting for a leaf page'),
    EvictStat('eviction_walk_passes', 'eviction passes of a file'),
    EvictStat('eviction_walk_random_returns_null_position', 'eviction walks random search fails to locate a page, results in a null position'),
    EvictStat('eviction_walk_restart', 'eviction walks restarted'),
    EvictStat('eviction_walk_saved_pos', 'eviction walks started from saved location in tree'),
    EvictStat('eviction_walk_sleeps', 'eviction walk most recent sleeps for checkpoint handle gathering'),
    EvictStat('eviction_walks_abandoned', 'eviction walks abandoned'),
    EvictStat('eviction_walks_active', 'files with active eviction walks', 'no_clear,no_scale'),
    EvictStat('eviction_walks_ended', 'eviction walks reached end of tree'),
    EvictStat('eviction_walks_gave_up_no_targets', 'eviction walks gave up because they saw too many pages and found no candidates'),
    EvictStat('eviction_walks_gave_up_ratio', 'eviction walks gave up because they saw too many pages and found too few candidates'),
    EvictStat('eviction_walks_started', 'files with new eviction walks started'),
    EvictStat('eviction_walks_stopped', 'eviction walks gave up because they restarted their walk twice'),
    EvictStat('eviction_worker_evict_attempt', 'evict page attempts by eviction worker threads'),
    EvictStat('eviction_worker_evict_fail', 'evict page failures by eviction worker threads'),
    # Note eviction_worker_evict_attempt - eviction_worker_evict_fail = evict page successes by eviction worker threads.

    ##########################################
    # Capacity statistics
    ##########################################
    CapacityStat('capacity_bytes_chunkcache', 'bytes written for chunk cache'),
    CapacityStat('capacity_bytes_ckpt', 'bytes written for checkpoint'),
    CapacityStat('capacity_bytes_evict', 'bytes written for eviction'),
    CapacityStat('capacity_bytes_log', 'bytes written for log'),
    CapacityStat('capacity_bytes_read', 'bytes read'),
    CapacityStat('capacity_bytes_written', 'bytes written total'),
    CapacityStat('capacity_threshold', 'threshold to call fsync'),
    CapacityStat('capacity_time_chunkcache', 'time waiting for chunk cache IO bandwidth (usecs)'),
    CapacityStat('capacity_time_ckpt', 'time waiting during checkpoint (usecs)'),
    CapacityStat('capacity_time_evict', 'time waiting during eviction (usecs)'),
    CapacityStat('capacity_time_log', 'time waiting during logging (usecs)'),
    CapacityStat('capacity_time_read', 'time waiting during read (usecs)'),
    CapacityStat('capacity_time_total', 'time waiting due to total capacity (usecs)'),
    CapacityStat('fsync_all_fh', 'background fsync file handles synced'),
    CapacityStat('fsync_all_fh_total', 'background fsync file handles considered'),
    CapacityStat('fsync_all_time', 'background fsync time (msecs)', 'no_clear,no_scale'),

    ##########################################
    # Checkpoint statistics
    ##########################################
    CheckpointStat('checkpoint_cleanup_success', 'checkpoint cleanup successful calls'),
    CheckpointStat('checkpoint_fsync_post', 'fsync calls after allocating the transaction ID'),
    CheckpointStat('checkpoint_fsync_post_duration', 'fsync duration after allocating the transaction ID (usecs)', 'no_clear,no_scale'),
    CheckpointStat('checkpoint_generation', 'generation', 'no_clear,no_scale'),
    CheckpointStat('checkpoint_handle_applied', 'most recent handles applied'),
    CheckpointStat('checkpoint_handle_apply_duration', 'most recent duration for gathering applied handles (usecs)', 'no_clear,no_scale'),
    CheckpointStat('checkpoint_handle_drop_duration', 'most recent duration for checkpoint dropping all handles (usecs)', 'no_clear,no_scale'),
    CheckpointStat('checkpoint_handle_dropped', 'most recent handles checkpoint dropped'),
    CheckpointStat('checkpoint_handle_duration', 'most recent duration for gathering all handles (usecs)', 'no_clear,no_scale'),
    CheckpointStat('checkpoint_handle_lock_duration', 'most recent duration for locking the handles (usecs)', 'no_clear,no_scale'),
    CheckpointStat('checkpoint_handle_locked', 'most recent handles metadata locked'),
    CheckpointStat('checkpoint_handle_meta_check_duration', 'most recent duration for handles metadata checked (usecs)', 'no_clear,no_scale'),
    CheckpointStat('checkpoint_handle_meta_checked', 'most recent handles metadata checked'),
    CheckpointStat('checkpoint_handle_skip_duration', 'most recent duration for gathering skipped handles (usecs)', 'no_clear,no_scale'),
    CheckpointStat('checkpoint_handle_skipped', 'most recent handles skipped'),
    CheckpointStat('checkpoint_handle_walked', 'most recent handles walked'),
    CheckpointStat('checkpoint_hs_pages_reconciled', 'number of history store pages caused to be reconciled'),
    CheckpointStat('checkpoint_pages_reconciled', 'number of pages caused to be reconciled'),
    CheckpointStat('checkpoint_pages_visited_internal', 'number of internal pages visited'),
    CheckpointStat('checkpoint_pages_visited_leaf', 'number of leaf pages visited'),
    CheckpointStat('checkpoint_prep_max', 'prepare max time (msecs)', 'no_clear,no_scale'),
    CheckpointStat('checkpoint_prep_min', 'prepare min time (msecs)', 'no_clear,no_scale'),
    CheckpointStat('checkpoint_prep_recent', 'prepare most recent time (msecs)', 'no_clear,no_scale'),
    CheckpointStat('checkpoint_prep_running', 'prepare currently running', 'no_clear,no_scale'),
    CheckpointStat('checkpoint_prep_total', 'prepare total time (msecs)', 'no_clear,no_scale'),
    CheckpointStat('checkpoint_presync', 'number of handles visited after writes complete'),
    CheckpointStat('checkpoint_scrub_max', 'scrub max time (msecs)', 'no_clear,no_scale'),
    CheckpointStat('checkpoint_scrub_min', 'scrub min time (msecs)', 'no_clear,no_scale'),
    CheckpointStat('checkpoint_scrub_recent', 'scrub most recent time (msecs)', 'no_clear,no_scale'),
    CheckpointStat('checkpoint_scrub_target', 'scrub dirty target', 'no_clear,no_scale'),
    CheckpointStat('checkpoint_scrub_total', 'scrub total time (msecs)', 'no_clear,no_scale'),
    CheckpointStat('checkpoint_skipped', 'checkpoints skipped because database was clean'),
    CheckpointStat('checkpoint_state', 'progress state', 'no_clear,no_scale'),
    CheckpointStat('checkpoint_stop_stress_active', 'stop timing stress active', 'no_clear,no_scale'),
    CheckpointStat('checkpoint_sync', 'number of files synced'),
    CheckpointStat('checkpoint_time_max', 'max time (msecs)', 'no_clear,no_scale'),
    CheckpointStat('checkpoint_time_min', 'min time (msecs)', 'no_clear,no_scale'),
    CheckpointStat('checkpoint_time_recent', 'most recent time (msecs)', 'no_clear,no_scale'),
    CheckpointStat('checkpoint_time_total', 'total time (msecs)', 'no_clear,no_scale'),
    CheckpointStat('checkpoint_tree_duration', 'time spent on per-tree checkpoint work (usecs)'),
    CheckpointStat('checkpoint_wait_reduce_dirty', 'wait cycles while cache dirty level is decreasing'),
    CheckpointStat('checkpoints_api', 'number of checkpoints started by api'),
    CheckpointStat('checkpoints_compact', 'number of checkpoints started by compaction'),
    CheckpointStat('checkpoints_total_failed', 'total failed number of checkpoints'),
    CheckpointStat('checkpoints_total_succeed', 'total succeed number of checkpoints'),

    ##########################################
    # Chunk cache statistics
    ##########################################
    ChunkCacheStat('chunkcache_bytes_inuse', 'total bytes used by the cache'),
    ChunkCacheStat('chunkcache_bytes_inuse_pinned', 'total bytes used by the cache for pinned chunks'),
    ChunkCacheStat('chunkcache_bytes_read_persistent', 'total bytes read from persistent content'),
    ChunkCacheStat('chunkcache_chunks_evicted', 'chunks evicted'),
    ChunkCacheStat('chunkcache_chunks_inuse', 'total chunks held by the chunk cache'),
    ChunkCacheStat('chunkcache_chunks_loaded_from_flushed_tables', 'number of chunks loaded from flushed tables in chunk cache'),
    ChunkCacheStat('chunkcache_chunks_pinned', 'total pinned chunks held by the chunk cache'),
    ChunkCacheStat('chunkcache_created_from_metadata', 'total number of chunks inserted on startup from persisted metadata.'),
    ChunkCacheStat('chunkcache_exceeded_bitmap_capacity', 'could not allocate due to exceeding bitmap capacity'),
    ChunkCacheStat('chunkcache_exceeded_capacity', 'could not allocate due to exceeding capacity'),
    ChunkCacheStat('chunkcache_io_failed', 'number of times a read from storage failed'),
    ChunkCacheStat('chunkcache_lookups', 'lookups'),
    ChunkCacheStat('chunkcache_metadata_inserted', 'number of metadata entries inserted'),
    ChunkCacheStat('chunkcache_metadata_removed', 'number of metadata entries removed'),
    ChunkCacheStat('chunkcache_metadata_work_units_created', 'number of metadata inserts/deletes pushed to the worker thread'),
    ChunkCacheStat('chunkcache_metadata_work_units_dequeued', 'number of metadata inserts/deletes read by the worker thread'),
    ChunkCacheStat('chunkcache_metadata_work_units_dropped', 'number of metadata inserts/deletes dropped by the worker thread'),
    ChunkCacheStat('chunkcache_misses', 'number of misses'),
    ChunkCacheStat('chunkcache_retries', 'retried accessing a chunk while I/O was in progress'),
    ChunkCacheStat('chunkcache_retries_checksum_mismatch', 'retries from a chunk cache checksum mismatch'),
    ChunkCacheStat('chunkcache_spans_chunks_read', 'aggregate number of spanned chunks on read'),
    ChunkCacheStat('chunkcache_toomany_retries', 'timed out due to too many retries'),

    ##########################################
    # Cursor operations
    ##########################################
    CursorStat('cursor_bulk_count', 'bulk cursor count', 'no_clear,no_scale'),
    CursorStat('cursor_cache', 'cursor close calls that result in cache'),
    CursorStat('cursor_cached_count', 'cached cursor count', 'no_clear,no_scale'),
    CursorStat('cursor_create', 'cursor create calls'),
    CursorStat('cursor_insert', 'cursor insert calls'),
    CursorStat('cursor_insert_bulk', 'cursor bulk loaded cursor insert calls'),
    CursorStat('cursor_insert_bytes', 'cursor insert key and value bytes', 'size'),
    CursorStat('cursor_modify', 'cursor modify calls'),
    CursorStat('cursor_modify_bytes', 'cursor modify key and value bytes affected', 'size'),
    CursorStat('cursor_modify_bytes_touch', 'cursor modify value bytes modified', 'size'),
    CursorStat('cursor_next', 'cursor next calls'),
    CursorStat('cursor_prev', 'cursor prev calls'),
    CursorStat('cursor_remove', 'cursor remove calls'),
    CursorStat('cursor_remove_bytes', 'cursor remove key bytes removed', 'size'),
    CursorStat('cursor_reopen', 'cursors reused from cache'),
    CursorStat('cursor_reserve', 'cursor reserve calls'),
    CursorStat('cursor_reset', 'cursor reset calls'),
    CursorStat('cursor_restart', 'cursor operation restarted'),
    CursorStat('cursor_search', 'cursor search calls'),
    CursorStat('cursor_search_hs', 'cursor search history store calls'),
    CursorStat('cursor_search_near', 'cursor search near calls'),
    CursorStat('cursor_truncate', 'cursor truncate calls'),
    CursorStat('cursor_truncate_keys_deleted', 'cursor truncates performed on individual keys'),
    CursorStat('cursor_update', 'cursor update calls'),
    CursorStat('cursor_update_bytes', 'cursor update key and value bytes', 'size'),
    CursorStat('cursor_update_bytes_changed', 'cursor update value size change', 'size'),

    ##########################################
    # Cursor sweep
    ##########################################
    CursorSweepStat('cursor_sweep', 'cursor sweeps'),
    CursorSweepStat('cursor_sweep_buckets', 'cursor sweep buckets'),
    CursorSweepStat('cursor_sweep_closed', 'cursor sweep cursors closed'),
    CursorSweepStat('cursor_sweep_examined', 'cursor sweep cursors examined'),

    ##########################################
    # Dhandle statistics
    ##########################################
    DhandleStat('dh_conn_handle_btree_count', 'btree connection data handles currently active', 'no_clear,no_scale'),
    DhandleStat('dh_conn_handle_checkpoint_count', 'checkpoint connection data handles currently active', 'no_clear,no_scale'),
    # dh_conn_handle_count = The sum of dh_conn_handle_{btree,table,tiered,tiered_tree}_count.
    DhandleStat('dh_conn_handle_count', 'connection data handles currently active', 'no_clear,no_scale'),
    DhandleStat('dh_conn_handle_size', 'connection data handle size', 'no_clear,no_scale,size'),
    DhandleStat('dh_conn_handle_table_count', 'Table connection data handles currently active', 'no_clear,no_scale'),
    DhandleStat('dh_conn_handle_tiered_count', 'Tiered connection data handles currently active', 'no_clear,no_scale'),
    DhandleStat('dh_conn_handle_tiered_tree_count', 'Tiered_Tree connection data handles currently active', 'no_clear,no_scale'),
    DhandleStat('dh_session_handles', 'session dhandles swept'),
    DhandleStat('dh_session_sweeps', 'session sweep attempts'),
    # dh_sweep_dead_close formerly called dh_sweep_close.
    DhandleStat('dh_sweep_dead_close', 'connection sweep dead dhandles closed'),
    DhandleStat('dh_sweep_expired_close', 'connection sweep expired dhandles closed'),
    # Note dh_sweep_total_close = dh_sweep_dead_close + dh_sweep_expired_close
    DhandleStat('dh_sweep_ref', 'connection sweep candidate became referenced'),
    DhandleStat('dh_sweep_remove', 'connection sweep dhandles removed from hash list'),
    DhandleStat('dh_sweep_skip_ckpt', 'connection sweeps skipped due to checkpoint gathering handles'),
    DhandleStat('dh_sweep_tod', 'connection sweep time-of-death sets'),
    DhandleStat('dh_sweeps', 'connection sweeps'),

    ##########################################
    # Live Restore statistics
    ##########################################
    LiveRestoreStat('live_restore_bytes_copied', 'number of bytes copied from the source to the destination', 'size'),
    LiveRestoreStat('live_restore_hist_source_read_latency_gt1000', 'source read latency histogram (bucket 7) - 1000ms+'),
    LiveRestoreStat('live_restore_hist_source_read_latency_lt10', 'source read latency histogram (bucket 1) - 0-10ms'),
    LiveRestoreStat('live_restore_hist_source_read_latency_lt50', 'source read latency histogram (bucket 2) - 10-49ms'),
    LiveRestoreStat('live_restore_hist_source_read_latency_lt100', 'source read latency histogram (bucket 3) - 50-99ms'),
    LiveRestoreStat('live_restore_hist_source_read_latency_lt250', 'source read latency histogram (bucket 4) - 100-249ms'),
    LiveRestoreStat('live_restore_hist_source_read_latency_lt500', 'source read latency histogram (bucket 5) - 250-499ms'),
    LiveRestoreStat('live_restore_hist_source_read_latency_lt1000', 'source read latency histogram (bucket 6) - 500-999ms'),
    LiveRestoreStat('live_restore_hist_source_read_latency_total_msecs', 'source read latency histogram total (msecs)'),
    LiveRestoreStat('live_restore_source_read_count', 'number of reads from the source database'),
    LiveRestoreStat('live_restore_state', 'state', 'no_clear,no_scale'),
    LiveRestoreStat('live_restore_work_remaining', 'number of files remaining for migration completion', 'no_clear,no_scale'),

    ##########################################
    # Locking statistics
    ##########################################
    LockStat('lock_btree_page_count', 'btree page lock acquisitions'),
    LockStat('lock_btree_page_wait_application', 'btree page lock application thread wait time (usecs)'),
    LockStat('lock_btree_page_wait_internal', 'btree page lock internal thread wait time (usecs)'),
    LockStat('lock_checkpoint_count', 'checkpoint lock acquisitions'),
    LockStat('lock_checkpoint_wait_application', 'checkpoint lock application thread wait time (usecs)'),
    LockStat('lock_checkpoint_wait_internal', 'checkpoint lock internal thread wait time (usecs)'),
    LockStat('lock_dhandle_read_count', 'dhandle read lock acquisitions'),
    LockStat('lock_dhandle_wait_application', 'dhandle lock application thread time waiting (usecs)'),
    LockStat('lock_dhandle_wait_internal', 'dhandle lock internal thread time waiting (usecs)'),
    LockStat('lock_dhandle_write_count', 'dhandle write lock acquisitions'),
    LockStat('lock_metadata_count', 'metadata lock acquisitions'),
    LockStat('lock_metadata_wait_application', 'metadata lock application thread wait time (usecs)'),
    LockStat('lock_metadata_wait_internal', 'metadata lock internal thread wait time (usecs)'),
    LockStat('lock_schema_count', 'schema lock acquisitions'),
    LockStat('lock_schema_wait_application', 'schema lock application thread wait time (usecs)'),
    LockStat('lock_schema_wait_internal', 'schema lock internal thread wait time (usecs)'),
    LockStat('lock_table_read_count', 'table read lock acquisitions'),
    LockStat('lock_table_wait_application', 'table lock application thread time waiting for the table lock (usecs)'),
    LockStat('lock_table_wait_internal', 'table lock internal thread time waiting for the table lock (usecs)'),
    LockStat('lock_table_write_count', 'table write lock acquisitions'),
    LockStat('lock_txn_global_read_count', 'txn global read lock acquisitions'),
    LockStat('lock_txn_global_wait_application', 'txn global lock application thread time waiting (usecs)'),
    LockStat('lock_txn_global_wait_internal', 'txn global lock internal thread time waiting (usecs)'),
    LockStat('lock_txn_global_write_count', 'txn global write lock acquisitions'),

    ##########################################
    # Logging statistics
    ##########################################
    LogStat('log_buffer_size', 'total log buffer size', 'no_clear,no_scale,size'),
    LogStat('log_bytes_payload', 'log bytes of payload data', 'size'),
    LogStat('log_bytes_written', 'log bytes written', 'size'),
    LogStat('log_close_yields', 'yields waiting for previous log file close'),
    LogStat('log_compress_len', 'total size of compressed records', 'size'),
    LogStat('log_compress_mem', 'total in-memory size of compressed records', 'size'),
    LogStat('log_compress_small', 'log records too small to compress'),
    LogStat('log_compress_write_fails', 'log records not compressed'),
    LogStat('log_compress_writes', 'log records compressed'),
    LogStat('log_flush', 'log flush operations'),
    LogStat('log_force_remove_sleep', 'force log remove time sleeping (usecs)'),
    LogStat('log_force_write', 'log force write operations'),
    LogStat('log_force_write_skip', 'log force write operations skipped'),
    LogStat('log_max_filesize', 'maximum log file size', 'no_clear,no_scale,size'),
    LogStat('log_prealloc_files', 'pre-allocated log files prepared'),
    LogStat('log_prealloc_max', 'number of pre-allocated log files to create', 'no_clear,no_scale'),
    LogStat('log_prealloc_missed', 'pre-allocated log files not ready and missed'),
    LogStat('log_prealloc_used', 'pre-allocated log files used'),
    LogStat('log_release_write_lsn', 'log release advances write LSN'),
    LogStat('log_scan_records', 'records processed by log scan'),
    LogStat('log_scan_rereads', 'log scan records requiring two reads'),
    LogStat('log_scans', 'log scan operations'),
    LogStat('log_slot_active_closed', 'slot join found active slot closed'),
    LogStat('log_slot_close_race', 'slot close lost race'),
    LogStat('log_slot_close_unbuf', 'slot close unbuffered waits'),
    LogStat('log_slot_closes', 'slot closures'),
    LogStat('log_slot_coalesced', 'written slots coalesced'),
    LogStat('log_slot_consolidated', 'logging bytes consolidated', 'size'),
    LogStat('log_slot_immediate', 'slot join calls did not yield'),
    LogStat('log_slot_no_free_slots', 'slot transitions unable to find free slot'),
    LogStat('log_slot_races', 'slot join atomic update races'),
    LogStat('log_slot_switch_busy', 'busy returns attempting to switch slots'),
    LogStat('log_slot_unbuffered', 'slot unbuffered writes'),
    LogStat('log_slot_yield', 'slot join calls yielded'),
    LogStat('log_slot_yield_close', 'slot join calls found active slot closed'),
    LogStat('log_slot_yield_duration', 'slot joins yield time (usecs)', 'no_clear,no_scale'),
    LogStat('log_slot_yield_race', 'slot join calls atomic updates raced'),
    LogStat('log_slot_yield_sleep', 'slot join calls slept'),
    LogStat('log_sync', 'log sync operations'),
    LogStat('log_sync_dir', 'log sync_dir operations'),
    LogStat('log_sync_dir_duration', 'log sync_dir time duration (usecs)', 'no_clear,no_scale'),
    LogStat('log_sync_duration', 'log sync time duration (usecs)', 'no_clear,no_scale'),
    LogStat('log_write_lsn', 'log server thread advances write LSN'),
    LogStat('log_write_lsn_skip', 'log server thread write LSN walk skipped'),
    LogStat('log_writes', 'log write operations'),
    LogStat('log_zero_fills', 'log files manually zero-filled'),

    ##########################################
    # Performance Histogram Stats
    ##########################################
    PerfHistStat('perf_hist_bmread_latency_gt1000', 'block manager read latency histogram (bucket 7) - 1000ms+'),
    PerfHistStat('perf_hist_bmread_latency_lt10', 'block manager read latency histogram (bucket 1) - 0-10ms'),
    PerfHistStat('perf_hist_bmread_latency_lt50', 'block manager read latency histogram (bucket 2) - 10-49ms'),
    PerfHistStat('perf_hist_bmread_latency_lt100', 'block manager read latency histogram (bucket 3) - 50-99ms'),
    PerfHistStat('perf_hist_bmread_latency_lt250', 'block manager read latency histogram (bucket 4) - 100-249ms'),
    PerfHistStat('perf_hist_bmread_latency_lt500', 'block manager read latency histogram (bucket 5) - 250-499ms'),
    PerfHistStat('perf_hist_bmread_latency_lt1000', 'block manager read latency histogram (bucket 6) - 500-999ms'),
    PerfHistStat('perf_hist_bmread_latency_total_msecs', 'block manager read latency histogram total (msecs)'),
    PerfHistStat('perf_hist_bmwrite_latency_gt1000', 'block manager write latency histogram (bucket 7) - 1000ms+'),
    PerfHistStat('perf_hist_bmwrite_latency_lt10', 'block manager write latency histogram (bucket 1) - 0-10ms'),
    PerfHistStat('perf_hist_bmwrite_latency_lt50', 'block manager write latency histogram (bucket 2) - 10-49ms'),
    PerfHistStat('perf_hist_bmwrite_latency_lt100', 'block manager write latency histogram (bucket 3) - 50-99ms'),
    PerfHistStat('perf_hist_bmwrite_latency_lt250', 'block manager write latency histogram (bucket 4) - 100-249ms'),
    PerfHistStat('perf_hist_bmwrite_latency_lt500', 'block manager write latency histogram (bucket 5) - 250-499ms'),
    PerfHistStat('perf_hist_bmwrite_latency_lt1000', 'block manager write latency histogram (bucket 6) - 500-999ms'),
    PerfHistStat('perf_hist_bmwrite_latency_total_msecs', 'block manager write latency histogram total (msecs)'),
    PerfHistStat('perf_hist_disaggbmread_latency_gt10000', 'disagg block manager read latency histogram (bucket 6) - 10000us+'),
    PerfHistStat('perf_hist_disaggbmread_latency_lt100', 'disagg block manager read latency histogram (bucket 1) - 50-99us'),
    PerfHistStat('perf_hist_disaggbmread_latency_lt250', 'disagg block manager read latency histogram (bucket 2) - 100-249us'),
    PerfHistStat('perf_hist_disaggbmread_latency_lt500', 'disagg block manager read latency histogram (bucket 3) - 250-499us'),
    PerfHistStat('perf_hist_disaggbmread_latency_lt1000', 'disagg block manager read latency histogram (bucket 4) - 500-999us'),
    PerfHistStat('perf_hist_disaggbmread_latency_lt10000', 'disagg block manager read latency histogram (bucket 5) - 1000-9999us'),
    PerfHistStat('perf_hist_disaggbmread_latency_total_usecs', 'disagg block manager read latency histogram total (usecs)'),
    PerfHistStat('perf_hist_disaggbmwrite_latency_gt10000', 'disagg block manager write latency histogram (bucket 6) - 10000us+'),
    PerfHistStat('perf_hist_disaggbmwrite_latency_lt100', 'disagg block manager write latency histogram (bucket 1) - 50-99us'),
    PerfHistStat('perf_hist_disaggbmwrite_latency_lt250', 'disagg block manager write latency histogram (bucket 2) - 100-249us'),
    PerfHistStat('perf_hist_disaggbmwrite_latency_lt500', 'disagg block manager write latency histogram (bucket 3) - 250-499us'),
    PerfHistStat('perf_hist_disaggbmwrite_latency_lt1000', 'disagg block manager write latency histogram (bucket 4) - 500-999us'),
    PerfHistStat('perf_hist_disaggbmwrite_latency_lt10000', 'disagg block manager write latency histogram (bucket 5) - 1000-9999us'),
    PerfHistStat('perf_hist_disaggbmwrite_latency_total_usecs', 'disagg block manager write latency histogram total (usecs)'),
    PerfHistStat('perf_hist_fsread_latency_gt1000', 'file system read latency histogram (bucket 7) - 1000ms+'),
    PerfHistStat('perf_hist_fsread_latency_lt10', 'file system read latency histogram (bucket 1) - 0-10ms'),
    PerfHistStat('perf_hist_fsread_latency_lt50', 'file system read latency histogram (bucket 2) - 10-49ms'),
    PerfHistStat('perf_hist_fsread_latency_lt100', 'file system read latency histogram (bucket 3) - 50-99ms'),
    PerfHistStat('perf_hist_fsread_latency_lt250', 'file system read latency histogram (bucket 4) - 100-249ms'),
    PerfHistStat('perf_hist_fsread_latency_lt500', 'file system read latency histogram (bucket 5) - 250-499ms'),
    PerfHistStat('perf_hist_fsread_latency_lt1000', 'file system read latency histogram (bucket 6) - 500-999ms'),
    PerfHistStat('perf_hist_fsread_latency_total_msecs', 'file system read latency histogram total (msecs)'),
    PerfHistStat('perf_hist_fswrite_latency_gt1000', 'file system write latency histogram (bucket 7) - 1000ms+'),
    PerfHistStat('perf_hist_fswrite_latency_lt10', 'file system write latency histogram (bucket 1) - 0-10ms'),
    PerfHistStat('perf_hist_fswrite_latency_lt50', 'file system write latency histogram (bucket 2) - 10-49ms'),
    PerfHistStat('perf_hist_fswrite_latency_lt100', 'file system write latency histogram (bucket 3) - 50-99ms'),
    PerfHistStat('perf_hist_fswrite_latency_lt250', 'file system write latency histogram (bucket 4) - 100-249ms'),
    PerfHistStat('perf_hist_fswrite_latency_lt500', 'file system write latency histogram (bucket 5) - 250-499ms'),
    PerfHistStat('perf_hist_fswrite_latency_lt1000', 'file system write latency histogram (bucket 6) - 500-999ms'),
    PerfHistStat('perf_hist_fswrite_latency_total_msecs', 'file system write latency histogram total (msecs)'),
    PerfHistStat('perf_hist_opread_latency_gt10000', 'operation read latency histogram (bucket 6) - 10000us+'),
    PerfHistStat('perf_hist_opread_latency_lt100', 'operation read latency histogram (bucket 1) - 0-100us'),
    PerfHistStat('perf_hist_opread_latency_lt250', 'operation read latency histogram (bucket 2) - 100-249us'),
    PerfHistStat('perf_hist_opread_latency_lt500', 'operation read latency histogram (bucket 3) - 250-499us'),
    PerfHistStat('perf_hist_opread_latency_lt1000', 'operation read latency histogram (bucket 4) - 500-999us'),
    PerfHistStat('perf_hist_opread_latency_lt10000', 'operation read latency histogram (bucket 5) - 1000-9999us'),
    PerfHistStat('perf_hist_opread_latency_total_usecs', 'operation read latency histogram total (usecs)'),
    PerfHistStat('perf_hist_opwrite_latency_gt10000', 'operation write latency histogram (bucket 6) - 10000us+'),
    PerfHistStat('perf_hist_opwrite_latency_lt100', 'operation write latency histogram (bucket 1) - 0-100us'),
    PerfHistStat('perf_hist_opwrite_latency_lt250', 'operation write latency histogram (bucket 2) - 100-249us'),
    PerfHistStat('perf_hist_opwrite_latency_lt500', 'operation write latency histogram (bucket 3) - 250-499us'),
    PerfHistStat('perf_hist_opwrite_latency_lt1000', 'operation write latency histogram (bucket 4) - 500-999us'),
    PerfHistStat('perf_hist_opwrite_latency_lt10000', 'operation write latency histogram (bucket 5) - 1000-9999us'),
    PerfHistStat('perf_hist_opwrite_latency_total_usecs', 'operation write latency histogram total (usecs)'),

    ##########################################
    # Prefetch statistics
    ##########################################
    PrefetchStat('prefetch_attempts', 'pre-fetch triggered by page read'),
    PrefetchStat('prefetch_disk_one', 'pre-fetch not triggered after single disk read'),
    PrefetchStat('prefetch_failed_start', 'number of times pre-fetch failed to start'),
    PrefetchStat('prefetch_pages_fail', 'pre-fetch page not on disk when reading'),
    PrefetchStat('prefetch_pages_queued', 'pre-fetch pages queued'),
    PrefetchStat('prefetch_pages_read', 'pre-fetch pages read in background'),
    PrefetchStat('prefetch_skipped', 'pre-fetch not triggered by page read'),
    PrefetchStat('prefetch_skipped_disk_read_count', 'pre-fetch not triggered due to disk read count'),
    PrefetchStat('prefetch_skipped_error_ok', 'pre-fetch skipped reading in a page due to harmless error'),
    PrefetchStat('prefetch_skipped_internal_page', 'could not perform pre-fetch on internal page'),
    PrefetchStat('prefetch_skipped_internal_session', 'pre-fetch not triggered due to internal session'),
    PrefetchStat('prefetch_skipped_no_flag_set', 'could not perform pre-fetch on ref without the pre-fetch flag set'),
    PrefetchStat('prefetch_skipped_no_valid_dhandle', 'pre-fetch not triggered as there is no valid dhandle'),
    PrefetchStat('prefetch_skipped_same_ref', 'pre-fetch not repeating for recently pre-fetched ref'),
    PrefetchStat('prefetch_skipped_special_handle', 'pre-fetch not triggered due to special btree handle'),

    ##########################################
    # Reconciliation statistics
    ##########################################
    RecStat('rec_maximum_hs_wrapup_milliseconds', 'maximum milliseconds spent in moving updates to the history store in a reconciliation', 'no_clear,no_scale,size'),
    RecStat('rec_maximum_image_build_milliseconds', 'maximum milliseconds spent in building a disk image in a reconciliation', 'no_clear,no_scale,size'),
    RecStat('rec_maximum_milliseconds', 'maximum milliseconds spent in a reconciliation call', 'no_clear,no_scale,size'),
    RecStat('rec_pages_with_prepare', 'page reconciliation calls that resulted in values with prepared transaction metadata'),
    RecStat('rec_pages_with_ts', 'page reconciliation calls that resulted in values with timestamps'),
    RecStat('rec_pages_with_txn', 'page reconciliation calls that resulted in values with transaction ids'),
    RecStat('rec_split_stashed_bytes', 'split bytes currently awaiting free', 'no_clear,no_scale,size'),
    RecStat('rec_split_stashed_objects', 'split objects currently awaiting free', 'no_clear,no_scale'),
    RecStat('rec_time_window_pages_prepared', 'pages written including at least one prepare state'),
    RecStat('rec_time_window_pages_start_ts', 'pages written including at least one start timestamp'),
    RecStat('rec_time_window_prepared', 'records written including a prepare state'),

    ##########################################
    # Session operations
    ##########################################
    SessionOpStat('session_open', 'open session count', 'no_clear,no_scale'),
    SessionOpStat('session_query_ts', 'session query timestamp calls'),
    SessionOpStat('session_table_alter_fail', 'table alter failed calls', 'no_clear,no_scale'),
    SessionOpStat('session_table_alter_skip', 'table alter unchanged and skipped', 'no_clear,no_scale'),
    SessionOpStat('session_table_alter_success', 'table alter successful calls', 'no_clear,no_scale'),
    SessionOpStat('session_table_alter_trigger_checkpoint', 'table alter triggering checkpoint calls', 'no_clear,no_scale'),
    SessionOpStat('session_table_compact_conflicting_checkpoint', 'table compact conflicted with checkpoint', 'no_clear,no_scale'),
    SessionOpStat('session_table_compact_dhandle_success', 'table compact dhandle successful calls', 'no_scale'),
    SessionOpStat('session_table_compact_eviction', 'table compact pulled into eviction', 'no_clear,no_scale'),
    SessionOpStat('session_table_compact_fail', 'table compact failed calls', 'no_clear,no_scale'),
    SessionOpStat('session_table_compact_fail_cache_pressure', 'table compact failed calls due to cache pressure', 'no_clear,no_scale'),
    SessionOpStat('session_table_compact_passes', 'table compact passes', 'no_scale'),
    SessionOpStat('session_table_compact_running', 'table compact running', 'no_clear,no_scale'),
    SessionOpStat('session_table_compact_skipped', 'table compact skipped as process would not reduce file size', 'no_clear,no_scale'),
    SessionOpStat('session_table_compact_success', 'table compact successful calls', 'no_clear,no_scale'),
    SessionOpStat('session_table_compact_timeout', 'table compact timeout', 'no_clear,no_scale'),
    SessionOpStat('session_table_create_fail', 'table create failed calls', 'no_clear,no_scale'),
    SessionOpStat('session_table_create_import_fail', 'table create with import failed calls', 'no_clear,no_scale'),
    SessionOpStat('session_table_create_import_repair', 'table create with import repair calls', 'no_clear,no_scale'),
    SessionOpStat('session_table_create_import_success', 'table create with import successful calls', 'no_clear,no_scale'),
    SessionOpStat('session_table_create_success', 'table create successful calls', 'no_clear,no_scale'),
    SessionOpStat('session_table_drop_fail', 'table drop failed calls', 'no_clear,no_scale'),
    SessionOpStat('session_table_drop_success', 'table drop successful calls', 'no_clear,no_scale'),
    SessionOpStat('session_table_salvage_fail', 'table salvage failed calls', 'no_clear,no_scale'),
    SessionOpStat('session_table_salvage_success', 'table salvage successful calls', 'no_clear,no_scale'),
    SessionOpStat('session_table_truncate_fail', 'table truncate failed calls', 'no_clear,no_scale'),
    SessionOpStat('session_table_truncate_success', 'table truncate successful calls', 'no_clear,no_scale'),
    SessionOpStat('session_table_verify_fail', 'table verify failed calls', 'no_clear,no_scale'),
    SessionOpStat('session_table_verify_success', 'table verify successful calls', 'no_clear,no_scale'),

    ##########################################
    # Tiered storage statistics
    ##########################################
    StorageStat('flush_tier', 'flush_tier operation calls'),
    StorageStat('flush_tier_fail', 'flush_tier failed calls'),
    StorageStat('flush_tier_skipped', 'flush_tier tables skipped due to no checkpoint'),
    StorageStat('flush_tier_switched', 'flush_tier tables switched'),
    StorageStat('local_objects_inuse', 'attempts to remove a local object and the object is in use'),
    StorageStat('local_objects_removed', 'local objects removed'),
    StorageStat('tiered_retention', 'tiered storage local retention time (secs)', 'no_clear,no_scale,size'),
    StorageStat('tiered_work_units_created', 'tiered operations scheduled'),
    StorageStat('tiered_work_units_dequeued', 'tiered operations dequeued and processed'),
    StorageStat('tiered_work_units_removed', 'tiered operations removed without processing'),

    ##########################################
    # Thread Count statistics
    ##########################################
    ThreadStat('thread_fsync_active', 'active filesystem fsync calls','no_clear,no_scale'),
    ThreadStat('thread_read_active', 'active filesystem read calls','no_clear,no_scale'),
    ThreadStat('thread_write_active', 'active filesystem write calls','no_clear,no_scale'),

    ##########################################
    # Transaction statistics
    ##########################################
    TxnStat('txn_begin', 'transaction begins'),
    TxnStat('txn_commit', 'transactions committed'),
    TxnStat('txn_hs_ckpt_duration', 'transaction checkpoint history store file duration (usecs)'),
    TxnStat('txn_pinned_checkpoint_range', 'transaction range of IDs currently pinned by a checkpoint', 'no_clear,no_scale'),
    TxnStat('txn_pinned_range', 'transaction range of IDs currently pinned', 'no_clear,no_scale'),
    TxnStat('txn_pinned_timestamp', 'transaction range of timestamps currently pinned', 'no_clear,no_scale'),
    TxnStat('txn_pinned_timestamp_checkpoint', 'transaction range of timestamps pinned by a checkpoint', 'no_clear,no_scale'),
    TxnStat('txn_pinned_timestamp_oldest', 'transaction range of timestamps pinned by the oldest timestamp', 'no_clear,no_scale'),
    TxnStat('txn_pinned_timestamp_reader', 'transaction range of timestamps pinned by the oldest active read timestamp', 'no_clear,no_scale'),
    TxnStat('txn_prepare', 'prepared transactions'),
    TxnStat('txn_prepare_active', 'prepared transactions currently active'),
    TxnStat('txn_prepare_commit', 'prepared transactions committed'),
    TxnStat('txn_prepare_rollback', 'prepared transactions rolled back'),
    TxnStat('txn_prepared_updates', 'Number of prepared updates'),
    TxnStat('txn_prepared_updates_committed', 'Number of prepared updates committed'),
    TxnStat('txn_prepared_updates_key_repeated', 'Number of prepared updates repeated on the same key'),
    TxnStat('txn_prepared_updates_rolledback', 'Number of prepared updates rolled back'),
    TxnStat('txn_query_ts', 'query timestamp calls'),
    TxnStat('txn_rollback', 'transactions rolled back'),
    TxnStat('txn_rollback_oldest_id', 'oldest transaction ID rolled back for eviction'),
    TxnStat('txn_rollback_oldest_pinned', 'oldest pinned transaction ID rolled back for eviction'),
    TxnStat('txn_rollback_to_stable_running', 'transaction rollback to stable currently running', 'no_clear,no_scale'),
    TxnStat('txn_rts', 'rollback to stable calls'),
    TxnStat('txn_rts_pages_visited', 'rollback to stable pages visited'),
    TxnStat('txn_rts_tree_walk_skip_pages', 'rollback to stable tree walk skipping pages'),
    TxnStat('txn_rts_upd_aborted', 'rollback to stable updates aborted'),
    TxnStat('txn_rts_upd_aborted_dryrun', 'rollback to stable updates that would have been aborted in non-dryrun mode'),
    TxnStat('txn_sessions_walked', 'sessions scanned in each walk of concurrent sessions'),
    TxnStat('txn_set_ts', 'set timestamp calls'),
    TxnStat('txn_set_ts_durable', 'set timestamp durable calls'),
    TxnStat('txn_set_ts_durable_upd', 'set timestamp durable updates'),
    TxnStat('txn_set_ts_force', 'set timestamp force calls'),
    TxnStat('txn_set_ts_oldest', 'set timestamp oldest calls'),
    TxnStat('txn_set_ts_oldest_upd', 'set timestamp oldest updates'),
    TxnStat('txn_set_ts_out_of_order', 'set timestamp global oldest timestamp set to be more recent than the global stable timestamp'),
    TxnStat('txn_set_ts_stable', 'set timestamp stable calls'),
    TxnStat('txn_set_ts_stable_upd', 'set timestamp stable updates'),
    TxnStat('txn_timestamp_oldest_active_read', 'transaction read timestamp of the oldest active reader', 'no_clear,no_scale'),
    TxnStat('txn_walk_sessions', 'transaction walk of concurrent sessions'),

    ##########################################
    # Yield statistics
    ##########################################
    YieldStat('application_cache_interruptible_ops', 'application thread operations waiting for interruptible cache eviction'),
    YieldStat('application_cache_interruptible_time', 'application thread time waiting for interruptible cache eviction (usecs)'),
    YieldStat('application_cache_ops', 'application thread operations waiting for cache'),
    YieldStat('application_cache_time', 'application thread time waiting for cache (usecs)'),
    YieldStat('application_cache_uninterruptible_ops', 'application thread operations waiting for mandatory cache eviction'),
    YieldStat('application_cache_uninterruptible_time', 'application thread time waiting for mandatory cache eviction (usecs)'),
    YieldStat('application_evict_snapshot_refreshed', 'application thread snapshot refreshed for eviction'),
    YieldStat('child_modify_blocked_page', 'page reconciliation yielded due to child modification'),
    YieldStat('dhandle_lock_blocked', 'data handle lock yielded'),
    YieldStat('page_busy_blocked', 'page acquire busy blocked'),
    YieldStat('page_del_rollback_blocked', 'page delete rollback time sleeping for state change (usecs)'),
    YieldStat('page_forcible_evict_blocked', 'page acquire eviction blocked'),
    YieldStat('page_index_slot_ref_blocked', 'get reference for page index and slot time sleeping (usecs)'),
    YieldStat('page_locked_blocked', 'page acquire locked blocked'),
    YieldStat('page_read_blocked', 'page acquire read blocked'),
    YieldStat('page_read_skip_deleted', 'pages skipped during read due to deleted state'),
    YieldStat('page_sleep', 'page acquire time sleeping (usecs)'),
    YieldStat('page_split_restart', 'page split and restart read'),
    YieldStat('prepared_transition_blocked_page', 'page access yielded due to prepare state change'),
    YieldStat('txn_release_blocked', 'connection close blocked waiting for transaction state stabilization'),
]

##########################################
# Data source statistics
##########################################
dsrc_stats = [
    ##########################################
    # Block manager statistics
    ##########################################
    BlockStat('allocation_size', 'file allocation unit size', 'max_aggregate,no_scale,size'),
    BlockStat('block_alloc', 'blocks allocated'),
    BlockStat('block_checkpoint_size', 'checkpoint size', 'no_scale,size'),
    BlockStat('block_extension', 'allocations requiring file extension'),
    BlockStat('block_free', 'blocks freed'),
    BlockStat('block_magic', 'file magic number', 'max_aggregate,no_scale'),
    BlockStat('block_major', 'file major version number', 'max_aggregate,no_scale'),
    BlockStat('block_minor', 'minor version number', 'max_aggregate,no_scale'),
    BlockStat('block_reuse_bytes', 'file bytes available for reuse', 'no_scale,size'),
    BlockStat('block_size', 'file size in bytes', 'no_scale,size'),

    ##########################################
    # Btree statistics
    ##########################################
    BtreeStat('btree_checkpoint_generation', 'btree checkpoint generation', 'no_clear,no_scale'),
    BtreeStat('btree_checkpoint_pages_reconciled', 'btree number of pages reconciled during checkpoint', 'no_clear,no_scale'),
    BtreeStat('btree_clean_checkpoint_timer', 'btree clean tree checkpoint expiration time', 'no_clear,no_scale'),
    BtreeStat('btree_column_deleted', 'column-store variable-size deleted values', 'no_scale,tree_walk'),
    BtreeStat('btree_column_fix', 'column-store fixed-size leaf pages', 'no_scale,tree_walk'),
    BtreeStat('btree_column_internal', 'column-store internal pages', 'no_scale,tree_walk'),
    BtreeStat('btree_column_rle', 'column-store variable-size RLE encoded values', 'no_scale,tree_walk'),
    BtreeStat('btree_column_tws', 'column-store fixed-size time windows', 'no_scale,tree_walk'),
    BtreeStat('btree_column_variable', 'column-store variable-size leaf pages', 'no_scale,tree_walk'),
    BtreeStat('btree_compact_bytes_rewritten_expected', 'btree expected number of compact bytes rewritten', 'no_clear,no_scale'),
    BtreeStat('btree_compact_pages_reviewed', 'btree compact pages reviewed', 'no_clear,no_scale'),
    BtreeStat('btree_compact_pages_rewritten', 'btree compact pages rewritten', 'no_clear,no_scale'),
    BtreeStat('btree_compact_pages_rewritten_expected', 'btree expected number of compact pages rewritten', 'no_clear,no_scale'),
    BtreeStat('btree_compact_pages_skipped', 'btree compact pages skipped', 'no_clear,no_scale'),
    BtreeStat('btree_compact_skipped', 'btree skipped by compaction as process would not reduce size', 'no_clear,no_scale'),
    BtreeStat('btree_entries', 'number of key/value pairs', 'no_scale,tree_walk'),
    BtreeStat('btree_fixed_len', 'fixed-record size', 'max_aggregate,no_scale,size'),
    BtreeStat('btree_maximum_depth', 'maximum tree depth', 'max_aggregate,no_scale'),
    BtreeStat('btree_maxintlpage', 'maximum internal page size', 'max_aggregate,no_scale,size'),
    BtreeStat('btree_maxleafkey', 'maximum leaf page key size', 'max_aggregate,no_scale,size'),
    BtreeStat('btree_maxleafpage', 'maximum leaf page size', 'max_aggregate,no_scale,size'),
    BtreeStat('btree_maxleafvalue', 'maximum leaf page value size', 'max_aggregate,no_scale,size'),
    BtreeStat('btree_overflow', 'overflow pages', 'no_scale,tree_walk'),
    BtreeStat('btree_row_empty_values', 'row-store empty values', 'no_scale,tree_walk'),
    BtreeStat('btree_row_internal', 'row-store internal pages', 'no_scale,tree_walk'),
    BtreeStat('btree_row_leaf', 'row-store leaf pages', 'no_scale,tree_walk'),

    ##########################################
    # Eviction statistics
    ##########################################
    EvictStat('eviction_fail', 'data source pages selected for eviction unable to be evicted'),
    EvictStat('eviction_walk_passes', 'eviction walk passes of a file'),

    ##########################################
    # Cache content statistics
    ##########################################
    EvictCacheWalkStat('cache_state_avg_unvisited_age', 'Average time in cache for pages that have not been visited by the eviction server', 'no_clear,no_scale'),
    EvictCacheWalkStat('cache_state_avg_visited_age', 'Average time in cache for pages that have been visited by the eviction server', 'no_clear,no_scale'),
    EvictCacheWalkStat('cache_state_avg_written_size', 'Average on-disk page image size seen', 'no_clear,no_scale'),
    EvictCacheWalkStat('cache_state_gen_avg_gap', 'Average difference between current eviction generation when the page was last considered', 'no_clear,no_scale'),
    EvictCacheWalkStat('cache_state_gen_current', 'Current eviction generation', 'no_clear,no_scale'),
    EvictCacheWalkStat('cache_state_gen_max_gap', 'Maximum difference between current eviction generation when the page was last considered', 'no_clear,no_scale'),
    EvictCacheWalkStat('cache_state_max_pagesize', 'Maximum page size seen', 'no_clear,no_scale'),
    EvictCacheWalkStat('cache_state_memory', 'Pages created in memory and never written', 'no_clear,no_scale'),
    EvictCacheWalkStat('cache_state_min_written_size', 'Minimum on-disk page image size seen', 'no_clear,no_scale'),
    EvictCacheWalkStat('cache_state_not_queueable', 'Pages that could not be queued for eviction', 'no_clear,no_scale'),
    EvictCacheWalkStat('cache_state_pages', 'Total number of pages currently in cache', 'no_clear,no_scale'),
    EvictCacheWalkStat('cache_state_pages_clean', 'Clean pages currently in cache', 'no_clear,no_scale'),
    EvictCacheWalkStat('cache_state_pages_dirty', 'Dirty pages currently in cache', 'no_clear,no_scale'),
    EvictCacheWalkStat('cache_state_pages_internal', 'Internal pages currently in cache', 'no_clear,no_scale'),
    EvictCacheWalkStat('cache_state_pages_leaf', 'Leaf pages currently in cache', 'no_clear,no_scale'),
    EvictCacheWalkStat('cache_state_queued', 'Pages currently queued for eviction', 'no_clear,no_scale'),
    EvictCacheWalkStat('cache_state_refs_skipped', 'Refs skipped during cache traversal', 'no_clear,no_scale'),
    EvictCacheWalkStat('cache_state_root_entries', 'Entries in the root page', 'no_clear,no_scale'),
    EvictCacheWalkStat('cache_state_root_size', 'Size of the root page', 'no_clear,no_scale'),
    EvictCacheWalkStat('cache_state_smaller_alloc_size', 'On-disk page image sizes smaller than a single allocation unit', 'no_clear,no_scale'),
    EvictCacheWalkStat('cache_state_unvisited_count', 'Number of pages never visited by eviction server', 'no_clear,no_scale'),

    ##########################################
    # Compression statistics
    ##########################################
    CompressStat('compress_precomp_intl_max_page_size', 'compressed page maximum internal page size prior to compression', 'no_clear,no_scale,size'),
    CompressStat('compress_precomp_leaf_max_page_size', 'compressed page maximum leaf page size prior to compression ', 'no_clear,no_scale,size'),
    CompressStat('compress_read', 'pages read from disk'),
    # dist/stat.py sorts stats by their descriptions and not their names. The following stat descriptions insert an extra
    # space before the single digit numbers (2, 4, 8) so stats will be sorted numerically (2, 4, 8, 16, 32) instead of
    # alphabetically (16, 2, 32, 4, 8).
    CompressStat('compress_read_ratio_hist_2', 'pages read from disk with compression ratio smaller than  2'),
    CompressStat('compress_read_ratio_hist_4', 'pages read from disk with compression ratio smaller than  4'),
    CompressStat('compress_read_ratio_hist_8', 'pages read from disk with compression ratio smaller than  8'),
    CompressStat('compress_read_ratio_hist_16', 'pages read from disk with compression ratio smaller than 16'),
    CompressStat('compress_read_ratio_hist_32', 'pages read from disk with compression ratio smaller than 32'),
    CompressStat('compress_read_ratio_hist_64', 'pages read from disk with compression ratio smaller than 64'),
    CompressStat('compress_read_ratio_hist_max', 'pages read from disk with compression ratio greater than 64'),
    CompressStat('compress_write', 'pages written to disk'),
    CompressStat('compress_write_fail', 'page written to disk failed to compress'),
    CompressStat('compress_write_ratio_hist_2', 'pages written to disk with compression ratio smaller than  2'),
    CompressStat('compress_write_ratio_hist_4', 'pages written to disk with compression ratio smaller than  4'),
    CompressStat('compress_write_ratio_hist_8', 'pages written to disk with compression ratio smaller than  8'),
    CompressStat('compress_write_ratio_hist_16', 'pages written to disk with compression ratio smaller than 16'),
    CompressStat('compress_write_ratio_hist_32', 'pages written to disk with compression ratio smaller than 32'),
    CompressStat('compress_write_ratio_hist_64', 'pages written to disk with compression ratio smaller than 64'),
    CompressStat('compress_write_ratio_hist_max', 'pages written to disk with compression ratio greater than 64'),
    CompressStat('compress_write_too_small', 'page written to disk was too small to compress'),

    ##########################################
    # Cursor operations
    ##########################################
    CursorStat('cursor_cache', 'close calls that result in cache'),
    CursorStat('cursor_create', 'create calls'),
    CursorStat('cursor_insert', 'insert calls'),
    CursorStat('cursor_insert_bulk', 'bulk loaded cursor insert calls'),
    CursorStat('cursor_insert_bytes', 'insert key and value bytes', 'size'),
    CursorStat('cursor_modify', 'modify'),
    CursorStat('cursor_modify_bytes', 'modify key and value bytes affected', 'size'),
    CursorStat('cursor_modify_bytes_touch', 'modify value bytes modified', 'size'),
    CursorStat('cursor_next', 'next calls'),
    CursorStat('cursor_prev', 'prev calls'),
    CursorStat('cursor_remove', 'remove calls'),
    CursorStat('cursor_remove_bytes', 'remove key bytes removed', 'size'),
    CursorStat('cursor_reopen', 'cache cursors reuse count'),
    CursorStat('cursor_reserve', 'reserve calls'),
    CursorStat('cursor_reset', 'reset calls'),
    CursorStat('cursor_restart', 'operation restarted'),
    CursorStat('cursor_search', 'search calls'),
    CursorStat('cursor_search_hs', 'search history store calls'),
    CursorStat('cursor_search_near', 'search near calls'),
    CursorStat('cursor_truncate', 'truncate calls'),
    CursorStat('cursor_update', 'update calls'),
    CursorStat('cursor_update_bytes', 'update key and value bytes', 'size'),
    CursorStat('cursor_update_bytes_changed', 'update value size change', 'size'),

    ##########################################
    # Reconciliation statistics
    ##########################################
    RecStat('rec_dictionary', 'dictionary matches'),
    RecStat('rec_multiblock_max', 'maximum blocks required for a page', 'max_aggregate,no_scale'),
    RecStat('rec_prefix_compression', 'leaf page key bytes discarded using prefix compression', 'size'),
    RecStat('rec_suffix_compression', 'internal page key bytes discarded using suffix compression', 'size'),
    RecStat('rec_time_window_pages_prepared', 'pages written including at least one prepare'),
    RecStat('rec_time_window_pages_start_ts', 'pages written including at least one start timestamp'),
    RecStat('rec_time_window_prepared', 'records written including a prepare'),

    ##########################################
    # Session operations
    ##########################################
    SessionOpStat('session_compact', 'object compaction'),
]

##########################################
# CONNECTION AND DATA SOURCE statistics
##########################################
conn_dsrc_stats = [
    ##########################################
    # Autocommit statistics
    ##########################################
    AutoCommitStat('autocommit_readonly_retry', 'retries for readonly operations'),
    AutoCommitStat('autocommit_update_retry', 'retries for update operations'),

    ##########################################
    # Backup statistics
    ##########################################
    BackupStat('backup_blocks_compressed', 'total modified incremental blocks with compressed data'),
    BackupStat('backup_blocks_uncompressed', 'total modified incremental blocks without compressed data'),

    ##########################################
    # Cache and eviction statistics
    ##########################################
    CacheStat('cache_bytes_dirty', 'tracked dirty bytes in the cache', 'no_clear,no_scale,size'),
    CacheStat('cache_bytes_dirty_internal', 'tracked dirty internal page bytes in the cache', 'no_clear,no_scale,size'),
    CacheStat('cache_bytes_dirty_leaf', 'tracked dirty leaf page bytes in the cache', 'no_clear,no_scale,size'),
    CacheStat('cache_bytes_dirty_total', 'bytes dirty in the cache cumulative', 'no_clear,no_scale,size'),
    CacheStat('cache_bytes_inuse', 'bytes currently in the cache', 'no_clear,no_scale,size'),
    CacheStat('cache_bytes_read', 'bytes read into cache', 'size'),
    CacheStat('cache_bytes_write', 'bytes written from cache', 'size'),
    CacheStat('cache_eviction_app_threads_fill_ratio_25_50', 'application threads eviction requested with cache fill ratio >= 25% and < 50%'),
    CacheStat('cache_eviction_app_threads_fill_ratio_50_75', 'application threads eviction requested with cache fill ratio >= 50% and < 75%'),
    CacheStat('cache_eviction_app_threads_fill_ratio_gt_75', 'application threads eviction requested with cache fill ratio >= 75%'),
    CacheStat('cache_eviction_app_threads_fill_ratio_lt_25', 'application threads eviction requested with cache fill ratio < 25%'),
    CacheStat('cache_eviction_blocked_checkpoint', 'checkpoint blocked page eviction'),
    CacheStat('cache_eviction_blocked_checkpoint_hs', 'checkpoint of history store file blocked non-history store page eviction'),
    CacheStat('cache_eviction_blocked_hazard', 'hazard pointer blocked page eviction'),
    CacheStat('cache_eviction_blocked_internal_page_split', 'internal page split blocked its eviction'),
    CacheStat('cache_eviction_blocked_multi_block_reconciliation_during_checkpoint', 'multi-block reconciliation blocked whilst checkpoint is running'),
    CacheStat('cache_eviction_blocked_no_progress', 'eviction gave up due to no progress being made'),
    CacheStat('cache_eviction_blocked_no_ts_checkpoint_race_1', 'eviction gave up due to detecting a disk value without a timestamp behind the last update on the chain'),
    CacheStat('cache_eviction_blocked_no_ts_checkpoint_race_2', 'eviction gave up due to detecting a tombstone without a timestamp ahead of the selected on disk update'),
    CacheStat('cache_eviction_blocked_no_ts_checkpoint_race_3', 'eviction gave up due to detecting a tombstone without a timestamp ahead of the selected on disk update after validating the update chain'),
    CacheStat('cache_eviction_blocked_no_ts_checkpoint_race_4', 'eviction gave up due to detecting update chain entries without timestamps after the selected on disk update'),
    CacheStat('cache_eviction_blocked_overflow_keys', 'overflow keys on a multiblock row-store page blocked its eviction'),
    CacheStat('cache_eviction_blocked_recently_modified', 'recent modification of a page blocked its eviction'),
    CacheStat('cache_eviction_blocked_remove_hs_race_with_checkpoint', 'eviction gave up due to needing to remove a record from the history store but checkpoint is running'),
    CacheStat('cache_eviction_blocked_uncommitted_truncate', 'uncommitted truncate blocked page eviction'),
    CacheStat('cache_eviction_clean', 'unmodified pages evicted'),
    CacheStat('cache_eviction_deepen', 'page split during eviction deepened the tree'),
    CacheStat('cache_eviction_dirty', 'modified pages evicted'),
    CacheStat('cache_eviction_dirty_obsolete_tw', 'pages dirtied due to obsolete time window by eviction'),
    CacheStat('cache_eviction_internal', 'internal pages evicted'),
    CacheStat('cache_eviction_pages_queued_clean', 'eviction walk pages queued that were clean'),
    CacheStat('cache_eviction_pages_queued_dirty', 'eviction walk pages queued that were dirty'),
    CacheStat('cache_eviction_pages_queued_updates', 'eviction walk pages queued that had updates'),
    CacheStat('cache_eviction_pages_seen', 'pages seen by eviction walk'),
    CacheStat('cache_eviction_pages_seen_clean', 'eviction walk pages seen that were clean'),
    CacheStat('cache_eviction_pages_seen_dirty', 'eviction walk pages seen that were dirty'),
    CacheStat('cache_eviction_pages_seen_updates', 'eviction walk pages seen that had updates'),
    CacheStat('cache_eviction_random_sample_inmem_root', 'locate a random in-mem ref by examining all entries on the root page'),
    CacheStat('cache_eviction_split_internal', 'internal pages split during eviction'),
    CacheStat('cache_eviction_split_leaf', 'leaf pages split during eviction'),
    CacheStat('cache_eviction_target_page_ge128', 'eviction walk target pages histogram - 128 and higher'),
    CacheStat('cache_eviction_target_page_lt10', 'eviction walk target pages histogram - 0-9'),
    CacheStat('cache_eviction_target_page_lt128', 'eviction walk target pages histogram - 64-128'),
    CacheStat('cache_eviction_target_page_lt32', 'eviction walk target pages histogram - 10-31'),
    CacheStat('cache_eviction_target_page_lt64', 'eviction walk target pages histogram - 32-63'),
    CacheStat('cache_eviction_target_page_reduced', 'eviction walk target pages reduced due to history store cache pressure'),
    CacheStat('cache_eviction_trigger_dirty_reached', 'number of times dirty trigger was reached'),
    CacheStat('cache_eviction_trigger_reached', 'number of times eviction trigger was reached'),
    CacheStat('cache_eviction_trigger_updates_reached', 'number of times updates trigger was reached'),
    CacheStat('cache_hs_btree_truncate', 'history store table truncation to remove all the keys of a btree'),
    CacheStat('cache_hs_btree_truncate_dryrun', 'history store table truncations that would have happened in non-dryrun mode'),
    CacheStat('cache_hs_insert', 'history store table insert calls'),
    CacheStat('cache_hs_insert_full_update', 'the number of times full update inserted to history store'),
    CacheStat('cache_hs_insert_restart', 'history store table insert calls that returned restart'),
    CacheStat('cache_hs_insert_reverse_modify', 'the number of times reverse modify inserted to history store'),
    CacheStat('cache_hs_key_truncate', 'history store table truncation to remove an update'),
    CacheStat('cache_hs_key_truncate_onpage_removal', 'history store table truncation to remove range of updates due to key being removed from the data page during reconciliation'),
    CacheStat('cache_hs_key_truncate_rts', 'history store table truncation by rollback to stable to remove an update'),
    CacheStat('cache_hs_key_truncate_rts_dryrun', 'history store table truncations to remove an update that would have happened in non-dryrun mode'),
    CacheStat('cache_hs_key_truncate_rts_unstable', 'history store table truncation by rollback to stable to remove an unstable update'),
    CacheStat('cache_hs_key_truncate_rts_unstable_dryrun', 'history store table truncations to remove an unstable update that would have happened in non-dryrun mode'),
    CacheStat('cache_hs_order_lose_durable_timestamp', 'history store table resolved updates without timestamps that lose their durable timestamp'),
    CacheStat('cache_hs_order_reinsert', 'history store table updates without timestamps fixed up by reinserting with the fixed timestamp'),
    CacheStat('cache_hs_order_remove', 'history store table truncation to remove range of updates due to an update without a timestamp on data page'),
    CacheStat('cache_hs_read', 'history store table reads'),
    CacheStat('cache_hs_read_miss', 'history store table reads missed'),
    CacheStat('cache_hs_read_squash', 'history store table reads requiring squashed modifies'),
    CacheStat('cache_hs_write_squash', 'history store table writes requiring squashed modifies'),
    CacheStat('cache_inmem_split', 'in-memory page splits'),
    CacheStat('cache_inmem_splittable', 'in-memory page passed criteria to be split'),
    CacheStat('cache_pages_prefetch', 'pages requested from the cache due to pre-fetch'),
    CacheStat('cache_pages_requested', 'pages requested from the cache'),
    CacheStat('cache_pages_requested_internal', 'pages requested from the cache internal'),
    CacheStat('cache_pages_requested_leaf', 'pages requested from the cache leaf'),
    CacheStat('cache_read', 'pages read into cache'),
    CacheStat('cache_read_checkpoint', 'pages read into cache by checkpoint'),
    CacheStat('cache_read_deleted', 'pages read into cache after truncate'),
    CacheStat('cache_read_deleted_prepared', 'pages read into cache after truncate in prepare state'),
    CacheStat('cache_read_overflow', 'overflow pages read into cache'),
    CacheStat('cache_reverse_splits', 'reverse splits performed'),
    CacheStat('cache_reverse_splits_skipped_vlcs', 'reverse splits skipped because of VLCS namespace gap restrictions'),
    CacheStat('cache_write', 'pages written from cache'),
    CacheStat('cache_write_hs', 'page written requiring history store records'),
    CacheStat('cache_write_restore', 'pages written requiring in-memory restoration'),

    ##########################################
    # Checkpoint statistics
    ##########################################
    CheckpointStat('checkpoint_cleanup_pages_evict', 'pages added for eviction during checkpoint cleanup'),
    CheckpointStat('checkpoint_cleanup_pages_obsolete_tw', 'pages dirtied due to obsolete time window by checkpoint cleanup'),
    CheckpointStat('checkpoint_cleanup_pages_read_obsolete_tw', 'pages read into cache during checkpoint cleanup due to obsolete time window'),
    CheckpointStat('checkpoint_cleanup_pages_read_reclaim_space', 'pages read into cache during checkpoint cleanup (reclaim_space)'),
    CheckpointStat('checkpoint_cleanup_pages_removed', 'pages removed during checkpoint cleanup'),
    CheckpointStat('checkpoint_cleanup_pages_visited', 'pages visited during checkpoint cleanup'),
    CheckpointStat('checkpoint_cleanup_pages_walk_skipped', 'pages skipped during checkpoint cleanup tree walk'),
    CheckpointStat('checkpoint_snapshot_acquired', 'checkpoint has acquired a snapshot for its transaction'),

    ##########################################
    # Cursor operations
    ##########################################
    CursorStat('cursor_bounds_comparisons', 'cursor bounds comparisons performed'),
    CursorStat('cursor_bounds_next_early_exit', 'cursor bounds next early exit'),
    CursorStat('cursor_bounds_next_unpositioned', 'cursor bounds next called on an unpositioned cursor'),
    CursorStat('cursor_bounds_prev_early_exit', 'cursor bounds prev early exit'),
    CursorStat('cursor_bounds_prev_unpositioned', 'cursor bounds prev called on an unpositioned cursor'),
    CursorStat('cursor_bounds_reset', 'cursor bounds cleared from reset'),
    CursorStat('cursor_bounds_search_early_exit', 'cursor bounds search early exit'),
    CursorStat('cursor_bounds_search_near_repositioned_cursor', 'cursor bounds search near call repositioned cursor'),
    CursorStat('cursor_next_hs_tombstone', 'cursor next calls that skip due to a globally visible history store tombstone'),
    CursorStat('cursor_next_skip_ge_100', 'cursor next calls that skip greater than or equal to 100 entries'),
    CursorStat('cursor_next_skip_lt_100', 'cursor next calls that skip greater than 1 and fewer than 100 entries'),
    CursorStat('cursor_next_skip_total', 'Total number of entries skipped by cursor next calls'),
    CursorStat('cursor_open_count', 'open cursor count', 'no_clear,no_scale'),
    CursorStat('cursor_prev_hs_tombstone', 'cursor prev calls that skip due to a globally visible history store tombstone'),
    CursorStat('cursor_prev_skip_ge_100', 'cursor prev calls that skip greater than or equal to 100 entries'),
    CursorStat('cursor_prev_skip_lt_100', 'cursor prev calls that skip less than 100 entries'),
    CursorStat('cursor_prev_skip_total', 'Total number of entries skipped by cursor prev calls'),
    CursorStat('cursor_reposition', 'Total number of times cursor temporarily releases pinned page to encourage eviction of hot or large page'),
    CursorStat('cursor_reposition_failed', 'Total number of times cursor fails to temporarily release pinned page to encourage eviction of hot or large page'),
    CursorStat('cursor_search_near_prefix_fast_paths', 'Total number of times a search near has exited due to prefix config'),
    CursorStat('cursor_skip_hs_cur_position', 'Total number of entries skipped to position the history store cursor'),
    CursorStat('cursor_tree_walk_del_page_skip', 'Total number of deleted pages skipped during tree walk'),
    CursorStat('cursor_tree_walk_inmem_del_page_skip', 'Total number of in-memory deleted pages skipped during tree walk'),
    CursorStat('cursor_tree_walk_ondisk_del_page_skip', 'Total number of on-disk deleted pages skipped during tree walk'),

    ##########################################
    # Cursor API error statistics
    ##########################################
    CursorErrorStat('cursor_bound_error', 'cursor bound calls that return an error'),
    CursorErrorStat('cursor_cache_error', 'cursor cache calls that return an error'),
    CursorErrorStat('cursor_close_error', 'cursor close calls that return an error'),
    CursorErrorStat('cursor_compare_error', 'cursor compare calls that return an error'),
    CursorErrorStat('cursor_equals_error', 'cursor equals calls that return an error'),
    CursorErrorStat('cursor_get_key_error', 'cursor get key calls that return an error'),
    CursorErrorStat('cursor_get_value_error', 'cursor get value calls that return an error'),
    CursorErrorStat('cursor_insert_check_error', 'cursor insert check calls that return an error'),
    CursorErrorStat('cursor_insert_error', 'cursor insert calls that return an error'),
    CursorErrorStat('cursor_largest_key_error', 'cursor largest key calls that return an error'),
    CursorErrorStat('cursor_modify_error', 'cursor modify calls that return an error'),
    CursorErrorStat('cursor_next_error', 'cursor next calls that return an error'),
    CursorErrorStat('cursor_next_random_error', 'cursor next random calls that return an error'),
    CursorErrorStat('cursor_prev_error', 'cursor prev calls that return an error'),
    CursorErrorStat('cursor_reconfigure_error', 'cursor reconfigure calls that return an error'),
    CursorErrorStat('cursor_remove_error', 'cursor remove calls that return an error'),
    CursorErrorStat('cursor_reopen_error', 'cursor reopen calls that return an error'),
    CursorErrorStat('cursor_reserve_error', 'cursor reserve calls that return an error'),
    CursorErrorStat('cursor_reset_error', 'cursor reset calls that return an error'),
    CursorErrorStat('cursor_search_error', 'cursor search calls that return an error'),
    CursorErrorStat('cursor_search_near_error', 'cursor search near calls that return an error'),
    CursorErrorStat('cursor_update_error', 'cursor update calls that return an error'),

    ##########################################
    # Disaggregated block manager statistics
    ##########################################
    BlockDisaggStat('disagg_block_get', 'Disaggregated block manager get'),
    BlockDisaggStat('disagg_block_hs_byte_read', 'Bytes read from the shared history store in SLS', 'size'),
    BlockDisaggStat('disagg_block_hs_byte_write', 'Bytes written to the shared history store in SLS', 'size'),
    BlockDisaggStat('disagg_block_hs_get', 'Disaggregated block manager get from the shared history store in SLS'),
    BlockDisaggStat('disagg_block_hs_put', 'Disaggregated block manager put to the shared history store in SLS'),
    BlockDisaggStat('disagg_block_put', 'Disaggregated block manager put '),

    ##########################################
    # Reconciliation statistics
    ##########################################
    RecStat('rec_hs_wrapup_next_prev_calls', 'cursor next/prev calls during HS wrapup search_near'),
    RecStat('rec_multiblock_internal', 'internal page multi-block writes'),
    RecStat('rec_multiblock_leaf', 'leaf page multi-block writes'),
    RecStat('rec_overflow_key_leaf', 'leaf-page overflow keys'),
    RecStat('rec_overflow_value', 'overflow values written'),
    RecStat('rec_page_delete', 'pages deleted'),
    RecStat('rec_page_delete_fast', 'fast-path pages deleted'),
    RecStat('rec_pages', 'page reconciliation calls'),
    RecStat('rec_pages_eviction', 'page reconciliation calls for eviction'),
    RecStat('rec_time_aggr_newest_start_durable_ts', 'pages written including an aggregated newest start durable timestamp '),
    RecStat('rec_time_aggr_newest_stop_durable_ts', 'pages written including an aggregated newest stop durable timestamp '),
    RecStat('rec_time_aggr_newest_stop_ts', 'pages written including an aggregated newest stop timestamp '),
    RecStat('rec_time_aggr_newest_stop_txn', 'pages written including an aggregated newest stop transaction ID'),
    RecStat('rec_time_aggr_newest_txn', 'pages written including an aggregated newest transaction ID '),
    RecStat('rec_time_aggr_oldest_start_ts', 'pages written including an aggregated oldest start timestamp '),
    RecStat('rec_time_aggr_prepared', 'pages written including an aggregated prepare'),
    RecStat('rec_time_window_bytes_ts', 'approximate byte size of timestamps in pages written'),
    RecStat('rec_time_window_bytes_txn', 'approximate byte size of transaction IDs in pages written'),
    RecStat('rec_time_window_durable_start_ts', 'records written including a start durable timestamp'),
    RecStat('rec_time_window_durable_stop_ts', 'records written including a stop durable timestamp'),
    RecStat('rec_time_window_pages_durable_start_ts', 'pages written including at least one start durable timestamp'),
    RecStat('rec_time_window_pages_durable_stop_ts', 'pages written including at least one stop durable timestamp'),
    RecStat('rec_time_window_pages_start_txn', 'pages written including at least one start transaction ID'),
    RecStat('rec_time_window_pages_stop_ts', 'pages written including at least one stop timestamp'),
    RecStat('rec_time_window_pages_stop_txn', 'pages written including at least one stop transaction ID'),
    RecStat('rec_time_window_start_ts', 'records written including a start timestamp'),
    RecStat('rec_time_window_start_txn', 'records written including a start transaction ID'),
    RecStat('rec_time_window_stop_ts', 'records written including a stop timestamp'),
    RecStat('rec_time_window_stop_txn', 'records written including a stop transaction ID'),
    RecStat('rec_vlcs_emptied_pages', 'VLCS pages explicitly reconciled as empty'),

    ##########################################
    # Transaction statistics
    ##########################################
    TxnStat('txn_read_overflow_remove', 'number of times overflow removed value is read'),
    TxnStat('txn_read_race_prepare_commit', 'a reader raced with a prepared transaction commit and skipped an update or updates'),
    TxnStat('txn_read_race_prepare_update', 'race to read prepared update retry'),
    TxnStat('txn_rts_delete_rle_skipped', 'rollback to stable skipping delete rle'),
    TxnStat('txn_rts_hs_removed', 'rollback to stable updates removed from history store'),
    TxnStat('txn_rts_hs_removed_dryrun', 'rollback to stable updates that would have been removed from history store in non-dryrun mode'),
    TxnStat('txn_rts_hs_restore_tombstones', 'rollback to stable restored tombstones from history store'),
    TxnStat('txn_rts_hs_restore_tombstones_dryrun', 'rollback to stable tombstones from history store that would have been restored in non-dryrun mode'),
    TxnStat('txn_rts_hs_restore_updates', 'rollback to stable restored updates from history store'),
    TxnStat('txn_rts_hs_restore_updates_dryrun', 'rollback to stable updates from history store that would have been restored in non-dryrun mode'),
    TxnStat('txn_rts_hs_stop_older_than_newer_start', 'rollback to stable history store records with stop timestamps older than newer records'),
    TxnStat('txn_rts_inconsistent_ckpt', 'rollback to stable inconsistent checkpoint'),
    TxnStat('txn_rts_keys_removed', 'rollback to stable keys removed'),
    TxnStat('txn_rts_keys_removed_dryrun', 'rollback to stable keys that would have been removed in non-dryrun mode'),
    TxnStat('txn_rts_keys_restored', 'rollback to stable keys restored'),
    TxnStat('txn_rts_keys_restored_dryrun', 'rollback to stable keys that would have been restored in non-dryrun mode'),
    TxnStat('txn_rts_stable_rle_skipped', 'rollback to stable skipping stable rle'),
    TxnStat('txn_rts_sweep_hs_keys', 'rollback to stable sweeping history store keys'),
    TxnStat('txn_rts_sweep_hs_keys_dryrun', 'rollback to stable history store keys that would have been swept in non-dryrun mode'),
    TxnStat('txn_update_conflict', 'update conflicts'),
]

##########################################
# Session statistics
##########################################
session_stats = [
    SessionStat('bytes_read', 'bytes read into cache'),
    SessionStat('bytes_write', 'bytes written from cache'),
    SessionStat('cache_time', 'time waiting for cache (usecs)'),
    SessionStat('cache_time_interruptible', 'time waiting for cache interruptible eviction (usecs)'),
    SessionStat('cache_time_mandatory', 'time waiting for mandatory cache eviction (usecs)'),
    SessionStat('lock_dhandle_wait', 'dhandle lock wait time (usecs)'),
    SessionStat('lock_schema_wait', 'schema lock wait time (usecs)'),
    SessionStat('read_time', 'page read from disk to cache time (usecs)'),
    SessionStat('txn_bytes_dirty', 'dirty bytes in this txn', 'no_clear,no_scale,size'),
    SessionStat('txn_updates', 'number of updates in this txn', 'no_clear,no_scale,size'),
    SessionStat('write_time', 'page write from cache to disk time (usecs)'),
]
