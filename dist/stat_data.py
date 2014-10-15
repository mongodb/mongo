# Auto-generate statistics #defines, with initialization, clear and aggregate
# functions.
#
# NOTE: Statistics reports show individual objects as operations per second.
# All objects where that does not make sense should have the word 'currently'
# or the phrase 'in the cache' in their text description, for example, 'files
# currently open'.
#
# Optional configuration flags:
#	no_clear	Value ignored by the statistics refresh function
#	no_aggregate	Ignore the value when aggregating statistics
#	max_aggregate	Take the maximum value when aggregating statistics
#	no_scale	Don't scale value per second in the logging tool script

from operator import attrgetter

class Stat:
	def __init__(self, name, desc, flags=''):
		self.name = name
		self.desc = desc
		self.flags = flags

	def __cmp__(self, other):
		return cmp(self.name, other.name)

##########################################
# CONNECTION statistics
##########################################
connection_stats = [
	##########################################
	# System statistics
	##########################################
	Stat('cond_wait', 'pthread mutex condition wait calls'),
	Stat('file_open', 'files currently open', 'no_clear,no_scale'),
	Stat('memory_allocation', 'memory allocations'),
	Stat('memory_free', 'memory frees'),
	Stat('memory_grow', 'memory re-allocations'),
	Stat('read_io', 'total read I/Os'),
	Stat('rwlock_read', 'pthread mutex shared lock read-lock calls'),
	Stat('rwlock_write', 'pthread mutex shared lock write-lock calls'),
	Stat('write_io', 'total write I/Os'),

	##########################################
	# Async API statistics
	##########################################
	Stat('async_alloc_race', 'async: number of allocation state races'),
	Stat('async_alloc_view', 'async: number of op slots viewed for alloc'),
	Stat('async_flush', 'async: number of async flush calls'),
	Stat('async_full', 'async: number of times op allocation failed'),
	Stat('async_cur_queue', 'async: current work queue length'),
	Stat('async_max_queue', 'async: maximum work queue length',
	    'max_aggregate,no_scale'),
	Stat('async_nowork', 'async: number of times worker found no work'),
	Stat('async_op_alloc', 'async op allocation'),
	Stat('async_op_compact', 'async op compact calls'),
	Stat('async_op_insert', 'async op insert calls'),
	Stat('async_op_remove', 'async op remove calls'),
	Stat('async_op_search', 'async op search calls'),
	Stat('async_op_update', 'async op update calls'),

	##########################################
	# Block manager statistics
	##########################################
	Stat('block_byte_map_read', 'block manager: mapped bytes read'),
	Stat('block_byte_read', 'block manager: bytes read'),
	Stat('block_byte_write', 'block manager: bytes written'),
	Stat('block_map_read', 'block manager: mapped blocks read'),
	Stat('block_preload', 'block manager: blocks pre-loaded'),
	Stat('block_read', 'block manager: blocks read'),
	Stat('block_write', 'block manager: blocks written'),

	##########################################
	# Cache and eviction statistics
	##########################################
	Stat('cache_bytes_dirty',
	    'cache: tracked dirty bytes in the cache', 'no_scale'),
	Stat('cache_bytes_inuse',
	    'cache: bytes currently in the cache', 'no_clear,no_scale'),
	Stat('cache_bytes_max',
	    'cache: maximum bytes configured', 'no_clear,no_scale'),
	Stat('cache_bytes_read', 'cache: bytes read into cache'),
	Stat('cache_bytes_write', 'cache: bytes written from cache'),
	Stat('cache_eviction_clean', 'cache: unmodified pages evicted'),
	Stat('cache_eviction_deepen',
	    'cache: page split during eviction deepened the tree'),
	Stat('cache_eviction_dirty', 'cache: modified pages evicted'),
	Stat('cache_eviction_checkpoint',
	    'cache: checkpoint blocked page eviction'),
	Stat('cache_eviction_fail',
	    'cache: pages selected for eviction unable to be evicted'),
	Stat('cache_eviction_force',
	    'cache: pages evicted because they exceeded the in-memory maximum'),
	Stat('cache_eviction_force_fail',
	    'cache: failed eviction of pages that exceeded the ' +
	    'in-memory maximum'),
	Stat('cache_eviction_hazard',
	    'cache: hazard pointer blocked page eviction'),
	Stat('cache_eviction_internal', 'cache: internal pages evicted'),
	Stat('cache_eviction_queue_empty',
	    'cache: eviction server candidate queue empty when topping up'),
	Stat('cache_eviction_queue_not_empty',
	    'cache: eviction server candidate queue not empty when topping up'),
	Stat('cache_eviction_server_evicting',
	    'cache: eviction server evicting pages'),
	Stat('cache_eviction_server_not_evicting',
	    'cache: eviction server populating queue, but not evicting pages'),
	Stat('cache_eviction_slow',
	    'cache: eviction server unable to reach eviction goal'),
	Stat('cache_eviction_split', 'cache: pages split during eviction'),
	Stat('cache_eviction_walk', 'cache: pages walked for eviction'),
	Stat('cache_pages_dirty',
	    'cache: tracked dirty pages in the cache', 'no_scale'),
	Stat('cache_pages_inuse',
	    'cache: pages currently held in the cache', 'no_clear,no_scale'),
	Stat('cache_read', 'cache: pages read into cache'),
	Stat('cache_write', 'cache: pages written from cache'),

	##########################################
	# Dhandle statistics
	##########################################
	Stat('dh_session_handles', 'dhandle: session dhandles swept'),
	Stat('dh_session_sweeps', 'dhandle: session sweep attempts'),

	##########################################
	# Logging statistics
	##########################################
	Stat('log_buffer_grow',
	    'log: log buffer size increases'),
	Stat('log_buffer_size',
	    'log: total log buffer size', 'no_clear,no_scale'),
	Stat('log_bytes_user', 'log: user provided log bytes written'),
	Stat('log_bytes_written', 'log: log bytes written'),
	Stat('log_max_filesize', 'log: maximum log file size', 'no_clear'),
	Stat('log_reads', 'log: log read operations'),
	Stat('log_scan_records', 'log: records processed by log scan'),
	Stat('log_scan_rereads', 'log: log scan records requiring two reads'),
	Stat('log_scans', 'log: log scan operations'),
	Stat('log_sync', 'log: log sync operations'),
	Stat('log_writes', 'log: log write operations'),

	Stat('log_slot_consolidated', 'log: logging bytes consolidated'),
	Stat('log_slot_closes', 'log: consolidated slot closures'),
	Stat('log_slot_joins', 'log: consolidated slot joins'),
	Stat('log_slot_races', 'log: consolidated slot join races'),
	Stat('log_slot_switch_fails',
	    'log: slots selected for switching that were unavailable'),
	Stat('log_slot_toobig', 'log: record size exceeded maximum'),
	Stat('log_slot_toosmall',
	    'log: failed to find a slot large enough for record'),
	Stat('log_slot_transitions', 'log: consolidated slot join transitions'),

	##########################################
	# Reconciliation statistics
	##########################################
	Stat('rec_pages', 'page reconciliation calls'),
	Stat('rec_pages_eviction', 'page reconciliation calls for eviction'),
	Stat('rec_skipped_update',
	    'reconciliation failed because an update could not be included'),
	Stat('rec_split_stashed_bytes',
	    'split bytes currently awaiting free', 'no_clear,no_scale'),
	Stat('rec_split_stashed_objects',
	    'split objects currently awaiting free', 'no_clear,no_scale'),

	##########################################
	# Transaction statistics
	##########################################
	Stat('txn_begin', 'transactions'),
	Stat('txn_checkpoint', 'transaction checkpoints'),
	Stat('txn_checkpoint_running',
	    'transaction checkpoint currently running',
	    'no_aggregate,no_clear,no_scale'),
	Stat('txn_commit', 'transactions committed'),
	Stat('txn_fail_cache', 'transaction failures due to cache overflow'),
	Stat('txn_rollback', 'transactions rolled back'),

	##########################################
	# LSM statistics
	##########################################
	Stat('lsm_checkpoint_throttle',
	    'sleep for LSM checkpoint throttle'),
	Stat('lsm_merge_throttle', 'sleep for LSM merge throttle'),
	Stat('lsm_rows_merged', 'rows merged in an LSM tree'),
	Stat('lsm_work_queue_app', 'LSM App work units currently queued',
            'no_clear,no_scale'),
	Stat('lsm_work_queue_manager', 'LSM Merge work units currently queued',
            'no_clear,no_scale'),
	Stat('lsm_work_queue_max', 'LSM tree queue hit maximum'),
	Stat('lsm_work_queue_switch', 'LSM Switch work units currently queued',
            'no_clear,no_scale'),
	Stat('lsm_work_units_created',
	    'LSM tree maintenance operations scheduled'),
	Stat('lsm_work_units_discarded',
	    'LSM tree maintenance operations discarded'),
	Stat('lsm_work_units_done', 'LSM tree maintenance operations executed'),

	##########################################
	# Session operations
	##########################################
	Stat('session_cursor_open', 'open cursor count', 'no_clear,no_scale'),

	##########################################
	# Total Btree cursor operations
	##########################################
	Stat('cursor_create', 'cursor creation'),
	Stat('cursor_insert', 'Btree cursor insert calls'),
	Stat('cursor_next', 'Btree cursor next calls'),
	Stat('cursor_prev', 'Btree cursor prev calls'),
	Stat('cursor_remove', 'Btree cursor remove calls'),
	Stat('cursor_reset', 'Btree cursor reset calls'),
	Stat('cursor_search', 'Btree cursor search calls'),
	Stat('cursor_search_near', 'Btree cursor search near calls'),
	Stat('cursor_update', 'Btree cursor update calls'),
]

