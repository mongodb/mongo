# Auto-generate statistics #defines, with allocation, clear and print functions.
#
# The XXX_stats dictionaries are a set of objects consisting of comma-separated
# configuration key words and a text description.  The configuration key words
# are:
#	perm	-- Field is not cleared by the stat clear function.

class Stat:
	def __init__(self, str, config=None):
		self.config = config or []
		self.str = str

##########################################
# CONNECTION handle statistics
##########################################
conn_stats = {
	'file_open' : Stat('file open'),
	'memalloc' : Stat('memory allocations'),
	'memfree' : Stat('memory frees'),
	'mtx_lock' : Stat('mutex lock calls'),
	'total_read_io' : Stat('total read I/Os'),
	'total_write_io' : Stat('total write I/Os'),
	'workq_passes' : Stat('workQ queue passes'),
	'workq_yield' : Stat('workQ yields'),
}

##########################################
# Cache handle statistics
##########################################
cache_stats = {
	'cache_bytes_inuse' : Stat('cache: bytes currently held in the cache', ['perm']),
	'cache_bytes_max' : Stat('cache: maximum bytes configured', ['perm']),
	'cache_evict_hazard' : Stat('cache: pages selected for eviction not evicted because of a hazard reference'),
	'cache_evict_modified' : Stat('cache: modified pages selected for eviction'),
	'cache_evict_unmodified' : Stat('cache: unmodified pages selected for eviction'),
	'cache_overflow_read' : Stat('cache: overflow pages read from the file'),
	'cache_page_read' : Stat('cache: pages read from a file'),
	'cache_page_write' : Stat('cache: pages written to a file'),
	'cache_pages_inuse' : Stat('cache: pages currently held in the cache', ['perm']),
}

##########################################
# BTREE handle statistics
##########################################
btree_stats = {
	'alloc' : Stat('file: block allocations'),
	'extend' : Stat('file: block allocations require file extension'),
	'free' : Stat('file: block frees'),
	'huffman_key' : Stat('file: huffman key compression in bytes'),
	'huffman_value' : Stat('file: huffman value compression in bytes'),
	'items_inserted' : Stat('file: key/value pairs inserted'),
	'overflow_data' : Stat('file: overflow values inserted'),
	'overflow_key' : Stat('file: overflow key items inserted'),
	'overflow_read' : Stat('file: overflow pages read from the file'),
	'page_read' : Stat('file: pages read from a file'),
	'page_write' : Stat('file: pages written to a file'),
	'split_intl' : Stat('split internal pages'),
	'split_leaf' : Stat('split leaf pages'),
}

##########################################
# BTREE file statistics
##########################################
btree_file_stats = {
	'file_allocsize' : Stat('page size allocation unit'),
	'file_base_recno' : Stat('base record number'),
	'file_col_fix' : Stat('column-store fixed-size leaf pages'),
	'file_col_internal' : Stat('column-store internal pages'),
	'file_col_rle' : Stat('column-store repeat-count compressed fixed-size leaf pages'),
	'file_col_variable' : Stat('column-store variable-size leaf pages'),
	'file_fixed_len' : Stat('fixed-record size'),
	'file_freelist_entries' : Stat('number of entries in the freelist'),
	'file_intlmax' : Stat('maximum internal page size'),
	'file_intlmin' : Stat('minimum internal page size'),
	'file_item_col_deleted' : Stat('column-store deleted data items'),
	'file_item_total_data' : Stat('total data items'),
	'file_item_total_key' : Stat('total keys'),
	'file_leafmax' : Stat('maximum leaf page size'),
	'file_leafmin' : Stat('minimum leaf page size'),
	'file_magic' : Stat('magic number'),
	'file_major' : Stat('major version number'),
	'file_minor' : Stat('minor version number'),
	'file_overflow' : Stat('overflow pages'),
	'file_row_internal' : Stat('row-store internal pages'),
	'file_row_leaf' : Stat('row-store leaf pages'),
}

##########################################
# FH handle statistics
##########################################
fh_stats = {
	'fsync' : Stat('fsyncs'),
	'read_io' : Stat('read I/Os'),
	'write_io' : Stat('write I/Os'),
}
