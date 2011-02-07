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
# IENV handle statistics
##########################################
ienv_stats = {
	'FILE_OPEN' : Stat('file open'),
	'MEMALLOC' : Stat('memory allocations'),
	'MEMFREE' : Stat('memory frees'),
	'MTX_LOCK' : Stat('mutex lock calls'),
	'TOTAL_READ_IO' : Stat('total read I/Os'),
	'TOTAL_WRITE_IO' : Stat('total write I/Os'),
	'WORKQ_PASSES' : Stat('workQ queue passes'),
	'WORKQ_YIELD' : Stat('workQ yields'),
}

##########################################
# Cache handle statistics
##########################################
cache_stats = {
	'CACHE_BYTES_INUSE' : Stat('cache: bytes currently held in the cache', ['perm']),
	'CACHE_BYTES_MAX' : Stat('cache: maximum bytes configured', ['perm']),
	'CACHE_EVICT_HAZARD' : Stat('cache: pages selected for eviction not evicted because of a hazard reference'),
	'CACHE_EVICT_MODIFIED' : Stat('cache: modified pages selected for eviction'),
	'CACHE_EVICT_UNMODIFIED' : Stat('cache: unmodified pages selected for eviction'),
	'CACHE_PAGES_INUSE' : Stat('cache: pages currently held in the cache', ['perm']),
	'CACHE_OVERFLOW_READ' : Stat('cache: overflow pages read from the file'),
	'CACHE_PAGE_READ' : Stat('cache: pages read from a file'),
	'CACHE_PAGE_WRITE' : Stat('cache: pages written to a file'),
}

##########################################
# BTREE handle statistics
##########################################
btree_stats = {
    'FILE_ALLOC' : Stat('file: block allocations'),
    'FILE_EXTEND' : Stat('file: block allocations require file extension'),
    'FILE_FREE' : Stat('file: block frees'),
    'FILE_HUFFMAN_DATA' : Stat('file: huffman data compression in bytes'),
    'FILE_HUFFMAN_KEY' : Stat('file: huffman key compression in bytes'),
    'FILE_ITEMS_INSERTED' : Stat('file: key/data pairs inserted'),
    'FILE_OVERFLOW_DATA' : Stat('file: overflow data items inserted'),
    'FILE_OVERFLOW_KEY' : Stat('file: overflow key items inserted'),
    'FILE_OVERFLOW_READ' : Stat('file: overflow pages read from the file'),
    'FILE_PAGE_READ' : Stat('file: pages read from a file'),
    'FILE_PAGE_WRITE' : Stat('file: pages written to a file'),
}

##########################################
# BTREE file statistics
##########################################
btree_fstats = {
    'BASE_RECNO' : Stat('base record number'),
    'FIXED_LEN' : Stat('fixed-record size'),
    'FREELIST_ENTRIES' : Stat('number of entries in the freelist'),
    'INTLMAX' : Stat('maximum internal page size'),
    'INTLMIN' : Stat('minimum internal page size'),
    'ITEM_COL_DELETED' : Stat('column-store deleted data items'),
    'ITEM_DATA_OVFL' : Stat('total overflow data items'),
    'ITEM_KEY_OVFL' : Stat('total overflow keys'),
    'ITEM_TOTAL_DATA' : Stat('total data items'),
    'ITEM_TOTAL_KEY' : Stat('total keys'),
    'LEAFMAX' : Stat('maximum leaf page size'),
    'LEAFMIN' : Stat('minimum leaf page size'),
    'MAGIC' : Stat('magic number'),
    'MAJOR' : Stat('major version number'),
    'MINOR' : Stat('minor version number'),
    'PAGE_COL_FIX' : Stat('column-store fixed-size leaf pages'),
    'PAGE_COL_INTERNAL' : Stat('column-store internal pages'),
    'PAGE_COL_RLE' : Stat('column-store repeat-count compressed fixed-size leaf pages'),
    'PAGE_COL_VARIABLE' : Stat('column-store variable-size leaf pages'),
    'PAGE_OVERFLOW' : Stat('overflow pages'),
    'PAGE_ROW_INTERNAL' : Stat('row-store internal pages'),
    'PAGE_ROW_LEAF' : Stat('row-store leaf pages'),
    'PAGE_SPLIT_INTL' : Stat('split internal pages'),
    'PAGE_SPLIT_LEAF' : Stat('split leaf pages'),
}

##########################################
# FH handle statistics
##########################################
fh_stats = {
	'FSYNC' : Stat('fsyncs'),
	'READ_IO' : Stat('read I/Os'),
	'WRITE_IO' : Stat('write I/Os'),
}
