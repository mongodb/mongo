# Auto-generate statistics #defines, with allocation, clear and print functions.
#
# The XXX_stats dictionaries are a set of objects consisting of comma-separated
# configuration key words and a text description.  The configuration key words
# are:
#	perm	-- Field is not cleared by the stat clear function.

class Stat:
	def __init__(self, config, str):
		self.config = config
		self.str = str

##########################################
# IENV handle statistics
##########################################
ienv_stats = {}
ienv_stats['FILE_OPEN'] = Stat([], 'file open')
ienv_stats['MEMALLOC'] = Stat([], 'memory allocations')
ienv_stats['MEMFREE'] = Stat([], 'memory frees')
ienv_stats['MTX_LOCK'] = Stat([], 'mutex lock calls')
ienv_stats['TOTAL_READ_IO'] = Stat([], 'total read I/Os')
ienv_stats['TOTAL_WRITE_IO'] = Stat([], 'total write I/Os')
ienv_stats['WORKQ_PASSES'] = Stat([], 'workQ queue passes')
ienv_stats['WORKQ_YIELD'] = Stat([], 'workQ yields')

##########################################
# Cache handle statistics
##########################################
cache_stats = {}
cache_stats['CACHE_BYTES_INUSE'] = Stat(['perm'], 'cache: bytes currently held in the cache')
cache_stats['CACHE_BYTES_MAX'] = Stat(['perm'], 'cache: maximum bytes configured')
cache_stats['CACHE_EVICT_HAZARD'] = Stat([], 'cache: pages selected for eviction not evicted because of a hazard reference')
cache_stats['CACHE_EVICT_MODIFIED'] = Stat([], 'cache: modified pages selected for eviction')
cache_stats['CACHE_EVICT_UNMODIFIED'] = Stat([], 'cache: unmodified pages selected for eviction')
cache_stats['CACHE_PAGES_INUSE'] = Stat(['perm'], 'cache: pages currently held in the cache')
cache_stats['CACHE_OVERFLOW_READ'] = Stat([], 'cache: overflow pages read from the file')
cache_stats['CACHE_PAGE_READ'] = Stat([], 'cache: pages read from a file')
cache_stats['CACHE_PAGE_WRITE'] = Stat([], 'cache: pages written to a file')

##########################################
# IDB handle statistics
##########################################
idb_stats = {}
idb_stats['FILE_ALLOC'] = Stat([], 'file: block allocations')
idb_stats['FILE_DUPLICATE_ITEMS_INSERTED'] = Stat([], 'file: duplicate key/data pairs inserted')
idb_stats['FILE_EXTEND'] = Stat([], 'file: block allocations require file extension')
idb_stats['FILE_FREE'] = Stat([], 'file: block frees')
idb_stats['FILE_HUFFMAN_DATA'] = Stat([], 'file: huffman data compression in bytes')
idb_stats['FILE_HUFFMAN_KEY'] = Stat([], 'file: huffman key compression in bytes')
idb_stats['FILE_ITEMS_INSERTED'] = Stat([], 'file: key/data pairs inserted')
idb_stats['FILE_OVERFLOW_DATA'] = Stat([], 'file: overflow data items inserted')
idb_stats['FILE_OVERFLOW_KEY'] = Stat([], 'file: overflow key items inserted')
idb_stats['FILE_OVERFLOW_READ'] = Stat([], 'file: overflow pages read from the file')
idb_stats['FILE_PAGE_READ'] = Stat([], 'file: pages read from a file')
idb_stats['FILE_PAGE_WRITE'] = Stat([], 'file: pages written to a file')

##########################################
# IDB file statistics
##########################################
idb_dstats = {}
idb_dstats['BASE_RECNO'] = Stat([], 'base record number')
idb_dstats['DUP_TREE'] = Stat([], 'duplicate data off-page trees')
idb_dstats['FIXED_LEN'] = Stat([], 'fixed-record size')
idb_dstats['FREELIST_ENTRIES'] = Stat([], 'number of entries in the freelist')
idb_dstats['INTLMAX'] = Stat([], 'maximum internal page size')
idb_dstats['INTLMIN'] = Stat([], 'minimum internal page size')
idb_dstats['ITEM_COL_DELETED'] = Stat([], 'column-store deleted data items')
idb_dstats['ITEM_DATA_OVFL'] = Stat([], 'total overflow data items')
idb_dstats['ITEM_DUP_DATA'] = Stat([], 'total duplicate data items')
idb_dstats['ITEM_KEY_OVFL'] = Stat([], 'total overflow keys')
idb_dstats['ITEM_TOTAL_DATA'] = Stat([], 'total data items')
idb_dstats['ITEM_TOTAL_KEY'] = Stat([], 'total keys')
idb_dstats['LEAFMAX'] = Stat([], 'maximum leaf page size')
idb_dstats['LEAFMIN'] = Stat([], 'minimum leaf page size')
idb_dstats['MAGIC'] = Stat([], 'magic number')
idb_dstats['MAJOR'] = Stat([], 'major version number')
idb_dstats['MINOR'] = Stat([], 'minor version number')
idb_dstats['PAGE_COL_FIX'] = Stat([], 'column-store fixed-size leaf pages')
idb_dstats['PAGE_COL_INTERNAL'] = Stat([], 'column-store internal pages')
idb_dstats['PAGE_COL_RLE'] = Stat([], 'column-store repeat-count compressed fixed-size leaf pages')
idb_dstats['PAGE_COL_VARIABLE'] = Stat([], 'column-store variable-size leaf pages')
idb_dstats['PAGE_DUP_INTERNAL'] = Stat([], 'duplicate internal pages')
idb_dstats['PAGE_DUP_LEAF'] = Stat([], 'duplicate leaf pages')
idb_dstats['PAGE_OVERFLOW'] = Stat([], 'overflow pages')
idb_dstats['PAGE_ROW_INTERNAL'] = Stat([], 'row-store internal pages')
idb_dstats['PAGE_ROW_LEAF'] = Stat([], 'row-store leaf pages')
idb_dstats['TREE_LEVEL'] = Stat([], 'number of levels in the btree')

##########################################
# FH handle statistics
##########################################
fh_stats = {}
fh_stats['FSYNC'] = Stat([], 'fsyncs')
fh_stats['READ_IO'] = Stat([], 'read I/Os')
fh_stats['WRITE_IO'] = Stat([], 'write I/Os')
