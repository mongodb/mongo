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
#	scale		Scale value per second in the logging tool script

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
	Stat('cond_wait', 'pthread mutex condition wait calls', 'scale'),
	Stat('file_open', 'files currently open', 'no_clear'),
	Stat('memory_allocation', 'total heap memory allocations', 'scale'),
	Stat('memory_free', 'total heap memory frees', 'scale'),
	Stat('memory_grow', 'total heap memory re-allocations', 'scale'),
	Stat('read_io', 'total read I/Os', 'scale'),
	Stat('rwlock_read',
	    'pthread mutex shared lock read-lock calls', 'scale'),
	Stat('rwlock_write',
	    'pthread mutex shared lock write-lock calls', 'scale'),
	Stat('write_io', 'total write I/Os', 'scale'),

	##########################################
	# Block manager statistics
	##########################################
	Stat('block_byte_map_read',
	    'mapped bytes read by the block manager', 'scale'),
	Stat('block_byte_read', 'bytes read by the block manager', 'scale'),
	Stat('block_byte_write', 'bytes written by the block manager', 'scale'),
	Stat('block_map_read',
	    'mapped blocks read by the block manager', 'scale'),
	Stat('block_read', 'blocks read by the block manager', 'scale'),
	Stat('block_write', 'blocks written by the block manager', 'scale'),

	##########################################
	# Cache and eviction statistics
	##########################################
	Stat('cache_bytes_dirty', 'cache: tracked dirty bytes in the cache'),
	Stat('cache_bytes_inuse',
	    'cache: bytes currently in the cache', 'no_clear'),
	Stat('cache_bytes_max', 'cache: maximum bytes configured', 'no_clear'),
	Stat('cache_bytes_read', 'cache: bytes read into cache', 'scale'),
	Stat('cache_bytes_write', 'cache: bytes written from cache', 'scale'),
	Stat('cache_eviction_clean',
	    'cache: unmodified pages evicted', 'scale'),
	Stat('cache_eviction_dirty', 'cache: modified pages evicted', 'scale'),
	Stat('cache_eviction_checkpoint',
	    'cache: checkpoint blocked page eviction', 'scale'),
	Stat('cache_eviction_fail',
	    'cache: pages selected for eviction unable to be evicted', 'scale'),
	Stat('cache_eviction_force',
	    'cache: pages queued for forced eviction', 'scale'),
	Stat('cache_eviction_hazard',
	    'cache: hazard pointer blocked page eviction', 'scale'),
	Stat('cache_eviction_internal',
	    'cache: internal pages evicted', 'scale'),
	Stat('cache_eviction_merge',
	    'cache: internal page merge operations completed', 'scale'),
	Stat('cache_eviction_merge_fail',
	    'cache: internal page merge attempts that could not complete',
	    'scale'),
	Stat('cache_eviction_merge_levels',
	    'cache: internal levels merged', 'scale'),
	Stat('cache_eviction_slow',
	    'cache: eviction server unable to reach eviction goal', 'scale'),
	Stat('cache_eviction_walk',
	    'cache: pages walked for eviction', 'scale'),
	Stat('cache_pages_dirty', 'cache: tracked dirty pages in the cache'),
	Stat('cache_pages_inuse',
	    'cache: pages currently held in the cache', 'no_clear'),
	Stat('cache_read', 'cache: pages read into cache', 'scale'),
	Stat('cache_write', 'cache: pages written from cache', 'scale'),

	##########################################
	# Reconciliation statistics
	##########################################
	Stat('rec_pages', 'page reconciliation calls', 'scale'),
	Stat('rec_pages_eviction', 'page reconciliation calls for eviction',
	    'scale'),
	Stat('rec_skipped_update',
	    'reconciliation failed because an update could not be included',
	    'scale'),

	##########################################
	# Transaction statistics
	##########################################
	Stat('txn_ancient', 'ancient transactions', 'scale'),
	Stat('txn_begin', 'transactions', 'scale'),
	Stat('txn_checkpoint', 'transaction checkpoints', 'scale'),
	Stat('txn_commit', 'transactions committed', 'scale'),
	Stat('txn_fail_cache',
	    'transaction failures due to cache overflow', 'scale'),
	Stat('txn_rollback', 'transactions rolled-back', 'scale'),

	##########################################
	# LSM statistics
	##########################################
	Stat('lsm_rows_merged', 'rows merged in an LSM tree', 'scale'),

	##########################################
	# Session operations
	##########################################
	Stat('session_cursor_open', 'open cursor count', 'no_clear'),

	##########################################
	# Total Btree cursor operations
	##########################################
	Stat('cursor_create', 'cursor creation', 'scale'),
	Stat('cursor_insert', 'Btree cursor insert calls', 'scale'),
	Stat('cursor_next', 'Btree cursor next calls', 'scale'),
	Stat('cursor_prev', 'Btree cursor prev calls', 'scale'),
	Stat('cursor_remove', 'Btree cursor remove calls', 'scale'),
	Stat('cursor_reset', 'Btree cursor reset calls', 'scale'),
	Stat('cursor_search', 'Btree cursor search calls', 'scale'),
	Stat('cursor_search_near', 'Btree cursor search near calls', 'scale'),
	Stat('cursor_update', 'Btree cursor update calls', 'scale'),
]

