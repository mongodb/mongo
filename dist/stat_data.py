# Auto-generate statistics #defines, with initialization, clear and aggregate
# functions.
#
# NOTE: Statistics reports show individual objects as operations per second.
# All objects where that does not make sense should have the word 'currently'
# or the phrase 'in the cache' in their text description, for example, 'files
# currently open'.
#
# Optional configuration flags:
#	aggrignore	Ignore the value when aggregating statistics
#	aggrmax		Take the maximum value when aggregating statistics
#	aggrset		Set the value when aggregating statistics
#	noclear		Value not cleared by the statistic clear function
#	sps		Scale value per second in the logging tool script

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
	Stat('cond_wait', 'pthread mutex condition wait calls', 'sps'),
	Stat('file_open', 'files currently open', 'noclear'),
	Stat('memory_allocation', 'total heap memory allocations', 'sps'),
	Stat('memory_free', 'total heap memory frees', 'sps'),
	Stat('memory_grow', 'total heap memory re-allocations', 'sps'),
	Stat('read_io', 'total read I/Os', 'sps'),
	Stat('rwlock_read', 'pthread mutex shared lock read-lock calls', 'sps'),
	Stat('rwlock_write',
	    'pthread mutex shared lock write-lock calls', 'sps'),
	Stat('write_io', 'total write I/Os', 'sps'),

	##########################################
	# Block manager statistics
	##########################################
	Stat('block_byte_map_read',
	    'mapped bytes read by the block manager', 'sps'),
	Stat('block_byte_read', 'bytes read by the block manager', 'sps'),
	Stat('block_byte_write', 'bytes written by the block manager', 'sps'),
	Stat('block_map_read',
	    'mapped blocks read by the block manager', 'sps'),
	Stat('block_read', 'blocks read by the block manager', 'sps'),
	Stat('block_write', 'blocks written by the block manager', 'sps'),

	##########################################
	# Cache and eviction statistics
	##########################################
	Stat('cache_bytes_dirty', 'cache: tracked dirty bytes in the cache'),
	Stat('cache_bytes_inuse',
	    'cache: bytes currently in the cache', 'noclear'),
	Stat('cache_bytes_max', 'cache: maximum bytes configured', 'noclear'),
	Stat('cache_bytes_read', 'cache: bytes read into cache', 'sps'),
	Stat('cache_bytes_write', 'cache: bytes written from cache', 'sps'),
	Stat('cache_eviction_clean', 'cache: unmodified pages evicted', 'sps'),
	Stat('cache_eviction_dirty', 'cache: modified pages evicted', 'sps'),
	Stat('cache_eviction_checkpoint',
	    'cache: checkpoint blocked page eviction', 'sps'),
	Stat('cache_eviction_fail',
	    'cache: pages selected for eviction unable to be evicted', 'sps'),
	Stat('cache_eviction_force',
	    'cache: pages queued for forced eviction', 'sps'),
	Stat('cache_eviction_hazard',
	    'cache: hazard pointer blocked page eviction', 'sps'),
	Stat('cache_eviction_internal', 'cache: internal pages evicted', 'sps'),
	Stat('cache_eviction_merge',
	    'cache: internal page merge operations completed', 'sps'),
	Stat('cache_eviction_merge_fail',
	    'cache: internal page merge attempts that could not complete',
	    'sps'),
	Stat('cache_eviction_merge_levels',
	    'cache: internal levels merged', 'sps'),
	Stat('cache_eviction_slow',
	    'cache: eviction server unable to reach eviction goal', 'sps'),
	Stat('cache_eviction_walk', 'cache: pages walked for eviction', 'sps'),
	Stat('cache_pages_dirty', 'cache: tracked dirty pages in the cache'),
	Stat('cache_pages_inuse',
	    'cache: pages currently held in the cache', 'noclear'),
	Stat('cache_read', 'cache: pages read into cache', 'sps'),
	Stat('cache_write', 'cache: pages written from cache', 'sps'),

	##########################################
	# Reconciliation statistics
	##########################################
	Stat('rec_pages', 'page reconciliation calls', 'sps'),
	Stat('rec_pages_eviction', 'page reconciliation calls for eviction',
	    'sps'),
	Stat('rec_skipped_update',
	    'reconciliation failed because an update could not be included',
	    'sps'),

	##########################################
	# Transaction statistics
	##########################################
	Stat('txn_ancient', 'ancient transactions', 'sps'),
	Stat('txn_begin', 'transactions', 'sps'),
	Stat('txn_checkpoint', 'transaction checkpoints', 'sps'),
	Stat('txn_commit', 'transactions committed', 'sps'),
	Stat('txn_fail_cache',
	    'transaction failures due to cache overflow', 'sps'),
	Stat('txn_rollback', 'transactions rolled-back', 'sps'),

	##########################################
	# LSM statistics
	##########################################
	Stat('lsm_rows_merged', 'rows merged in an LSM tree', 'sps'),

	##########################################
	# Session operations
	##########################################
	Stat('session_cursor_open', 'open cursor count', 'noclear'),

	##########################################
	# Total Btree cursor operations
	##########################################
	Stat('cursor_create', 'cursor creation', 'sps'),
	Stat('cursor_insert', 'Btree cursor insert calls', 'sps'),
	Stat('cursor_next', 'Btree cursor next calls', 'sps'),
	Stat('cursor_prev', 'Btree cursor prev calls', 'sps'),
	Stat('cursor_remove', 'Btree cursor remove calls', 'sps'),
	Stat('cursor_reset', 'Btree cursor reset calls', 'sps'),
	Stat('cursor_search', 'Btree cursor search calls', 'sps'),
	Stat('cursor_search_near', 'Btree cursor search near calls', 'sps'),
	Stat('cursor_update', 'Btree cursor update calls', 'sps'),
]

