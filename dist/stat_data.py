# Auto-generate statistics #defines, with allocation, clear and print functions.
#
# The XXX_stats dictionaries are a set of objects consisting of comma-separated
# configuration key words and a text description.  The configuration key words
# are:
#	perm	-- Field is not cleared by the stat clear function.

class Stat:
	def __init__(self, name, desc, config=None):
		self.name = name
		self.desc = desc
		self.config = config or []

	def __cmp__(self, other):
		return cmp(self.name, other.name)

##########################################
# CONNECTION statistics
##########################################
connection_stats = [
	Stat('block_read', 'blocks read from a file'),
	Stat('block_write', 'blocks written to a file'),
	Stat('cache_bytes_inuse', 'cache: bytes currently held in the cache', 'perm'),
	Stat('cache_bytes_max', 'cache: maximum bytes configured', 'perm'),
	Stat('cache_evict_hazard', 'cache: pages selected for eviction not evicted because of a hazard reference'),
	Stat('cache_evict_internal', 'cache: internal pages evicted'),
	Stat('cache_evict_modified', 'cache: modified pages evicted'),
	Stat('cache_evict_slow', 'cache: eviction server unable to reach eviction goal'),
	Stat('cache_evict_unmodified', 'cache: unmodified pages evicted'),
	Stat('cache_pages_inuse', 'cache: pages currently held in the cache', 'perm'),
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

##########################################
# BTREE statistics
##########################################
btree_stats = [
	Stat('alloc', 'file: block allocations'),
	Stat('cursor_inserts', 'cursor-inserts'),
	Stat('cursor_read', 'cursor-read'),
	Stat('cursor_read_near', 'cursor-read-near'),
	Stat('cursor_read_next', 'cursor-read-next'),
	Stat('cursor_read_prev', 'cursor-read-prev'),
	Stat('cursor_removes', 'cursor-removes'),
	Stat('cursor_resets', 'cursor-resets'),
	Stat('cursor_updates', 'cursor-updates'),
	Stat('extend', 'file: block allocations required file extension'),
	Stat('file_allocsize', 'page size allocation unit'),
	Stat('file_bulk_loaded', 'bulk-loaded entries'),
	Stat('file_col_deleted', 'column-store deleted values'),
	Stat('file_col_fix_pages', 'column-store fixed-size leaf pages'),
	Stat('file_col_int_pages', 'column-store internal pages'),
	Stat('file_col_var_pages', 'column-store variable-size leaf pages'),
	Stat('file_compact_rewrite', 'pages rewritten by compaction'),
	Stat('file_entries', 'total entries'),
	Stat('file_fixed_len', 'fixed-record size'),
	Stat('file_magic', 'magic number'),
	Stat('file_major', 'major version number'),
	Stat('file_maxintlitem', 'maximum internal page item size'),
	Stat('file_maxintlpage', 'maximum internal page size'),
	Stat('file_maxleafitem', 'maximum leaf page item size'),
	Stat('file_maxleafpage', 'maximum leaf page size'),
	Stat('file_minor', 'minor version number'),
	Stat('file_overflow', 'overflow pages'),
	Stat('file_row_int_pages', 'row-store internal pages'),
	Stat('file_row_leaf_pages', 'row-store leaf pages'),
	Stat('file_size', 'file: size'),
	Stat('file_write_conflicts', 'write generation conflicts'),
	Stat('free', 'file: block frees'),
	Stat('overflow_read', 'file: overflow pages read from the file'),
	Stat('overflow_value_cache', 'file: overflow values cached in memory'),
	Stat('page_evict', 'file: pages evicted from the file'),
	Stat('page_evict_fail', 'file: pages that were selected for eviction that could not be evicted'),
	Stat('page_read', 'file: pages read from the file'),
	Stat('page_write', 'file: pages written to the file'),
	Stat('rec_dictionary', 'reconcile: dictionary match'),
	Stat('rec_hazard', 'reconcile: unable to acquire hazard reference'),
	Stat('rec_ovfl_key', 'reconcile: overflow key'),
	Stat('rec_ovfl_value', 'reconcile: overflow value'),
	Stat('rec_page_delete', 'reconcile: pages deleted'),
	Stat('rec_page_merge', 'reconcile: deleted or temporary pages merged'),
	Stat('rec_split_intl', 'reconcile: internal pages split'),
	Stat('rec_split_leaf', 'reconcile: leaf pages split'),
	Stat('rec_written', 'reconcile: pages written'),
	Stat('update_conflict', 'update conflicts'),
]

##########################################
# LSM statistics
##########################################
lsm_stats = [
	Stat('chunk_cache_evict', 'Number of pages evicted from LSM chunks'),
	Stat('chunk_cache_read', 'Number of pages read into LSM chunks'),
	Stat('bloom_false_positives', 'Number of bloom filter false positives'),
	Stat('bloom_hits', 'Number of bloom filter hits'),
	Stat('bloom_misses', 'Number of bloom filter misses'),
	Stat('search_miss_no_bloom', 'Number of queries that could have benefited from a bloom filter that did not exist'),
	Stat('bloom_space', 'Total space used by bloom filters'),
	Stat('bloom_cache_evict', 'Number of bloom pages evicted from cache'),
	Stat('bloom_cache_read', 'Number of bloom pages read into cache'),
	Stat('chunk_count', 'Number of chunks in the LSM tree'),
	Stat('bloom_count', 'Number of bloom filters in the LSM tree'),
	Stat('cache_evict', 'Number of pages evicted from cache'),
	Stat('cache_evict_fail', 'Number of pages selected for eviction that could not be evicted'),
	Stat('cache_read', 'Number of pages read into cache'),
	Stat('cache_write', 'Number of pages written from cache'),
	Stat('generation_max', 'Highest merge generation in the LSM tree'),
]

