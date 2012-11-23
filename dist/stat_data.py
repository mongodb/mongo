# Auto-generate statistics #defines, with allocation, clear and print functions.
#
# The XXX_stats dictionaries are a set of objects consisting of comma-separated
# configuration key words and a text description.  The configuration key words
# are:
#	perm	-- Field is not cleared by the stat clear function.

from operator import attrgetter

class Stat:
	def __init__(self, name, desc, **flags):
		self.name = name
		self.desc = desc
		self.flags = flags

	def __cmp__(self, other):
		return cmp(self.name, other.name)

##########################################
# CONNECTION statistics
##########################################
connection_stats = [
	Stat('block_read', 'blocks read from a file'),
	Stat('block_write', 'blocks written to a file'),
	Stat('byte_read', 'bytes read from a file'),
	Stat('byte_write', 'bytes written to a file'),
	Stat('cache_bytes_inuse',
		'cache: bytes currently held in the cache', perm=1),
	Stat('cache_bytes_max', 'cache: maximum bytes configured', perm=1),
	Stat('cache_evict_hazard',
	    'cache: pages selected for eviction not ' +
	    'evicted because of a hazard reference'),
	Stat('cache_evict_internal', 'cache: internal pages evicted'),
	Stat('cache_evict_modified', 'cache: modified pages evicted'),
	Stat('cache_evict_slow',
		'cache: eviction server unable to reach eviction goal'),
	Stat('cache_evict_unmodified', 'cache: unmodified pages evicted'),
	Stat('cache_pages_inuse',
		'cache: pages currently held in the cache', perm=1),
	Stat('checkpoint', 'checkpoints'),
	Stat('cond_wait', 'condition wait calls'),
	Stat('file_open', 'files currently open'),
	Stat('memalloc', 'total memory allocations'),
	Stat('memfree', 'total memory frees'),
	Stat('rwlock_rdlock', 'rwlock readlock calls'),
	Stat('rwlock_wrlock', 'rwlock writelock calls'),
	Stat('total_read_io', 'total read I/Os'),
	Stat('total_write_io', 'total write I/Os'),
	Stat('txn_ancient', 'ancient transactions'),
	Stat('txn_begin', 'transactions'),
	Stat('txn_commit', 'transactions committed'),
	Stat('txn_fail_cache', 'transaction failures due to cache overflow'),
	Stat('txn_rollback', 'transactions rolled-back'),
]

connection_stats = sorted(connection_stats, key=attrgetter('name'))

##########################################
# Data source statistics
##########################################
dsrc_stats = [
	Stat('block_alloc', 'block allocations'),
	Stat('block_extend', 'block allocations required file extension'),
	Stat('block_free', 'block frees'),
	Stat('byte_read', 'bytes read into cache'),
	Stat('byte_write', 'bytes written from cache'),
	Stat('ckpt_size', 'checkpoint size'),
	Stat('cursor_insert', 'cursor-inserts'),
	Stat('cursor_read', 'cursor-read'),
	Stat('cursor_read_near', 'cursor-read-near'),
	Stat('cursor_read_next', 'cursor-read-next'),
	Stat('cursor_read_prev', 'cursor-read-prev'),
	Stat('cursor_remove', 'cursor-removes'),
	Stat('cursor_reset', 'cursor-resets'),
	Stat('cursor_update', 'cursor-updates'),
	Stat('entries', 'total entries'),
	Stat('file_allocsize', 'page size allocation unit'),
	Stat('file_bulk_loaded', 'bulk-loaded entries'),
	Stat('file_compact_rewrite', 'pages rewritten by compaction'),
	Stat('file_fixed_len', 'fixed-record size'),
	Stat('file_magic', 'magic number'),
	Stat('file_major', 'major version number'),
	Stat('file_maxintlitem', 'maximum internal page item size'),
	Stat('file_maxintlpage', 'maximum internal page size'),
	Stat('file_maxleafitem', 'maximum leaf page item size'),
	Stat('file_maxleafpage', 'maximum leaf page size'),
	Stat('file_minor', 'minor version number'),
	Stat('file_size', 'file size'),
	Stat('overflow_page', 'overflow pages'),
	Stat('overflow_read', 'overflow pages read into cache'),
	Stat('overflow_value_cache', 'overflow values cached in memory'),
	Stat('page_col_deleted', 'column-store deleted values'),
	Stat('page_col_fix', 'column-store fixed-size leaf pages'),
	Stat('page_col_int', 'column-store internal pages'),
	Stat('page_col_var', 'column-store variable-size leaf pages'),
	Stat('page_evict', 'pages evicted from the data source'),
	Stat('page_evict_fail',
	    'pages that were selected for eviction that could not be evicted'),
	Stat('page_read', 'pages read into cache'),
	Stat('page_row_int', 'row-store internal pages'),
	Stat('page_row_leaf', 'row-store leaf pages'),
	Stat('page_write', 'pages written from cache'),
	Stat('rec_dictionary', 'reconcile: dictionary match'),
	Stat('rec_hazard', 'reconciliation unable to acquire hazard reference'),
	Stat('rec_ovfl_key', 'reconciliation overflow key'),
	Stat('rec_ovfl_value', 'reconciliation overflow value'),
	Stat('rec_page_delete', 'pages deleted'),
	Stat('rec_page_merge', 'deleted or temporary pages merged'),
	Stat('rec_split_intl', 'internal pages split'),
	Stat('rec_split_leaf', 'leaf pages split'),
	Stat('rec_written', 'pages written from reconciliation'),
	Stat('txn_update_conflict', 'update conflicts'),
	Stat('txn_write_conflict', 'write generation conflicts'),

##########################################
# LSM statistics
##########################################
	Stat('bloom_count', 'number of Bloom filters in the LSM tree'),
	Stat('bloom_false_positive', 'number of Bloom filter false positives'),
	Stat('bloom_hit', 'number of Bloom filter hits'),
	Stat('bloom_miss', 'number of Bloom filter misses'),
	Stat('bloom_page_evict', 'number of Bloom pages evicted from cache'),
	Stat('bloom_page_read', 'number of Bloom pages read into cache'),
	Stat('bloom_size', 'total size of Bloom filters'),
	Stat('lsm_chunk_count', 'number of chunks in the LSM tree'),
	Stat('lsm_generation_max', 'highest merge generation in the LSM tree'),
	Stat('lsm_lookup_no_bloom',
	    'number of queries that could have benefited ' +
	    'from a Bloom filter that did not exist'),

]

dsrc_stats = sorted(dsrc_stats, key=attrgetter('name'))
