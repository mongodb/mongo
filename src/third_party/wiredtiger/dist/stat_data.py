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

class BlockCacheStat(Stat):
    prefix = 'block-cache'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, BlockCacheStat.prefix, desc, flags)
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
class CacheWalkStat(Stat):
    prefix = 'cache_walk'
    def __init__(self, name, desc, flags=''):
        flags += ',cache_walk'
        Stat.__init__(self, name, CacheWalkStat.prefix, desc, flags)
class CapacityStat(Stat):
    prefix = 'capacity'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, CapacityStat.prefix, desc, flags)
class CheckpointCleanupStat(Stat):
    prefix = 'checkpoint-cleanup'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, CheckpointCleanupStat.prefix, desc, flags)
class CompressStat(Stat):
    prefix = 'compression'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, CompressStat.prefix, desc, flags)
class ConnStat(Stat):
    prefix = 'connection'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, ConnStat.prefix, desc, flags)
class CursorStat(Stat):
    prefix = 'cursor'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, CursorStat.prefix, desc, flags)
class DhandleStat(Stat):
    prefix = 'data-handle'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, DhandleStat.prefix, desc, flags)
class JoinStat(Stat):
    prefix = ''  # prefix is inserted dynamically
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, JoinStat.prefix, desc, flags)
class LockStat(Stat):
    prefix = 'lock'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, LockStat.prefix, desc, flags)
class LogStat(Stat):
    prefix = 'log'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, LogStat.prefix, desc, flags)
class LSMStat(Stat):
    prefix = 'LSM'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, LSMStat.prefix, desc, flags)
class SessionStat(Stat):
    prefix = 'session'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, SessionStat.prefix, desc, flags)
class PerfHistStat(Stat):
    prefix = 'perf'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, PerfHistStat.prefix, desc, flags)
class RecStat(Stat):
    prefix = 'reconciliation'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, RecStat.prefix, desc, flags)
class SessionOpStat(Stat):
    prefix = 'session'
    def __init__(self, name, desc, flags=''):
        Stat.__init__(self, name, SessionOpStat.prefix, desc, flags)
class StorageStat(Stat):
    prefix = 'session'
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
# Groupings of useful statistics:
# A pre-defined dictionary containing the group name as the key and the
# list of prefix tags that comprise that group.
##########################################
groups = {}
groups['cursor'] = [CursorStat.prefix, SessionOpStat.prefix]
groups['evict'] = [
    BlockCacheStat.prefix,
    BlockStat.prefix,
    CacheStat.prefix,
    CacheWalkStat.prefix,
    ConnStat.prefix,
    ThreadStat.prefix
]
groups['lsm'] = [LSMStat.prefix, TxnStat.prefix]
groups['memory'] = [
    CacheStat.prefix,
    CacheWalkStat.prefix,
    ConnStat.prefix,
    RecStat.prefix]