connection_stats = sorted(connection_stats, key=attrgetter('name'))

##########################################
# Data source statistics
##########################################
dsrc_stats = [
	##########################################
	# Session operations
	##########################################
	Stat('session_compact', 'object compaction'),
	Stat('session_cursor_open', 'open cursor count', 'no_clear,no_scale'),

	##########################################
	# Cursor operations
	##########################################
	Stat('cursor_create', 'cursor creation'),
	Stat('cursor_insert', 'cursor insert calls'),
	Stat('cursor_insert_bulk', 'bulk-loaded cursor-insert calls'),
	Stat('cursor_insert_bytes',
	    'cursor-insert key and value bytes inserted'),
	Stat('cursor_next', 'cursor next calls'),
	Stat('cursor_prev', 'cursor prev calls'),
	Stat('cursor_remove', 'cursor remove calls'),
	Stat('cursor_remove_bytes', 'cursor-remove key bytes removed'),
	Stat('cursor_reset', 'cursor reset calls'),
	Stat('cursor_search', 'cursor search calls'),
	Stat('cursor_search_near', 'cursor search near calls'),
	Stat('cursor_update', 'cursor update calls'),
	Stat('cursor_update_bytes', 'cursor-update value bytes updated'),

	##########################################
	# Btree statistics
	##########################################
	Stat('btree_column_deleted',
	    'column-store variable-size deleted values', 'no_scale'),
	Stat('btree_column_fix',
	    'column-store fixed-size leaf pages', 'no_scale'),
	Stat('btree_column_internal',
	    'column-store internal pages', 'no_scale'),
	Stat('btree_column_variable',
	    'column-store variable-size leaf pages', 'no_scale'),
	Stat('btree_compact_rewrite', 'pages rewritten by compaction'),
	Stat('btree_entries', 'number of key/value pairs', 'no_scale'),
	Stat('btree_fixed_len', 'fixed-record size', 'no_aggregate,no_scale'),
	Stat('btree_maximum_depth',
	    'maximum tree depth', 'max_aggregate,no_scale'),
	Stat('btree_maxintlitem',
	    'maximum internal page item size', 'no_aggregate,no_scale'),
	Stat('btree_maxintlpage',
	    'maximum internal page size', 'no_aggregate,no_scale'),
	Stat('btree_maxleafitem',
	    'maximum leaf page item size', 'no_aggregate,no_scale'),
	Stat('btree_maxleafpage',
	    'maximum leaf page size', 'no_aggregate,no_scale'),
	Stat('btree_overflow', 'overflow pages', 'no_scale'),
	Stat('btree_row_internal', 'row-store internal pages', 'no_scale'),
	Stat('btree_row_leaf', 'row-store leaf pages', 'no_scale'),

	##########################################
	# LSM statistics
	##########################################
	Stat('bloom_count', 'bloom filters in the LSM tree', 'no_scale'),
	Stat('bloom_false_positive', 'bloom filter false positives'),
	Stat('bloom_hit', 'bloom filter hits'),
	Stat('bloom_miss', 'bloom filter misses'),
	Stat('bloom_page_evict',
	    'bloom filter pages evicted from cache'),
	Stat('bloom_page_read', 'bloom filter pages read into cache'),
	Stat('bloom_size', 'total size of bloom filters', 'no_scale'),
	Stat('lsm_checkpoint_throttle',
	    'sleep for LSM checkpoint throttle'),
	Stat('lsm_chunk_count',
	    'chunks in the LSM tree', 'no_aggregate,no_scale'),
	Stat('lsm_generation_max',
	    'highest merge generation in the LSM tree',
	    'max_aggregate,no_scale'),
	Stat('lsm_lookup_no_bloom',
	    'queries that could have benefited ' +
	    'from a Bloom filter that did not exist'),
	Stat('lsm_merge_throttle', 'sleep for LSM merge throttle'),

	##########################################
	# Block manager statistics
	##########################################
	Stat('block_alloc', 'block manager: blocks allocated'),
	Stat('allocation_size',
	    'block manager: file allocation unit size',
	    'no_aggregate,no_scale'),
	Stat('block_checkpoint_size',
	    'block manager: checkpoint size', 'no_scale'),
	Stat('block_extension',
	    'block manager: allocations requiring file extension'),
	Stat('block_free', 'block manager: blocks freed'),
	Stat('block_magic',
	    'block manager: file magic number', 'no_aggregate,no_scale'),
	Stat('block_major', 'block manager: file major version number',
	    'no_aggregate,no_scale'),
	Stat('block_minor',
	    'block manager: minor version number', 'no_aggregate,no_scale'),
	Stat('block_reuse_bytes',
	    'block manager: file bytes available for reuse'),
	Stat('block_size', 'block manager: file size in bytes', 'no_scale'),

	##########################################
	# Cache and eviction statistics
	##########################################
	Stat('cache_bytes_read', 'bytes read into cache'),
	Stat('cache_bytes_write', 'bytes written from cache'),
	Stat('cache_eviction_clean', 'unmodified pages evicted'),
	Stat('cache_eviction_checkpoint',
	    'cache: checkpoint blocked page eviction'),
	Stat('cache_eviction_dirty', 'modified pages evicted'),
	Stat('cache_eviction_fail',
	    'data source pages selected for eviction unable to be evicted'),
	Stat('cache_eviction_hazard',
	    'cache: hazard pointer blocked page eviction'),
	Stat('cache_eviction_internal', 'internal pages evicted'),
	Stat('cache_overflow_value',
	    'overflow values cached in memory', 'no_scale'),
	Stat('cache_read', 'pages read into cache'),
	Stat('cache_read_overflow', 'overflow pages read into cache'),
	Stat('cache_write', 'pages written from cache'),

	##########################################
	# Compression statistics
	##########################################
	Stat('compress_raw_ok', 'raw compression call succeeded'),
	Stat('compress_raw_fail',
	    'raw compression call failed, no additional data available'),
	Stat('compress_raw_fail_temporary',
	    'raw compression call failed, additional data available'),
	Stat('compress_read', 'compressed pages read'),
	Stat('compress_write', 'compressed pages written'),
	Stat('compress_write_fail', 'page written failed to compress'),
	Stat('compress_write_too_small',
	    'page written was too small to compress'),

	##########################################
	# Reconciliation statistics
	##########################################
	Stat('rec_dictionary', 'reconciliation dictionary matches'),
	Stat('rec_overflow_key_internal',
	    'reconciliation internal-page overflow keys'),
	Stat('rec_overflow_key_leaf', 'reconciliation leaf-page overflow keys'),
	Stat('rec_overflow_value', 'reconciliation overflow values written'),
	Stat('rec_page_match', 'reconciliation page checksum matches'),
	Stat('rec_page_delete', 'reconciliation pages deleted'),
	Stat('rec_pages', 'page reconciliation calls'),
	Stat('rec_pages_eviction', 'page reconciliation calls for eviction'),
	Stat('rec_prefix_compression',
	    'leaf page key bytes discarded using prefix compression'),
	Stat('rec_suffix_compression',
	    'internal page key bytes discarded using suffix compression'),
	Stat('rec_skipped_update',
	    'reconciliation failed because an update could not be included'),
	Stat('rec_multiblock_internal',
	    'reconciliation internal page multi-block writes'),
	Stat('rec_multiblock_leaf',
	    'reconciliation leaf page multi-block writes'),
	Stat('rec_multiblock_max',
	    'reconciliation maximum blocks required for a page',
	    'max_aggregate,no_scale'),

	##########################################
	# Transaction statistics
	##########################################
	Stat('txn_update_conflict', 'update conflicts'),
]

dsrc_stats = sorted(dsrc_stats, key=attrgetter('name'))
