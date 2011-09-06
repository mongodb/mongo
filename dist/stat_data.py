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
	Stat('cache_evict_modified', 'cache: modified pages selected for eviction'),
	Stat('cache_evict_slow', 'cache: eviction server unable to reach eviction goal'),
	Stat('cache_evict_unmodified', 'cache: unmodified pages selected for eviction'),
	Stat('cache_pages_inuse', 'cache: pages currently held in the cache', 'perm'),
	Stat('file_open', 'files currently open'),
	Stat('memalloc', 'total memory allocations'),
	Stat('memfree', 'total memory frees'),
	Stat('mtx_lock', 'mutex lock calls'),
	Stat('total_read_io', 'total read I/Os'),
	Stat('total_write_io', 'total write I/Os'),
	Stat('workq_passes', 'workQ queue passes'),
	Stat('workq_yield', 'workQ yields'),
]

##########################################
# BTREE statistics
##########################################
btree_stats = [
	Stat('alloc', 'file: block allocations'),
	Stat('cursor_first', 'cursor-first'),
	Stat('cursor_inserts', 'cursor-inserts'),
	Stat('cursor_last', 'cursor-last'),
	Stat('cursor_read', 'cursor-read'),
	Stat('cursor_read_near', 'cursor-read-near'),
	Stat('cursor_read_next', 'cursor-read-next'),
	Stat('cursor_read_prev', 'cursor-read-prev'),
	Stat('cursor_removes', 'cursor-removes'),
	Stat('cursor_updates', 'cursor-updates'),
	Stat('extend', 'file: block allocations required file extension'),
	Stat('file_allocsize', 'page size allocation unit'),
	Stat('file_bulk_loaded', 'bulk-loaded entries'),
	Stat('file_col_deleted', 'column-store deleted values'),
	Stat('file_col_fix_pages', 'column-store fixed-size leaf pages'),
	Stat('file_col_int_pages', 'column-store internal pages'),
	Stat('file_col_var_pages', 'column-store variable-size leaf pages'),
	Stat('file_entries', 'total entries'),
	Stat('file_fixed_len', 'fixed-record size'),
	Stat('file_freelist_bytes', 'number of bytes in the freelist'),
	Stat('file_freelist_entries', 'number of entries in the freelist'),
	Stat('file_intlmax', 'maximum internal page size'),
	Stat('file_intlmin', 'minimum internal page size'),
	Stat('file_leafmax', 'maximum leaf page size'),
	Stat('file_leafmin', 'minimum leaf page size'),
	Stat('file_magic', 'magic number'),
	Stat('file_major', 'major version number'),
	Stat('file_minor', 'minor version number'),
	Stat('file_overflow', 'overflow pages'),
	Stat('file_row_int_pages', 'row-store internal pages'),
	Stat('file_row_leaf_pages', 'row-store leaf pages'),
	Stat('file_size', 'file: size'),
	Stat('free', 'file: block frees'),
	Stat('overflow_read', 'file: overflow pages read from the file'),
	Stat('page_read', 'file: pages read from the file'),
	Stat('page_write', 'file: pages written to the file'),
	Stat('rec_hazard', 'reconcile: unable to acquire hazard reference'),
	Stat('rec_ovfl_key', 'reconcile: overflow key'),
	Stat('rec_ovfl_value', 'reconcile: overflow value'),
	Stat('rec_page_delete', 'reconcile: pages deleted'),
	Stat('rec_page_merge', 'reconcile: deleted or temporary pages merged'),
	Stat('rec_split_intl', 'reconcile: internal pages split'),
	Stat('rec_split_leaf', 'reconcile: leaf pages split'),
]