groups['system'] = [
    CapacityStat.prefix,
    ConnStat.prefix,
    DhandleStat.prefix,
    PerfHistStat.prefix,
    SessionOpStat.prefix,
    ThreadStat.prefix
]

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
    ConnStat('read_io', 'total read I/Os'),
    ConnStat('rwlock_read', 'pthread mutex shared lock read-lock calls'),
    ConnStat('rwlock_write', 'pthread mutex shared lock write-lock calls'),
    ConnStat('time_travel', 'detected system time went backwards'),
    ConnStat('write_io', 'total write I/Os'),

    ##########################################
    # Block manager statistics
    ##########################################
    BlockCacheStat('block_cache_blocks', 'total blocks'),
    BlockCacheStat('block_cache_blocks_evicted', 'evicted blocks'),
    BlockCacheStat('block_cache_blocks_insert_read', 'total blocks inserted on read path'),
    BlockCacheStat('block_cache_blocks_insert_write', 'total blocks inserted on write path'),
    BlockCacheStat('block_cache_blocks_removed', 'removed blocks'),
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
    BlockStat('block_byte_read_mmap', 'bytes read via memory map API', 'size'),
    BlockStat('block_byte_read_syscall', 'bytes read via system call API', 'size'),
    BlockStat('block_byte_write', 'bytes written', 'size'),
    BlockStat('block_byte_write_checkpoint', 'bytes written for checkpoint', 'size'),
    BlockStat('block_byte_write_mmap', 'bytes written via memory map API', 'size'),
    BlockStat('block_byte_write_syscall', 'bytes written via system call API', 'size'),
    BlockStat('block_map_read', 'mapped blocks read'),
    BlockStat('block_preload', 'blocks pre-loaded'),
    BlockStat('block_read', 'blocks read'),
    BlockStat('block_remap_file_resize', 'number of times the file was remapped because it changed size via fallocate or truncate'),
    BlockStat('block_remap_file_write', 'number of times the region was remapped via write'),
    BlockStat('block_write', 'blocks written'),

    ##########################################
    # Cache and eviction statistics
    ##########################################
    CacheStat('cache_bytes_image', 'bytes belonging to page images in the cache', 'no_clear,no_scale,size'),
    CacheStat('cache_bytes_internal', 'tracked bytes belonging to internal pages in the cache', 'no_clear,no_scale,size'),
    CacheStat('cache_bytes_leaf', 'tracked bytes belonging to leaf pages in the cache', 'no_clear,no_scale,size'),
    CacheStat('cache_bytes_hs', 'bytes belonging to the history store table in the cache', 'no_clear,no_scale,size'),
    CacheStat('cache_bytes_max', 'maximum bytes configured', 'no_clear,no_scale,size'),
    CacheStat('cache_bytes_other', 'bytes not belonging to page images in the cache', 'no_clear,no_scale,size'),
    CacheStat('cache_bytes_updates', 'bytes allocated for updates', 'no_clear,no_scale,size'),
    CacheStat('cache_eviction_active_workers', 'eviction worker thread active', 'no_clear'),
    CacheStat('cache_eviction_aggressive_set', 'eviction currently operating in aggressive mode', 'no_clear,no_scale'),
    CacheStat('cache_eviction_app', 'pages evicted by application threads'),
    CacheStat('cache_eviction_app_dirty', 'modified pages evicted by application threads'),
    CacheStat('cache_eviction_empty_score', 'eviction empty score', 'no_clear,no_scale'),
    CacheStat('cache_eviction_fail', 'pages selected for eviction unable to be evicted'),
    CacheStat('cache_eviction_fail_active_children_on_an_internal_page', 'pages selected for eviction unable to be evicted because of active children on an internal page'),
    CacheStat('cache_eviction_fail_checkpoint_out_of_order_ts', 'pages selected for eviction unable to be evicted because of race between checkpoint and out of order timestamps handling'),
    CacheStat('cache_eviction_fail_in_reconciliation', 'pages selected for eviction unable to be evicted because of failure in reconciliation'),
    CacheStat('cache_eviction_force', 'forced eviction - pages selected count'),
    CacheStat('cache_eviction_force_long_update_list', 'forced eviction - pages selected because of a large number of updates to a single item'),
    CacheStat('cache_eviction_force_clean', 'forced eviction - pages evicted that were clean count'),
    CacheStat('cache_eviction_force_clean_time', 'forced eviction - pages evicted that were clean time (usecs)'),
    CacheStat('cache_eviction_force_delete', 'forced eviction - pages selected because of too many deleted items count'),
    CacheStat('cache_eviction_force_dirty', 'forced eviction - pages evicted that were dirty count'),
    CacheStat('cache_eviction_force_dirty_time', 'forced eviction - pages evicted that were dirty time (usecs)'),
    CacheStat('cache_eviction_force_fail', 'forced eviction - pages selected unable to be evicted count'),
    CacheStat('cache_eviction_force_fail_time', 'forced eviction - pages selected unable to be evicted time'),
    CacheStat('cache_eviction_force_hs', 'forced eviction - history store pages selected while session has history store cursor open'),
    CacheStat('cache_eviction_force_hs_fail', 'forced eviction - history store pages failed to evict while session has history store cursor open'),
    CacheStat('cache_eviction_force_hs_success', 'forced eviction - history store pages successfully evicted while session has history store cursor open'),
    CacheStat('cache_eviction_force_retune', 'force re-tuning of eviction workers once in a while'),
    CacheStat('cache_eviction_get_ref', 'eviction calls to get a page'),
    CacheStat('cache_eviction_get_ref_empty', 'eviction calls to get a page found queue empty'),
    CacheStat('cache_eviction_get_ref_empty2', 'eviction calls to get a page found queue empty after locking'),
    CacheStat('cache_eviction_internal_pages_seen', 'internal pages seen by eviction walk'),
    CacheStat('cache_eviction_internal_pages_already_queued', 'internal pages seen by eviction walk that are already queued'),
    CacheStat('cache_eviction_internal_pages_queued', 'internal pages queued for eviction'),
    CacheStat('cache_eviction_maximum_page_size', 'maximum page size at eviction', 'no_clear,no_scale,size'),
    CacheStat('cache_eviction_pages_queued', 'pages queued for eviction'),
    CacheStat('cache_eviction_pages_queued_oldest', 'pages queued for urgent eviction during walk'),
    CacheStat('cache_eviction_pages_queued_post_lru', 'pages queued for eviction post lru sorting'),
    CacheStat('cache_eviction_pages_queued_urgent', 'pages queued for urgent eviction'),
    CacheStat('cache_eviction_pages_queued_urgent_hs_dirty', 'pages queued for urgent eviction from history store due to high dirty content'),
    CacheStat('cache_eviction_pages_already_queued', 'pages seen by eviction walk that are already queued'),
    CacheStat('cache_eviction_pages_in_parallel_with_checkpoint', 'pages evicted in parallel with checkpoint'),
    CacheStat('cache_eviction_queue_empty', 'eviction server candidate queue empty when topping up'),
    CacheStat('cache_eviction_queue_not_empty', 'eviction server candidate queue not empty when topping up'),
    CacheStat('cache_eviction_server_evicting', 'eviction server evicting pages'),
    CacheStat('cache_eviction_server_slept', 'eviction server slept, because we did not make progress with eviction'),
    CacheStat('cache_eviction_slow', 'eviction server unable to reach eviction goal'),
    CacheStat('cache_eviction_stable_state_workers', 'eviction worker thread stable number', 'no_clear'),
    CacheStat('cache_eviction_state', 'eviction state', 'no_clear,no_scale'),
    CacheStat('cache_eviction_target_strategy_clean', 'eviction walk target strategy only clean pages'),
    CacheStat('cache_eviction_target_strategy_dirty', 'eviction walk target strategy only dirty pages'),
    CacheStat('cache_eviction_target_strategy_both_clean_and_dirty', 'eviction walk target strategy both clean and dirty pages'),
    CacheStat('cache_eviction_walk', 'pages walked for eviction'),
    CacheStat('cache_eviction_walk_leaf_notfound', 'eviction server waiting for a leaf page'),
    CacheStat('cache_eviction_walk_passes', 'eviction passes of a file'),
    CacheStat('cache_eviction_walk_sleeps', 'eviction walk most recent sleeps for checkpoint handle gathering'),
    CacheStat('cache_eviction_walks_active', 'files with active eviction walks', 'no_clear,no_scale'),
    CacheStat('cache_eviction_walks_started', 'files with new eviction walks started'),
    CacheStat('cache_eviction_worker_created', 'eviction worker thread created'),
    CacheStat('cache_eviction_worker_evicting', 'eviction worker thread evicting pages'),
    CacheStat('cache_eviction_worker_removed', 'eviction worker thread removed'),
    CacheStat('cache_hazard_checks', 'hazard pointer check calls'),
    CacheStat('cache_hazard_max', 'hazard pointer maximum array length', 'max_aggregate,no_scale'),
    CacheStat('cache_hazard_walks', 'hazard pointer check entries walked'),
    CacheStat('cache_hs_ondisk', 'history store table on-disk size', 'no_clear,no_scale,size'),
    CacheStat('cache_hs_ondisk_max', 'history store table max on-disk size', 'no_clear,no_scale,size'),
    CacheStat('cache_hs_score', 'history store score', 'no_clear,no_scale'),
    CacheStat('cache_lookaside_score', 'cache overflow score', 'no_clear,no_scale'),
    CacheStat('cache_overhead', 'percentage overhead', 'no_clear,no_scale'),
    CacheStat('cache_pages_dirty', 'tracked dirty pages in the cache', 'no_clear,no_scale'),
    CacheStat('cache_pages_inuse', 'pages currently held in the cache', 'no_clear,no_scale'),
    CacheStat('cache_read_app_count', 'application threads page read from disk to cache count'),
    CacheStat('cache_read_app_time', 'application threads page read from disk to cache time (usecs)'),
    CacheStat('cache_timed_out_ops', 'operations timed out waiting for space in cache'),
    CacheStat('cache_write_app_count', 'application threads page write from cache to disk count'),
    CacheStat('cache_write_app_time', 'application threads page write from cache to disk time (usecs)'),

    ##########################################
    # Capacity statistics
    ##########################################
    CapacityStat('capacity_bytes_ckpt', 'bytes written for checkpoint'),
    CapacityStat('capacity_bytes_evict', 'bytes written for eviction'),
    CapacityStat('capacity_bytes_log', 'bytes written for log'),
    CapacityStat('capacity_bytes_read', 'bytes read'),
    CapacityStat('capacity_bytes_written', 'bytes written total'),
    CapacityStat('capacity_threshold', 'threshold to call fsync'),
    CapacityStat('capacity_time_ckpt', 'time waiting during checkpoint (usecs)'),
    CapacityStat('capacity_time_evict', 'time waiting during eviction (usecs)'),
    CapacityStat('capacity_time_log', 'time waiting during logging (usecs)'),
    CapacityStat('capacity_time_read', 'time waiting during read (usecs)'),
    CapacityStat('capacity_time_total', 'time waiting due to total capacity (usecs)'),
    CapacityStat('fsync_all_fh', 'background fsync file handles synced'),
    CapacityStat('fsync_all_fh_total', 'background fsync file handles considered'),
    CapacityStat('fsync_all_time', 'background fsync time (msecs)', 'no_clear,no_scale'),

    ##########################################
    # Cursor operations
    ##########################################
    CursorStat('cursor_cached_count', 'cached cursor count', 'no_clear,no_scale'),
    CursorStat('cursor_cache', 'cursor close calls that result in cache'),
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
    CursorStat('cursor_update', 'cursor update calls'),
    CursorStat('cursor_update_bytes', 'cursor update key and value bytes', 'size'),
    CursorStat('cursor_update_bytes_changed', 'cursor update value size change', 'size'),

    ##########################################
    # Cursor sweep
    ##########################################
    CursorStat('cursor_sweep', 'cursor sweeps'),
    CursorStat('cursor_sweep_buckets', 'cursor sweep buckets'),
    CursorStat('cursor_sweep_closed', 'cursor sweep cursors closed'),
    CursorStat('cursor_sweep_examined', 'cursor sweep cursors examined'),

    ##########################################
    # Dhandle statistics
    ##########################################
    DhandleStat('dh_conn_handle_count', 'connection data handles currently active', 'no_clear,no_scale'),
    DhandleStat('dh_conn_handle_size', 'connection data handle size', 'no_clear,no_scale,size'),
    DhandleStat('dh_session_handles', 'session dhandles swept'),
    DhandleStat('dh_session_sweeps', 'session sweep attempts'),
    DhandleStat('dh_sweep_close', 'connection sweep dhandles closed'),
    DhandleStat('dh_sweep_ref', 'connection sweep candidate became referenced'),
    DhandleStat('dh_sweep_remove', 'connection sweep dhandles removed from hash list'),
    DhandleStat('dh_sweep_skip_ckpt', 'connection sweeps skipped due to checkpoint gathering handles'),
    DhandleStat('dh_sweep_tod', 'connection sweep time-of-death sets'),
    DhandleStat('dh_sweeps', 'connection sweeps'),

    ##########################################
    # Locking statistics
    ##########################################
    LockStat('lock_checkpoint_count', 'checkpoint lock acquisitions'),
    LockStat('lock_checkpoint_wait_application', 'checkpoint lock application thread wait time (usecs)'),
    LockStat('lock_checkpoint_wait_internal', 'checkpoint lock internal thread wait time (usecs)'),
    LockStat('lock_dhandle_read_count', 'dhandle read lock acquisitions'),
    LockStat('lock_dhandle_wait_application', 'dhandle lock application thread time waiting (usecs)'),
    LockStat('lock_dhandle_wait_internal', 'dhandle lock internal thread time waiting (usecs)'),
    LockStat('lock_dhandle_write_count', 'dhandle write lock acquisitions'),
    LockStat('lock_durable_timestamp_read_count', 'durable timestamp queue read lock acquisitions'),
    LockStat('lock_durable_timestamp_wait_application', 'durable timestamp queue lock application thread time waiting (usecs)'),
    LockStat('lock_durable_timestamp_wait_internal', 'durable timestamp queue lock internal thread time waiting (usecs)'),
    LockStat('lock_durable_timestamp_write_count', 'durable timestamp queue write lock acquisitions'),
    LockStat('lock_metadata_count', 'metadata lock acquisitions'),
    LockStat('lock_metadata_wait_application', 'metadata lock application thread wait time (usecs)'),
    LockStat('lock_metadata_wait_internal', 'metadata lock internal thread wait time (usecs)'),
    LockStat('lock_read_timestamp_read_count', 'read timestamp queue read lock acquisitions'),
    LockStat('lock_read_timestamp_wait_application', 'read timestamp queue lock application thread time waiting (usecs)'),
    LockStat('lock_read_timestamp_wait_internal', 'read timestamp queue lock internal thread time waiting (usecs)'),
    LockStat('lock_read_timestamp_write_count', 'read timestamp queue write lock acquisitions'),
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
    LogStat('log_force_archive_sleep', 'force archive time sleeping (usecs)'),
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
    # LSM statistics
    ##########################################
    LSMStat('lsm_rows_merged', 'rows merged in an LSM tree'),
    LSMStat('lsm_work_queue_app', 'application work units currently queued', 'no_clear,no_scale'),
    LSMStat('lsm_work_queue_manager', 'merge work units currently queued', 'no_clear,no_scale'),
    LSMStat('lsm_work_queue_max', 'tree queue hit maximum'),
    LSMStat('lsm_work_queue_switch', 'switch work units currently queued', 'no_clear,no_scale'),
    LSMStat('lsm_work_units_created', 'tree maintenance operations scheduled'),
    LSMStat('lsm_work_units_discarded', 'tree maintenance operations discarded'),
    LSMStat('lsm_work_units_done', 'tree maintenance operations executed'),

    ##########################################
    # Performance Histogram Stats
    ##########################################
    PerfHistStat('perf_hist_fsread_latency_gt1000', 'file system read latency histogram (bucket 6) - 1000ms+'),
    PerfHistStat('perf_hist_fsread_latency_lt50', 'file system read latency histogram (bucket 1) - 10-49ms'),
    PerfHistStat('perf_hist_fsread_latency_lt100', 'file system read latency histogram (bucket 2) - 50-99ms'),
    PerfHistStat('perf_hist_fsread_latency_lt250', 'file system read latency histogram (bucket 3) - 100-249ms'),
    PerfHistStat('perf_hist_fsread_latency_lt500', 'file system read latency histogram (bucket 4) - 250-499ms'),
    PerfHistStat('perf_hist_fsread_latency_lt1000', 'file system read latency histogram (bucket 5) - 500-999ms'),
    PerfHistStat('perf_hist_fswrite_latency_gt1000', 'file system write latency histogram (bucket 6) - 1000ms+'),
    PerfHistStat('perf_hist_fswrite_latency_lt50', 'file system write latency histogram (bucket 1) - 10-49ms'),
    PerfHistStat('perf_hist_fswrite_latency_lt100', 'file system write latency histogram (bucket 2) - 50-99ms'),
    PerfHistStat('perf_hist_fswrite_latency_lt250', 'file system write latency histogram (bucket 3) - 100-249ms'),
    PerfHistStat('perf_hist_fswrite_latency_lt500', 'file system write latency histogram (bucket 4) - 250-499ms'),
    PerfHistStat('perf_hist_fswrite_latency_lt1000', 'file system write latency histogram (bucket 5) - 500-999ms'),
    PerfHistStat('perf_hist_opread_latency_gt10000', 'operation read latency histogram (bucket 5) - 10000us+'),
    PerfHistStat('perf_hist_opread_latency_lt250', 'operation read latency histogram (bucket 1) - 100-249us'),
    PerfHistStat('perf_hist_opread_latency_lt500', 'operation read latency histogram (bucket 2) - 250-499us'),
    PerfHistStat('perf_hist_opread_latency_lt1000', 'operation read latency histogram (bucket 3) - 500-999us'),
    PerfHistStat('perf_hist_opread_latency_lt10000', 'operation read latency histogram (bucket 4) - 1000-9999us'),
    PerfHistStat('perf_hist_opwrite_latency_gt10000', 'operation write latency histogram (bucket 5) - 10000us+'),
    PerfHistStat('perf_hist_opwrite_latency_lt250', 'operation write latency histogram (bucket 1) - 100-249us'),
    PerfHistStat('perf_hist_opwrite_latency_lt500', 'operation write latency histogram (bucket 2) - 250-499us'),
    PerfHistStat('perf_hist_opwrite_latency_lt1000', 'operation write latency histogram (bucket 3) - 500-999us'),
    PerfHistStat('perf_hist_opwrite_latency_lt10000', 'operation write latency histogram (bucket 4) - 1000-9999us'),

    ##########################################
    # Reconciliation statistics
    ##########################################
    RecStat('rec_maximum_seconds', 'maximum seconds spent in a reconciliation call', 'no_clear,no_scale,size'),
    RecStat('rec_overflow_key_leaf', 'leaf-page overflow keys'),
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
    SessionOpStat('session_table_compact_fail', 'table compact failed calls', 'no_clear,no_scale'),
    SessionOpStat('session_table_compact_fail_cache_pressure', 'table compact failed calls due to cache pressure', 'no_clear,no_scale'),
    SessionOpStat('session_table_compact_running', 'table compact running', 'no_clear,no_scale'),
    SessionOpStat('session_table_compact_skipped', 'table compact skipped as process would not reduce file size', 'no_clear,no_scale'),
    SessionOpStat('session_table_compact_success', 'table compact successful calls', 'no_clear,no_scale'),
    SessionOpStat('session_table_compact_timeout', 'table compact timeout', 'no_clear,no_scale'),
    SessionOpStat('session_table_create_fail', 'table create failed calls', 'no_clear,no_scale'),
    SessionOpStat('session_table_create_success', 'table create successful calls', 'no_clear,no_scale'),
    SessionOpStat('session_table_drop_fail', 'table drop failed calls', 'no_clear,no_scale'),
    SessionOpStat('session_table_drop_success', 'table drop successful calls', 'no_clear,no_scale'),
    SessionOpStat('session_table_rename_fail', 'table rename failed calls', 'no_clear,no_scale'),
    SessionOpStat('session_table_rename_success', 'table rename successful calls', 'no_clear,no_scale'),
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
    StorageStat('flush_tier_skipped', 'flush_tier tables skipped due to no checkpoint'),
    StorageStat('flush_tier_switched', 'flush_tier tables switched'),
    StorageStat('local_objects_inuse', 'attempts to remove a local object and the object is in use'),
    StorageStat('local_objects_removed', 'local objects removed'),

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
    TxnStat('txn_checkpoint', 'transaction checkpoints'),
    TxnStat('txn_checkpoint_fsync_post', 'transaction fsync calls for checkpoint after allocating the transaction ID'),
    TxnStat('txn_checkpoint_fsync_post_duration', 'transaction fsync duration for checkpoint after allocating the transaction ID (usecs)', 'no_clear,no_scale'),
    TxnStat('txn_checkpoint_handle_applied', 'transaction checkpoint most recent handles applied'),
    TxnStat('txn_checkpoint_handle_skipped', 'transaction checkpoint most recent handles skipped'),
    TxnStat('txn_checkpoint_handle_walked', 'transaction checkpoint most recent handles walked'),
    TxnStat('txn_checkpoint_handle_duration', 'transaction checkpoint most recent duration for gathering all handles (usecs)', 'no_clear,no_scale'),
    TxnStat('txn_checkpoint_handle_duration_apply', 'transaction checkpoint most recent duration for gathering applied handles (usecs)', 'no_clear,no_scale'),
    TxnStat('txn_checkpoint_handle_duration_skip', 'transaction checkpoint most recent duration for gathering skipped handles (usecs)', 'no_clear,no_scale'),
    TxnStat('txn_hs_ckpt_duration', 'transaction checkpoint history store file duration (usecs)'),
    TxnStat('txn_checkpoint_generation', 'transaction checkpoint generation', 'no_clear,no_scale'),
    TxnStat('txn_checkpoint_prep_max', 'transaction checkpoint prepare max time (msecs)', 'no_clear,no_scale'),
    TxnStat('txn_checkpoint_prep_min', 'transaction checkpoint prepare min time (msecs)', 'no_clear,no_scale'),
    TxnStat('txn_checkpoint_prep_recent', 'transaction checkpoint prepare most recent time (msecs)', 'no_clear,no_scale'),
    TxnStat('txn_checkpoint_prep_running', 'transaction checkpoint prepare currently running', 'no_clear,no_scale'),
    TxnStat('txn_checkpoint_prep_total', 'transaction checkpoint prepare total time (msecs)', 'no_clear,no_scale'),
    TxnStat('txn_checkpoint_running', 'transaction checkpoint currently running', 'no_clear,no_scale'),
    TxnStat('txn_checkpoint_running_hs', 'transaction checkpoint currently running for history store file', 'no_clear,no_scale'),
    TxnStat('txn_checkpoint_scrub_target', 'transaction checkpoint scrub dirty target', 'no_clear,no_scale'),
    TxnStat('txn_checkpoint_scrub_time', 'transaction checkpoint scrub time (msecs)', 'no_clear,no_scale'),
    TxnStat('txn_checkpoint_skipped', 'transaction checkpoints skipped because database was clean'),
    TxnStat('txn_checkpoint_time_max', 'transaction checkpoint max time (msecs)', 'no_clear,no_scale'),
    TxnStat('txn_checkpoint_time_min', 'transaction checkpoint min time (msecs)', 'no_clear,no_scale'),
    TxnStat('txn_checkpoint_time_recent', 'transaction checkpoint most recent time (msecs)', 'no_clear,no_scale'),
    TxnStat('txn_checkpoint_time_total', 'transaction checkpoint total time (msecs)', 'no_clear,no_scale'),
    TxnStat('txn_commit', 'transactions committed'),
    TxnStat('txn_fail_cache', 'transaction failures due to history store'),
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
    TxnStat('txn_prepare_rollback_do_not_remove_hs_update', 'prepared transactions rolled back and do not remove the history store entry'),
    TxnStat('txn_prepare_rollback_fix_hs_update_with_ckpt_reserved_txnid', 'prepared transactions rolled back and fix the history store entry with checkpoint reserved transaction id'),
    TxnStat('txn_prepared_updates_committed', 'Number of prepared updates committed'),
    TxnStat('txn_prepared_updates', 'Number of prepared updates'),
    TxnStat('txn_prepared_updates_key_repeated', 'Number of prepared updates repeated on the same key'),
    TxnStat('txn_prepared_updates_rolledback', 'Number of prepared updates rolled back'),
    TxnStat('txn_query_ts', 'query timestamp calls'),
    TxnStat('txn_rollback', 'transactions rolled back'),
    TxnStat('txn_rollback_to_stable_running', 'transaction rollback to stable currently running', 'no_clear,no_scale'),
    TxnStat('txn_rts', 'rollback to stable calls'),
    TxnStat('txn_rts_pages_visited', 'rollback to stable pages visited'),
    TxnStat('txn_rts_tree_walk_skip_pages', 'rollback to stable tree walk skipping pages'),
    TxnStat('txn_rts_upd_aborted', 'rollback to stable updates aborted'),
    TxnStat('txn_sessions_walked', 'sessions scanned in each walk of concurrent sessions'),
    TxnStat('txn_set_ts', 'set timestamp calls'),
    TxnStat('txn_set_ts_durable', 'set timestamp durable calls'),
    TxnStat('txn_set_ts_durable_upd', 'set timestamp durable updates'),
    TxnStat('txn_set_ts_oldest', 'set timestamp oldest calls'),
    TxnStat('txn_set_ts_oldest_upd', 'set timestamp oldest updates'),
    TxnStat('txn_set_ts_stable', 'set timestamp stable calls'),
    TxnStat('txn_set_ts_stable_upd', 'set timestamp stable updates'),
    TxnStat('txn_timestamp_oldest_active_read', 'transaction read timestamp of the oldest active reader', 'no_clear,no_scale'),
    TxnStat('txn_walk_sessions', 'transaction walk of concurrent sessions'),

    ##########################################
    # Yield statistics
    ##########################################
    YieldStat('application_cache_time', 'application thread time waiting for cache (usecs)'),
    YieldStat('application_evict_time', 'application thread time evicting (usecs)'),
    YieldStat('child_modify_blocked_page', 'page reconciliation yielded due to child modification'),
    YieldStat('conn_close_blocked_lsm', 'connection close yielded for lsm manager shutdown'),
    YieldStat('dhandle_lock_blocked', 'data handle lock yielded'),
    YieldStat('page_busy_blocked', 'page acquire busy blocked'),
    YieldStat('page_del_rollback_blocked', 'page delete rollback time sleeping for state change (usecs)'),
    YieldStat('page_forcible_evict_blocked', 'page acquire eviction blocked'),
    YieldStat('page_index_slot_ref_blocked', 'get reference for page index and slot time sleeping (usecs)'),
    YieldStat('page_locked_blocked', 'page acquire locked blocked'),
    YieldStat('page_read_blocked', 'page acquire read blocked'),
    YieldStat('page_sleep', 'page acquire time sleeping (usecs)'),
    YieldStat('prepared_transition_blocked_page', 'page access yielded due to prepare state change'),
    YieldStat('txn_release_blocked', 'connection close blocked waiting for transaction state stabilization'),
]