connection_stats = sorted(connection_stats, key=attrgetter('name'))

##########################################
# Data source statistics
##########################################
dsrc_stats = [
	##########################################
	# Session operations
	##########################################
	Stat('session_compact', 'object compaction', 'sps'),
	Stat('session_cursor_open', 'open cursor count', 'noclear'),

	##########################################
	# Cursor operations
	##########################################
	Stat('cursor_create', 'cursor creation', 'sps'),
	Stat('cursor_insert', 'cursor insert calls', 'sps'),
	Stat('cursor_insert_bulk', 'bulk-loaded cursor-insert calls', 'sps'),
	Stat('cursor_insert_bytes',
	    'cursor-insert key and value bytes inserted', 'sps'),
	Stat('cursor_next', 'cursor next calls', 'sps'),
	Stat('cursor_prev', 'cursor prev calls', 'sps'),
	Stat('cursor_remove', 'cursor remove calls', 'sps'),
	Stat('cursor_remove_bytes', 'cursor-remove key bytes removed', 'sps'),
	Stat('cursor_reset', 'cursor reset calls', 'sps'),
	Stat('cursor_search', 'cursor search calls', 'sps'),
	Stat('cursor_search_near', 'cursor search near calls', 'sps'),
	Stat('cursor_update', 'cursor update calls', 'sps'),
	Stat('cursor_update_bytes', 'cursor-update value bytes updated', 'sps'),

	##########################################
	# Btree statistics
	##########################################
	Stat('btree_column_deleted',
	    'column-store variable-size deleted values'),
	Stat('btree_column_fix', 'column-store fixed-size leaf pages'),
	Stat('btree_column_internal', 'column-store internal pages'),
	Stat('btree_column_variable', 'column-store variable-size leaf pages'),
	Stat('btree_compact_rewrite', 'pages rewritten by compaction', 'sps'),
	Stat('btree_entries',
	    'total LSM, table or file object key/value pairs'),
	Stat('btree_fixed_len', 'fixed-record size', 'aggrignore'),
	Stat('btree_maximum_depth', 'maximum tree depth', 'aggrmax'),
	Stat('btree_maxintlitem',
	    'maximum internal page item size', 'aggrignore'),
	Stat('btree_maxintlpage', 'maximum internal page size', 'aggrignore'),
	Stat('btree_maxleafitem', 'maximum leaf page item size', 'aggrignore'),
	Stat('btree_maxleafpage', 'maximum leaf page size', 'aggrignore'),
	Stat('btree_overflow', 'overflow pages'),
	Stat('btree_row_internal', 'row-store internal pages'),
	Stat('btree_row_leaf', 'row-store leaf pages'),

	##########################################
	# LSM statistics
	##########################################
	Stat('bloom_count', 'bloom filters in the LSM tree'),
	Stat('bloom_false_positive', 'bloom filter false positives', 'sps'),
	Stat('bloom_hit', 'bloom filter hits', 'sps'),
	Stat('bloom_miss', 'bloom filter misses', 'sps'),
	Stat('bloom_page_evict',
	    'bloom filter pages evicted from cache', 'sps'),
	Stat('bloom_page_read', 'bloom filter pages read into cache', 'sps'),
	Stat('bloom_size', 'total size of bloom filters'),
	Stat('lsm_chunk_count', 'chunks in the LSM tree', 'aggrignore'),
	Stat('lsm_generation_max',
	    'highest merge generation in the LSM tree', 'aggrmax'),
	Stat('lsm_lookup_no_bloom',
	    'queries that could have benefited ' +
	    'from a Bloom filter that did not exist', 'sps'),

	##########################################
	# Block manager statistics
	##########################################
	Stat('block_alloc', 'blocks allocated', 'sps'),
	Stat('block_allocsize',
	    'block manager file allocation unit size', 'aggrignore'),
	Stat('block_checkpoint_size', 'checkpoint size'),
	Stat('block_extension',
	    'block allocations requiring file extension', 'sps'),
	Stat('block_free', 'blocks freed', 'sps'),
	Stat('block_magic', 'file magic number', 'aggrignore'),
	Stat('block_major', 'file major version number', 'aggrignore'),
	Stat('block_minor', 'minor version number', 'aggrignore'),
	Stat('block_size', 'block manager file size in bytes'),

	##########################################
	# Cache and eviction statistics
	##########################################
	Stat('cache_bytes_read', 'bytes read into cache', 'sps'),
	Stat('cache_bytes_write', 'bytes written from cache', 'sps'),
	Stat('cache_eviction_clean', 'unmodified pages evicted', 'sps'),
	Stat('cache_eviction_checkpoint',
	    'cache: checkpoint blocked page eviction', 'sps'),
	Stat('cache_eviction_dirty', 'modified pages evicted', 'sps'),
	Stat('cache_eviction_fail',
	    'data source pages selected for eviction unable to be evicted',
	    'sps'),
	Stat('cache_eviction_force',
	    'cache: pages queued for forced eviction', 'sps'),
	Stat('cache_eviction_hazard',
	    'cache: hazard pointer blocked page eviction', 'sps'),
	Stat('cache_eviction_internal', 'internal pages evicted', 'sps'),
	Stat('cache_eviction_merge',
	    'cache: internal page merge operations completed', 'sps'),
	Stat('cache_eviction_merge_fail',
	    'cache: internal page merge attempts that could not complete',
	    'sps'),
	Stat('cache_eviction_merge_levels',
	    'cache: internal levels merged', 'sps'),
	Stat('cache_overflow_value', 'overflow values cached in memory'),
	Stat('cache_read', 'pages read into cache', 'sps'),
	Stat('cache_read_overflow', 'overflow pages read into cache', 'sps'),
	Stat('cache_write', 'pages written from cache', 'sps'),

	##########################################
	# Compression statistics
	##########################################
	Stat('compress_raw_ok', 'raw compression call succeeded', 'sps'),
	Stat('compress_raw_fail',
	    'raw compression call failed, no additional data available', 'sps'),
	Stat('compress_raw_fail_temporary',
	    'raw compression call failed, additional data available', 'sps'),
	Stat('compress_read', 'compressed pages read', 'sps'),
	Stat('compress_write', 'compressed pages written', 'sps'),
	Stat('compress_write_fail', 'page written failed to compress', 'sps'),
	Stat('compress_write_too_small',
	    'page written was too small to compress', 'sps'),

	##########################################
	# Reconciliation statistics
	##########################################
	Stat('rec_dictionary', 'reconciliation dictionary matches', 'sps'),
	Stat('rec_ovfl_key', 'reconciliation overflow keys written', 'sps'),
	Stat('rec_ovfl_value', 'reconciliation overflow values written', 'sps'),
	Stat('rec_page_delete', 'reconciliation pages deleted', 'sps'),
	Stat('rec_page_merge', 'reconciliation pages merged', 'sps'),
	Stat('rec_pages', 'page reconciliation calls', 'sps'),
	Stat('rec_pages_eviction',
	    'page reconciliation calls for eviction', 'sps'),
	Stat('rec_skipped_update',
	    'reconciliation failed because an update could not be included',
	    'sps'),
	Stat('rec_split_intl', 'reconciliation internal pages split', 'sps'),
	Stat('rec_split_leaf', 'reconciliation leaf pages split', 'sps'),
	Stat('rec_split_max',
	    'reconciliation maximum number of splits created for a page',
	    'aggrmax'),

	##########################################
	# Transaction statistics
	##########################################
	Stat('txn_update_conflict', 'update conflicts', 'sps'),
	Stat('txn_write_conflict', 'write generation conflicts', 'sps'),
]

dsrc_stats = sorted(dsrc_stats, key=attrgetter('name'))