connection_stats = sorted(connection_stats, key=attrgetter('name'))

##########################################
# Data source statistics
##########################################
dsrc_stats = [
	##########################################
	# Session operations
	##########################################
	Stat('session_compact', 'object compaction', 'scale'),
	Stat('session_cursor_open', 'open cursor count', 'no_clear'),

	##########################################
	# Cursor operations
	##########################################
	Stat('cursor_create', 'cursor creation', 'scale'),
	Stat('cursor_insert', 'cursor insert calls', 'scale'),
	Stat('cursor_insert_bulk', 'bulk-loaded cursor-insert calls', 'scale'),
	Stat('cursor_insert_bytes',
	    'cursor-insert key and value bytes inserted', 'scale'),
	Stat('cursor_next', 'cursor next calls', 'scale'),
	Stat('cursor_prev', 'cursor prev calls', 'scale'),
	Stat('cursor_remove', 'cursor remove calls', 'scale'),
	Stat('cursor_remove_bytes', 'cursor-remove key bytes removed', 'scale'),
	Stat('cursor_reset', 'cursor reset calls', 'scale'),
	Stat('cursor_search', 'cursor search calls', 'scale'),
	Stat('cursor_search_near', 'cursor search near calls', 'scale'),
	Stat('cursor_update', 'cursor update calls', 'scale'),
	Stat('cursor_update_bytes',
	    'cursor-update value bytes updated', 'scale'),

	##########################################
	# Btree statistics
	##########################################
	Stat('btree_column_deleted',
	    'column-store variable-size deleted values'),
	Stat('btree_column_fix', 'column-store fixed-size leaf pages'),
	Stat('btree_column_internal', 'column-store internal pages'),
	Stat('btree_column_variable', 'column-store variable-size leaf pages'),
	Stat('btree_compact_rewrite', 'pages rewritten by compaction', 'scale'),
	Stat('btree_entries',
	    'total LSM, table or file object key/value pairs'),
	Stat('btree_fixed_len', 'fixed-record size', 'no_aggregate'),
	Stat('btree_maximum_depth', 'maximum tree depth', 'max_aggregate'),
	Stat('btree_maxintlitem',
	    'maximum internal page item size', 'no_aggregate'),
	Stat('btree_maxintlpage', 'maximum internal page size', 'no_aggregate'),
	Stat('btree_maxleafitem',
	    'maximum leaf page item size', 'no_aggregate'),
	Stat('btree_maxleafpage', 'maximum leaf page size', 'no_aggregate'),
	Stat('btree_overflow', 'overflow pages'),
	Stat('btree_row_internal', 'row-store internal pages'),
	Stat('btree_row_leaf', 'row-store leaf pages'),

	##########################################
	# LSM statistics
	##########################################
	Stat('bloom_count', 'bloom filters in the LSM tree'),
	Stat('bloom_false_positive', 'bloom filter false positives', 'scale'),
	Stat('bloom_hit', 'bloom filter hits', 'scale'),
	Stat('bloom_miss', 'bloom filter misses', 'scale'),
	Stat('bloom_page_evict',
	    'bloom filter pages evicted from cache', 'scale'),
	Stat('bloom_page_read', 'bloom filter pages read into cache', 'scale'),
	Stat('bloom_size', 'total size of bloom filters'),
	Stat('lsm_chunk_count', 'chunks in the LSM tree', 'no_aggregate'),
	Stat('lsm_generation_max',
	    'highest merge generation in the LSM tree', 'max_aggregate'),
	Stat('lsm_lookup_no_bloom',
	    'queries that could have benefited ' +
	    'from a Bloom filter that did not exist', 'scale'),

	##########################################
	# Block manager statistics
	##########################################
	Stat('block_alloc', 'blocks allocated', 'scale'),
	Stat('block_allocsize',
	    'block manager file allocation unit size', 'no_aggregate'),
	Stat('block_checkpoint_size', 'checkpoint size'),
	Stat('block_extension',
	    'block allocations requiring file extension', 'scale'),
	Stat('block_free', 'blocks freed', 'scale'),
	Stat('block_magic', 'file magic number', 'no_aggregate'),
	Stat('block_major', 'file major version number', 'no_aggregate'),
	Stat('block_minor', 'minor version number', 'no_aggregate'),
	Stat('block_size', 'block manager file size in bytes'),

	##########################################
	# Cache and eviction statistics
	##########################################
	Stat('cache_bytes_read', 'bytes read into cache', 'scale'),
	Stat('cache_bytes_write', 'bytes written from cache', 'scale'),
	Stat('cache_eviction_clean', 'unmodified pages evicted', 'scale'),
	Stat('cache_eviction_checkpoint',
	    'cache: checkpoint blocked page eviction', 'scale'),
	Stat('cache_eviction_dirty', 'modified pages evicted', 'scale'),
	Stat('cache_eviction_fail',
	    'data source pages selected for eviction unable to be evicted',
	    'scale'),
	Stat('cache_eviction_force',
	    'cache: pages queued for forced eviction', 'scale'),
	Stat('cache_eviction_hazard',
	    'cache: hazard pointer blocked page eviction', 'scale'),
	Stat('cache_eviction_internal', 'internal pages evicted', 'scale'),
	Stat('cache_eviction_merge',
	    'cache: internal page merge operations completed', 'scale'),
	Stat('cache_eviction_merge_fail',
	    'cache: internal page merge attempts that could not complete',
	    'scale'),
	Stat('cache_eviction_merge_levels',
	    'cache: internal levels merged', 'scale'),
	Stat('cache_overflow_value', 'overflow values cached in memory'),
	Stat('cache_read', 'pages read into cache', 'scale'),
	Stat('cache_read_overflow', 'overflow pages read into cache', 'scale'),
	Stat('cache_write', 'pages written from cache', 'scale'),

	##########################################
	# Compression statistics
	##########################################
	Stat('compress_raw_ok', 'raw compression call succeeded', 'scale'),
	Stat('compress_raw_fail',
	    'raw compression call failed, no additional data available',
	    'scale'),
	Stat('compress_raw_fail_temporary',
	    'raw compression call failed, additional data available', 'scale'),
	Stat('compress_read', 'compressed pages read', 'scale'),
	Stat('compress_write', 'compressed pages written', 'scale'),
	Stat('compress_write_fail', 'page written failed to compress', 'scale'),
	Stat('compress_write_too_small',
	    'page written was too small to compress', 'scale'),

	##########################################
	# Reconciliation statistics
	##########################################
	Stat('rec_dictionary', 'reconciliation dictionary matches', 'scale'),
	Stat('rec_ovfl_key', 'reconciliation overflow keys written', 'scale'),
	Stat('rec_ovfl_value',
	    'reconciliation overflow values written', 'scale'),
	Stat('rec_page_delete', 'reconciliation pages deleted', 'scale'),
	Stat('rec_page_merge', 'reconciliation pages merged', 'scale'),
	Stat('rec_pages', 'page reconciliation calls', 'scale'),
	Stat('rec_pages_eviction',
	    'page reconciliation calls for eviction', 'scale'),
	Stat('rec_skipped_update',
	    'reconciliation failed because an update could not be included',
	    'scale'),
	Stat('rec_split_intl', 'reconciliation internal pages split', 'scale'),
	Stat('rec_split_leaf', 'reconciliation leaf pages split', 'scale'),
	Stat('rec_split_max',
	    'reconciliation maximum number of splits created for a page',
	    'max_aggregate'),

	##########################################
	# Transaction statistics
	##########################################
	Stat('txn_update_conflict', 'update conflicts', 'scale'),
	Stat('txn_write_conflict', 'write generation conflicts', 'scale'),
]

dsrc_stats = sorted(dsrc_stats, key=attrgetter('name'))