conn_stats = sorted(conn_stats, key=attrgetter('desc'))

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
    BtreeStat('btree_clean_checkpoint_timer', 'btree clean tree checkpoint expiration time', 'no_clear,no_scale'),
    BtreeStat('btree_column_deleted', 'column-store variable-size deleted values', 'no_scale,tree_walk'),
    BtreeStat('btree_column_fix', 'column-store fixed-size leaf pages', 'no_scale,tree_walk'),
    BtreeStat('btree_column_internal', 'column-store internal pages', 'no_scale,tree_walk'),
    BtreeStat('btree_column_rle', 'column-store variable-size RLE encoded values', 'no_scale,tree_walk'),
    BtreeStat('btree_column_tws', 'column-store fixed-size time windows', 'no_scale,tree_walk'),
    BtreeStat('btree_column_variable', 'column-store variable-size leaf pages', 'no_scale,tree_walk'),
    BtreeStat('btree_compact_pages_reviewed', 'btree compact pages reviewed', 'no_clear,no_scale'),
    BtreeStat('btree_compact_pages_skipped', 'btree compact pages skipped', 'no_clear,no_scale'),
    BtreeStat('btree_compact_pages_rewritten', 'btree compact pages rewritten', 'no_clear,no_scale'),
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
    # Cache and eviction statistics
    ##########################################
    CacheStat('cache_eviction_fail', 'data source pages selected for eviction unable to be evicted'),
    CacheStat('cache_eviction_walk_passes', 'eviction walk passes of a file'),

    ##########################################
    # Cache content statistics
    ##########################################
    CacheWalkStat('cache_state_avg_unvisited_age', 'Average time in cache for pages that have not been visited by the eviction server', 'no_clear,no_scale'),
    CacheWalkStat('cache_state_avg_visited_age', 'Average time in cache for pages that have been visited by the eviction server', 'no_clear,no_scale'),
    CacheWalkStat('cache_state_avg_written_size', 'Average on-disk page image size seen', 'no_clear,no_scale'),
    CacheWalkStat('cache_state_gen_avg_gap', 'Average difference between current eviction generation when the page was last considered', 'no_clear,no_scale'),
    CacheWalkStat('cache_state_gen_current', 'Current eviction generation', 'no_clear,no_scale'),
    CacheWalkStat('cache_state_gen_max_gap', 'Maximum difference between current eviction generation when the page was last considered', 'no_clear,no_scale'),
    CacheWalkStat('cache_state_max_pagesize', 'Maximum page size seen', 'no_clear,no_scale'),
    CacheWalkStat('cache_state_memory', 'Pages created in memory and never written', 'no_clear,no_scale'),
    CacheWalkStat('cache_state_min_written_size', 'Minimum on-disk page image size seen', 'no_clear,no_scale'),
    CacheWalkStat('cache_state_not_queueable', 'Pages that could not be queued for eviction', 'no_clear,no_scale'),
    CacheWalkStat('cache_state_pages', 'Total number of pages currently in cache', 'no_clear,no_scale'),
    CacheWalkStat('cache_state_pages_clean', 'Clean pages currently in cache', 'no_clear,no_scale'),
    CacheWalkStat('cache_state_pages_dirty', 'Dirty pages currently in cache', 'no_clear,no_scale'),
    CacheWalkStat('cache_state_pages_internal', 'Internal pages currently in cache', 'no_clear,no_scale'),
    CacheWalkStat('cache_state_pages_leaf', 'Leaf pages currently in cache', 'no_clear,no_scale'),
    CacheWalkStat('cache_state_queued', 'Pages currently queued for eviction', 'no_clear,no_scale'),
    CacheWalkStat('cache_state_refs_skipped', 'Refs skipped during cache traversal', 'no_clear,no_scale'),
    CacheWalkStat('cache_state_root_entries', 'Entries in the root page', 'no_clear,no_scale'),
    CacheWalkStat('cache_state_root_size', 'Size of the root page', 'no_clear,no_scale'),
    CacheWalkStat('cache_state_smaller_alloc_size', 'On-disk page image sizes smaller than a single allocation unit', 'no_clear,no_scale'),
    CacheWalkStat('cache_state_unvisited_count', 'Number of pages never visited by eviction server', 'no_clear,no_scale'),

    ##########################################
    # Compression statistics
    ##########################################
    CompressStat('compress_hist_ratio_2', 'number of blocks with compress ratio smaller than 2'),
    CompressStat('compress_hist_ratio_4', 'number of blocks with compress ratio smaller than 4'),
    CompressStat('compress_hist_ratio_8', 'number of blocks with compress ratio smaller than 8'),
    CompressStat('compress_hist_ratio_16', 'number of blocks with compress ratio smaller than 16'),
    CompressStat('compress_hist_ratio_32', 'number of blocks with compress ratio smaller than 32'),
    CompressStat('compress_hist_ratio_64', 'number of blocks with compress ratio smaller than 64'),
    CompressStat('compress_hist_ratio_max', 'number of blocks with compress ratio greater than 64'),
    CompressStat('compress_precomp_intl_max_page_size', 'compressed page maximum internal page size prior to compression', 'no_clear,no_scale,size'),
    CompressStat('compress_precomp_leaf_max_page_size', 'compressed page maximum leaf page size prior to compression ', 'no_clear,no_scale,size'),
    CompressStat('compress_read', 'compressed pages read'),
    CompressStat('compress_write', 'compressed pages written'),
    CompressStat('compress_write_fail', 'page written failed to compress'),
    CompressStat('compress_write_too_small', 'page written was too small to compress'),

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
    # LSM statistics
    ##########################################
    LSMStat('bloom_count', 'bloom filters in the LSM tree', 'no_scale'),
    LSMStat('bloom_false_positive', 'bloom filter false positives'),
    LSMStat('bloom_hit', 'bloom filter hits'),
    LSMStat('bloom_miss', 'bloom filter misses'),
    LSMStat('bloom_page_evict', 'bloom filter pages evicted from cache'),
    LSMStat('bloom_page_read', 'bloom filter pages read into cache'),
    LSMStat('bloom_size', 'total size of bloom filters', 'no_scale,size'),
    LSMStat('lsm_chunk_count', 'chunks in the LSM tree', 'no_scale'),
    LSMStat('lsm_generation_max', 'highest merge generation in the LSM tree', 'max_aggregate,no_scale'),
    LSMStat('lsm_lookup_no_bloom', 'queries that could have benefited from a Bloom filter that did not exist'),

    ##########################################
    # Reconciliation statistics
    ##########################################
    RecStat('rec_dictionary', 'dictionary matches'),
    RecStat('rec_multiblock_internal', 'internal page multi-block writes'),
    RecStat('rec_multiblock_leaf', 'leaf page multi-block writes'),
    RecStat('rec_multiblock_max', 'maximum blocks required for a page', 'max_aggregate,no_scale'),
    RecStat('rec_overflow_key_leaf', 'leaf-page overflow keys'),
    RecStat('rec_overflow_value', 'overflow values written'),
    RecStat('rec_page_match', 'page checksum matches'),
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

