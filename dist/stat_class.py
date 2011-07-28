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

##########################################
# CONNECTION statistics
##########################################
conn_stats = [
	Stat('cache_bytes_inuse', 'cache: bytes currently held in the cache', 'perm'),
	Stat('cache_bytes_max', 'cache: maximum bytes configured', 'perm'),
	Stat('cache_evict_hazard', 'cache: pages selected for eviction not evicted because of a hazard reference'),
	Stat('cache_evict_modified', 'cache: modified pages selected for eviction'),
	Stat('cache_evict_unmodified', 'cache: unmodified pages selected for eviction'),
	Stat('cache_overflow_read', 'cache: overflow pages read from the file'),
	Stat('cache_page_read', 'cache: pages read from a file'),
	Stat('cache_page_write', 'cache: pages written to a file'),
	Stat('cache_pages_inuse', 'cache: pages currently held in the cache', 'perm'),
	Stat('file_open', 'file open'),
	Stat('memalloc', 'memory allocations'),
	Stat('memfree', 'memory frees'),
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
	Stat('extend', 'file: block allocations require file extension'),
	Stat('file_allocsize', 'page size allocation unit'),
	Stat('file_col_fix', 'column-store fixed-size leaf pages'),
	Stat('file_col_internal', 'column-store internal pages'),
	Stat('file_col_variable', 'column-store variable-size leaf pages'),
	Stat('file_fixed_len', 'fixed-record size'),
	Stat('file_freelist_entries', 'number of entries in the freelist'),
	Stat('file_intlmax', 'maximum internal page size'),
	Stat('file_intlmin', 'minimum internal page size'),
	Stat('file_item_col_deleted', 'column-store deleted data items'),
	Stat('file_item_total_data', 'total data items'),
	Stat('file_item_total_key', 'total keys'),
	Stat('file_leafmax', 'maximum leaf page size'),
	Stat('file_leafmin', 'minimum leaf page size'),
	Stat('file_magic', 'magic number'),
	Stat('file_major', 'major version number'),
	Stat('file_minor', 'minor version number'),
	Stat('file_overflow', 'overflow pages'),
	Stat('file_row_internal', 'row-store internal pages'),
	Stat('file_row_leaf', 'row-store leaf pages'),
	Stat('free', 'file: block frees'),
	Stat('items_inserted', 'file: key/value pairs inserted'),
	Stat('overflow_data', 'file: overflow values inserted'),
	Stat('overflow_key', 'file: overflow key items inserted'),
	Stat('overflow_read', 'file: overflow pages read from the file'),
	Stat('page_delete', 'file: pages deleted'),
	Stat('page_read', 'file: pages read from a file'),
	Stat('page_write', 'file: pages written to a file'),
	Stat('split_intl', 'split internal pages'),
	Stat('split_leaf', 'split leaf pages'),
]
