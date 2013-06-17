# Auto-generate statistics #defines, with initialization, clear and aggregate
# functions.
#
# NOTE: Statistics reports show individual objects as operations per second.
# All objects where that does not make sense should have the word 'currently'
# or the phrase 'in the cache' in their text description, for example, 'files
# currently open'.
#
# Optional configuration flags:
#	no_clear	Value ignored by the statistics clear function
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
	Stat('memory_allocation', 'total heap memory allocations'),
	Stat('memory_free', 'total heap memory frees'),
	Stat('memory_grow', 'total heap memory re-allocations'),
	Stat('read_io', 'total read I/Os'),
	Stat('rwlock_read', 'pthread mutex shared lock read-lock calls'),
	Stat('rwlock_write', 'pthread mutex shared lock write-lock calls'),
	Stat('write_io', 'total write I/Os'),

	##########################################
	# Block manager statistics
	##########################################
	Stat('block_byte_map_read', 'mapped bytes read by the block manager'),
	Stat('block_byte_read', 'bytes read by the block manager'),
	Stat('block_byte_write', 'bytes written by the block manager'),
	Stat('block_map_read', 'mapped blocks read by the block manager'),
	Stat('block_preload', 'blocks pre-loaded by the block manager'),
	Stat('block_read', 'blocks read by the block manager'),
	Stat('block_write', 'blocks written by the block manager'),

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
	Stat('cache_eviction_dirty', 'cache: modified pages evicted'),
	Stat('cache_eviction_checkpoint',
	    'cache: checkpoint blocked page eviction'),
	Stat('cache_eviction_fail',
	    'cache: pages selected for eviction unable to be evicted'),
	Stat('cache_eviction_hazard',
	    'cache: hazard pointer blocked page eviction'),
	Stat('cache_eviction_internal', 'cache: internal pages evicted'),
	Stat('cache_eviction_merge',
	    'cache: internal page merge operations completed'),
	Stat('cache_eviction_merge_fail',
	    'cache: internal page merge attempts that could not complete'),
	Stat('cache_eviction_merge_levels', 'cache: internal levels merged'),
	Stat('cache_eviction_slow',
	    'cache: eviction server unable to reach eviction goal'),
	Stat('cache_eviction_walk', 'cache: pages walked for eviction'),
	Stat('cache_pages_dirty',
	    'cache: tracked dirty pages in the cache', 'no_scale'),
	Stat('cache_pages_inuse',
	    'cache: pages currently held in the cache', 'no_clear,no_scale'),
	Stat('cache_read', 'cache: pages read into cache'),
	Stat('cache_write', 'cache: pages written from cache'),

	##########################################
	# Logging statistics
	##########################################
	Stat('log_bytes_written', 'log: total log bytes written'),
	Stat('log_max_filesize', 'log: maximum log file size', 'no_clear'),
	Stat('log_reads', 'log: total log read operations'),
	Stat('log_scan_records', 'log: total records processed by log scan'),
	Stat('log_scans', 'log: total log scan operations'),
	Stat('log_sync', 'log: total log sync operations'),
	Stat('log_writes', 'log: total log write operations'),

	Stat('log_slot_consolidated', 'log: total logging bytes consolidated'),
	Stat('log_slot_closes', 'log: total consolidated slot closures'),
	Stat('log_slot_joins', 'log: total consolidated slot joins'),
	Stat('log_slot_races', 'log: total consolidated slot join races'),
	Stat('log_slot_toobig', 'log: record size exceeded maximum'),
	Stat('log_slot_transitions',
            'log: total consolidated slot join transitions'),

	##########################################
	# Reconciliation statistics
	##########################################
	Stat('rec_pages', 'page reconciliation calls'),
	Stat('rec_pages_eviction', 'page reconciliation calls for eviction'),
	Stat('rec_skipped_update',
	    'reconciliation failed because an update could not be included'),

	##########################################
	# Transaction statistics
	##########################################
	Stat('txn_begin', 'transactions'),
	Stat('txn_checkpoint', 'transaction checkpoints'),
	Stat('txn_commit', 'transactions committed'),
	Stat('txn_fail_cache', 'transaction failures due to cache overflow'),
	Stat('txn_rollback', 'transactions rolled-back'),

	##########################################
	# LSM statistics
	##########################################
	Stat('lsm_rows_merged', 'rows merged in an LSM tree'),

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
	Stat('btree_entries',
	    'total LSM, table or file object key/value pairs', 'no_scale'),
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
	Stat('lsm_chunk_count',
	    'chunks in the LSM tree', 'no_aggregate,no_scale'),
	Stat('lsm_generation_max',
	    'highest merge generation in the LSM tree',
	    'max_aggregate,no_scale'),
	Stat('lsm_lookup_no_bloom',
	    'queries that could have benefited ' +
	    'from a Bloom filter that did not exist'),

	##########################################
	# Block manager statistics
	##########################################
	Stat('block_alloc', 'blocks allocated'),
	Stat('block_allocsize',
	    'block manager file allocation unit size', 'no_aggregate,no_scale'),
	Stat('block_checkpoint_size', 'checkpoint size', 'no_scale'),
	Stat('block_extension',
	    'block allocations requiring file extension'),
	Stat('block_free', 'blocks freed'),
	Stat('block_magic', 'file magic number', 'no_aggregate,no_scale'),
	Stat('block_major',
	    'file major version number', 'no_aggregate,no_scale'),
	Stat('block_minor', 'minor version number', 'no_aggregate,no_scale'),
	Stat('block_size', 'block manager file size in bytes', 'no_scale'),

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
	Stat('cache_eviction_merge',
	    'cache: internal page merge operations completed'),
	Stat('cache_eviction_merge_fail',
	    'cache: internal page merge attempts that could not complete'),
	Stat('cache_eviction_merge_levels', 'cache: internal levels merged'),
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
	Stat('rec_ovfl_key', 'reconciliation overflow keys written'),
	Stat('rec_ovfl_value', 'reconciliation overflow values written'),
	Stat('rec_page_delete', 'reconciliation pages deleted'),
	Stat('rec_page_merge', 'reconciliation pages merged'),
	Stat('rec_pages', 'page reconciliation calls'),
	Stat('rec_pages_eviction', 'page reconciliation calls for eviction'),
	Stat('rec_skipped_update',
	    'reconciliation failed because an update could not be included'),
	Stat('rec_split_intl', 'reconciliation internal pages split'),
	Stat('rec_split_leaf', 'reconciliation leaf pages split'),
	Stat('rec_split_max',
	    'reconciliation maximum number of splits created for a page',
	    'max_aggregate,no_scale'),

	##########################################
	# Transaction statistics
	##########################################
	Stat('txn_update_conflict', 'update conflicts'),
	Stat('txn_write_conflict', 'write generation conflicts'),
]

dsrc_stats = sorted(dsrc_stats, key=attrgetter('name'))