dsrc_stats = sorted(dsrc_stats, key=attrgetter('desc'))

##########################################
# CONNECTION AND DATA SOURCE statistics
##########################################
conn_dsrc_stats = [
    ##########################################
    # Cache and eviction statistics
    ##########################################
    CacheStat('cache_bytes_dirty', 'tracked dirty bytes in the cache', 'no_clear,no_scale,size'),
    CacheStat('cache_bytes_dirty_total', 'bytes dirty in the cache cumulative', 'no_clear,no_scale,size'),
    CacheStat('cache_bytes_inuse', 'bytes currently in the cache', 'no_clear,no_scale,size'),
    CacheStat('cache_bytes_read', 'bytes read into cache', 'size'),
    CacheStat('cache_bytes_write', 'bytes written from cache', 'size'),
    CacheStat('cache_eviction_checkpoint', 'checkpoint blocked page eviction'),
    CacheStat('cache_eviction_blocked_checkpoint_hs', 'checkpoint of history store file blocked non-history store page eviction'),
    CacheStat('cache_eviction_blocked_ooo_checkpoint_race_1', 'eviction gave up due to detecting an out of order on disk value behind the last update on the chain'),
    CacheStat('cache_eviction_blocked_ooo_checkpoint_race_2', 'eviction gave up due to detecting an out of order tombstone ahead of the selected on disk update'),
    CacheStat('cache_eviction_blocked_ooo_checkpoint_race_3', 'eviction gave up due to detecting an out of order tombstone ahead of the selected on disk update after validating the update chain'),
    CacheStat('cache_eviction_blocked_ooo_checkpoint_race_4', 'eviction gave up due to detecting out of order timestamps on the update chain after the selected on disk update'),
    CacheStat('cache_eviction_clean', 'unmodified pages evicted'),
    CacheStat('cache_eviction_deepen', 'page split during eviction deepened the tree'),
    CacheStat('cache_eviction_dirty', 'modified pages evicted'),
    CacheStat('cache_eviction_hazard', 'hazard pointer blocked page eviction'),
    CacheStat('cache_eviction_internal', 'internal pages evicted'),
    CacheStat('cache_eviction_pages_seen', 'pages seen by eviction walk'),
    CacheStat('cache_eviction_split_internal', 'internal pages split during eviction'),
    CacheStat('cache_eviction_split_leaf', 'leaf pages split during eviction'),
    CacheStat('cache_eviction_target_page_ge128', 'eviction walk target pages histogram - 128 and higher'),
    CacheStat('cache_eviction_target_page_lt10', 'eviction walk target pages histogram - 0-9'),
    CacheStat('cache_eviction_target_page_lt128', 'eviction walk target pages histogram - 64-128'),
    CacheStat('cache_eviction_target_page_lt32', 'eviction walk target pages histogram - 10-31'),
    CacheStat('cache_eviction_target_page_lt64', 'eviction walk target pages histogram - 32-63'),
    CacheStat('cache_eviction_target_page_reduced', 'eviction walk target pages reduced due to history store cache pressure'),
    CacheStat('cache_eviction_walk_from_root', 'eviction walks started from root of tree'),
    CacheStat('cache_eviction_walk_restart', 'eviction walks restarted'),
    CacheStat('cache_eviction_walk_saved_pos', 'eviction walks started from saved location in tree'),
    CacheStat('cache_eviction_walks_abandoned', 'eviction walks abandoned'),
    CacheStat('cache_eviction_walks_ended', 'eviction walks reached end of tree'),
    CacheStat('cache_eviction_walks_gave_up_no_targets', 'eviction walks gave up because they saw too many pages and found no candidates'),
    CacheStat('cache_eviction_walks_gave_up_ratio', 'eviction walks gave up because they saw too many pages and found too few candidates'),
    CacheStat('cache_eviction_walks_stopped', 'eviction walks gave up because they restarted their walk twice'),
    CacheStat('cache_hs_insert', 'history store table insert calls'),
    CacheStat('cache_hs_insert_full_update', 'the number of times full update inserted to history store'),
    CacheStat('cache_hs_insert_restart', 'history store table insert calls that returned restart'),
    CacheStat('cache_hs_insert_reverse_modify', 'the number of times reverse modify inserted to history store'),
    CacheStat('cache_hs_key_truncate', 'history store table truncation to remove an update'),
    CacheStat('cache_hs_key_truncate_onpage_removal', 'history store table truncation to remove range of updates due to key being removed from the data page during reconciliation'),
    CacheStat('cache_hs_order_remove', 'history store table truncation to remove range of updates due to out-of-order timestamp update on data page'),
    CacheStat('cache_hs_key_truncate_rts', 'history store table truncation by rollback to stable to remove an update'),
    CacheStat('cache_hs_key_truncate_rts_unstable', 'history store table truncation by rollback to stable to remove an unstable update'),
    CacheStat('cache_hs_order_reinsert', 'history store table out-of-order updates that were fixed up by reinserting with the fixed timestamp'),
    CacheStat('cache_hs_order_lose_durable_timestamp', 'history store table out-of-order resolved updates that lose their durable timestamp'),
    CacheStat('cache_hs_read', 'history store table reads'),
    CacheStat('cache_hs_read_miss', 'history store table reads missed'),
    CacheStat('cache_hs_read_squash', 'history store table reads requiring squashed modifies'),
    CacheStat('cache_hs_write_squash', 'history store table writes requiring squashed modifies'),
    CacheStat('cache_inmem_split', 'in-memory page splits'),
    CacheStat('cache_inmem_splittable', 'in-memory page passed criteria to be split'),
    CacheStat('cache_pages_requested', 'pages requested from the cache'),
    CacheStat('cache_read', 'pages read into cache'),
    CacheStat('cache_read_deleted', 'pages read into cache after truncate'),
    CacheStat('cache_read_deleted_prepared', 'pages read into cache after truncate in prepare state'),
    CacheStat('cache_read_overflow', 'overflow pages read into cache'),
    CacheStat('cache_write', 'pages written from cache'),
    CacheStat('cache_write_hs', 'page written requiring history store records'),
    CacheStat('cache_write_restore', 'pages written requiring in-memory restoration'),
    ##########################################
    # Cursor operations
    ##########################################
    CursorStat('cursor_next_hs_tombstone', 'cursor next calls that skip due to a globally visible history store tombstone'),
    CursorStat('cursor_next_skip_ge_100', 'cursor next calls that skip greater than or equal to 100 entries'),
    CursorStat('cursor_next_skip_lt_100', 'cursor next calls that skip less than 100 entries'),
    CursorStat('cursor_next_skip_total', 'Total number of entries skipped by cursor next calls'),
    CursorStat('cursor_open_count', 'open cursor count', 'no_clear,no_scale'),
    CursorStat('cursor_prev_hs_tombstone', 'cursor prev calls that skip due to a globally visible history store tombstone'),
    CursorStat('cursor_prev_skip_ge_100', 'cursor prev calls that skip greater than or equal to 100 entries'),
    CursorStat('cursor_prev_skip_lt_100', 'cursor prev calls that skip less than 100 entries'),
    CursorStat('cursor_prev_skip_total', 'Total number of entries skipped by cursor prev calls'),
    CursorStat('cursor_search_near_prefix_fast_paths', 'Total number of times a search near has exited due to prefix config'),
    CursorStat('cursor_skip_hs_cur_position', 'Total number of entries skipped to position the history store cursor'),
    ##########################################
    # Checkpoint cleanup statistics
    ##########################################
    CheckpointCleanupStat('cc_pages_evict', 'pages added for eviction'),
    CheckpointCleanupStat('cc_pages_removed', 'pages removed'),
    CheckpointCleanupStat('cc_pages_visited', 'pages visited'),
    CheckpointCleanupStat('cc_pages_walk_skipped', 'pages skipped during tree walk'),

    ##########################################
    # LSM statistics
    ##########################################
    LSMStat('lsm_checkpoint_throttle', 'sleep for LSM checkpoint throttle'),
    LSMStat('lsm_merge_throttle', 'sleep for LSM merge throttle'),

    ##########################################
    # Reconciliation statistics
    ##########################################
    RecStat('rec_page_delete', 'pages deleted'),
    RecStat('rec_page_delete_fast', 'fast-path pages deleted'),
    RecStat('rec_pages', 'page reconciliation calls'),
    RecStat('rec_pages_eviction', 'page reconciliation calls for eviction'),
    RecStat('rec_time_aggr_newest_start_durable_ts', 'pages written including an aggregated newest start durable timestamp '),
    RecStat('rec_time_aggr_newest_stop_durable_ts', 'pages written including an aggregated newest stop durable timestamp '),
    RecStat('rec_time_aggr_newest_stop_ts', 'pages written including an aggregated newest stop timestamp '),
    RecStat('rec_time_aggr_newest_stop_txn', 'pages written including an aggregated newest stop transaction ID'),
    RecStat('rec_time_aggr_oldest_start_ts', 'pages written including an aggregated oldest start timestamp '),
    RecStat('rec_time_aggr_newest_txn', 'pages written including an aggregated newest transaction ID '),
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

    ##########################################
    # Tiered storage statistics
    ##########################################
    StorageStat('tiered_object_size', 'tiered storage object size', 'no_clear,no_scale,size'),
    StorageStat('tiered_retention', 'tiered storage local retention time (secs)', 'no_clear,no_scale,size'),
    StorageStat('tiered_work_units_created', 'tiered operations scheduled'),
    StorageStat('tiered_work_units_dequeued', 'tiered operations dequeued and processed'),

    ##########################################
    # Transaction statistics
    ##########################################
    TxnStat('txn_checkpoint_obsolete_applied', 'transaction checkpoints due to obsolete pages'),
    TxnStat('txn_read_race_prepare_update', 'race to read prepared update retry'),
    TxnStat('txn_rts_delete_rle_skipped', 'rollback to stable skipping delete rle'),
    TxnStat('txn_rts_hs_removed', 'rollback to stable updates removed from history store'),
    TxnStat('txn_rts_hs_restore_updates', 'rollback to stable restored updates from history store'),
    TxnStat('txn_rts_hs_restore_tombstones', 'rollback to stable restored tombstones from history store'),
    TxnStat('txn_rts_hs_stop_older_than_newer_start', 'rollback to stable history store records with stop timestamps older than newer records'),
    TxnStat('txn_rts_inconsistent_ckpt', 'rollback to stable inconsistent checkpoint'),
    TxnStat('txn_rts_keys_removed', 'rollback to stable keys removed'),
    TxnStat('txn_rts_keys_restored', 'rollback to stable keys restored'),
    TxnStat('txn_rts_stable_rle_skipped', 'rollback to stable skipping stable rle'),
    TxnStat('txn_rts_sweep_hs_keys', 'rollback to stable sweeping history store keys'),
    TxnStat('txn_update_conflict', 'update conflicts'),
]

conn_dsrc_stats = sorted(conn_dsrc_stats, key=attrgetter('desc'))

##########################################
# Cursor Join statistics
##########################################
join_stats = [
    JoinStat('bloom_false_positive', 'bloom filter false positives'),
    JoinStat('bloom_insert', 'items inserted into a bloom filter'),
    JoinStat('iterated', 'items iterated'),
    JoinStat('main_access', 'accesses to the main table'),
    JoinStat('membership_check', 'checks that conditions of membership are satisfied'),
]

join_stats = sorted(join_stats, key=attrgetter('desc'))

##########################################
# Session statistics
##########################################
session_stats = [
    SessionStat('bytes_read', 'bytes read into cache'),
    SessionStat('bytes_write', 'bytes written from cache'),
    SessionStat('cache_time', 'time waiting for cache (usecs)'),
    SessionStat('lock_dhandle_wait', 'dhandle lock wait time (usecs)'),
    SessionStat('lock_schema_wait', 'schema lock wait time (usecs)'),
    SessionStat('read_time', 'page read from disk to cache time (usecs)'),
    SessionStat('write_time', 'page write from cache to disk time (usecs)'),
]

session_stats = sorted(session_stats, key=attrgetter('desc'))
