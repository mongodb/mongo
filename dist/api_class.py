# Auto-generate everything we can:
#	flag values
#	getter/setter code
#	manual page headers
#	structure method fields
#
# The api file is a python script that loads two dictionaries: "methods" and
# "flags".
#
# The "methods" dictionary is a set of API class objects, keyed by the
# method name.   The fields are:
#	1: the method name
#		 'handle' + '.' + 'method'
#	2: a string of comma-separated configuration key words
#		colonly	 -- column databases only
#		extfunc  -- call an external function to do the work
#		getter	 -- getter method
#		connlock -- locks the connection mutex (implied by getter/setter)
#		method	 -- method returns an int
#		methodV  -- method returns void
#		noauto	 -- don't auto-generate a stub at all
#		rdonly	 -- not allowed if the database is read-only
#		restart	 -- handle WT_RESTART in the API call
#		rowonly	 -- row databases only
#		session	 -- function takes a SESSION/BTREE argument pair
#		setter	 -- setter method
#		verify	 -- setter methods call validation function
#     3: a slash-separated list of argument names, declaration and optional
#         default value triplets, that is, first the name of the argument,
#         the declaration for the argument, and, in the case of a setter, an
#         optional default value.  In the declaration, "@S" is replaced by
#         the argument name when needed (sometimes we need the name in a
#         declaration, and sometimes we don't).
#	4: a list of flags, if any.
#		If there's a flags variable, but the method doesn't currently
#		take any flags, enter '__NONE__'.
#	5: a list of on-transitions, if any
#	6: a list of off-transitions, if any

flags = {}
methods = {}

class Api:
	def __init__(self, key, config, args, f, on, off):
		self.key = key
		self.handle = key.split('.')[0]
		self.method = key.split('.')[1]
		self.config = config
		self.args = args
		if f:
			flags[key] = f
		self.on = on
		self.off = off

###################################################
# SESSION method declarations
###################################################
methods['session.close'] = Api(
	'session.close',
	'method, connlock',
	['flags/uint32_t @S'],
	['__NONE__'],
	['init'], [])

###################################################
# CONNECTION method declarations
###################################################
methods['connection.cache_size_get'] = Api(
	'connection.cache_size_get',
	'method, getter',
	['cache_size/uint32_t *@S'],
	[],
	['init'], [])
methods['connection.cache_size_set'] = Api(
	'connection.cache_size_set',
	'method, setter, verify',
	['cache_size/uint32_t @S/20'],
	[],
	['init'], ['open'])

methods['connection.close'] = Api(
	'connection.close',
	'method',
	['flags/uint32_t @S'],
	['__NONE__'],
	['init'], [])

methods['connection.data_update_min_get'] = Api(
	'connection.data_update_min_get',
	'method, getter',
	['data_update_min/uint32_t *@S'],
	[],
	['init'], [])
methods['connection.data_update_min_set'] = Api(
	'connection.data_update_min_set',
	'method, setter',
	['data_update_min/uint32_t @S/8 * 1024'],
	[],
	['init'], [])

methods['connection.data_update_max_get'] = Api(
	'connection.data_update_max_get',
	'method, getter',
	['data_update_max/uint32_t *@S'],
	[],
	['init'], [])
methods['connection.data_update_max_set'] = Api(
	'connection.data_update_max_set',
	'method, setter',
	['data_update_max/uint32_t @S/32 * 1024'],
	[],
	['init'], [])

methods['connection.btree'] = Api(
	'connection.btree',
	'method',
	['flags/uint32_t @S',
	 'btreep/BTREE **@S'],
	['__NONE__'],
	['open'], [])

methods['connection.hazard_size_get'] = Api(
	'connection.hazard_size_get',
	'method, getter',
	['hazard_size/uint32_t *@S/15'],
	[],
	['init'], [])
methods['connection.hazard_size_set'] = Api(
	'connection.hazard_size_set',
	'method, setter, verify',
	['hazard_size/uint32_t @S'],
	[],
	['init'], ['open'])

methods['connection.msgcall_get'] = Api(
	'connection.msgcall_get',
	'method, getter',
	['msgcall/void (**@S)(const CONNECTION *, const char *)'],
	[],
	['init'], [])
methods['connection.msgcall_set'] = Api(
	'connection.msgcall_set',
	'method, setter',
	['msgcall/void (*@S)(const CONNECTION *, const char *)'],
	[],
	['init'], [])

methods['connection.msgfile_get'] = Api(
	'connection.msgfile_get',
	'method, getter',
	['msgfile/FILE **@S'],
	[],
	['init'], [])
methods['connection.msgfile_set'] = Api(
	'connection.msgfile_set',
	'method, setter',
	['msgfile/FILE *@S'],
	[],
	['init'], [])

methods['connection.open'] = Api(
	'connection.open',
	'method',
	['home/const char *@S',
	 'mode/mode_t @S',
	 'flags/uint32_t @S'],
	['__NONE__'],
	['init'], ['open'])

methods['connection.stat_clear'] = Api(
	'connection.stat_clear',
	'method',
	['flags/uint32_t @S'],
	['__NONE__'],
	['init'], [])

methods['connection.stat_print'] = Api(
	'connection.stat_print',
	'method',
	['stream/FILE *@S',
	 'flags/uint32_t @S'],
	['__NONE__'],
	['init'], [])

methods['connection.sync'] = Api(
	'connection.sync',
	'method',
	['progress/void (*@S)(const char *, uint64_t)',
	 'flags/uint32_t @S'],
	['__NONE__'],
	['open'], [])

methods['connection.session'] = Api(
	'connection.session',
	'method, connlock',
	['flags/uint32_t @S',
	 'sessionp/SESSION **@S'],
	['__NONE__'],
	['open'], [])

methods['connection.session_size_get'] = Api(
	'connection.session_size_get',
	'method, getter',
	['session_size/uint32_t *@S'],
	[],
	['init'], [])
methods['connection.session_size_set'] = Api(
	'connection.session_size_set',
	'method, setter, verify',
	['session_size/uint32_t @S/50'],
	[],
	['init'], ['open'])

methods['connection.verbose_get'] = Api(
	'connection.verbose_get',
	'method, getter',
	['verbose/uint32_t *@S'],
	[],
	['init'], [])
methods['connection.verbose_set'] = Api(
	'connection.verbose_set',
	'method, setter, verify',
	['verbose/uint32_t @S'],
	['VERB_ALL',
	 'VERB_EVICT',
	 'VERB_FILEOPS',
	 'VERB_HAZARD',
	 'VERB_MUTEX',
	 'VERB_READ'],
	['init'], [])

###################################################
# Db standalone method declarations
###################################################
methods['btree.btree_compare_get'] = Api(
	'btree.btree_compare_get',
	'method, getter',
	['btree_compare/int (**@S)(BTREE *, const WT_ITEM *, const WT_ITEM *)'],
	[],
	['init'], [])
methods['btree.btree_compare_set'] = Api(
	'btree.btree_compare_set',
	'method, setter',
	['btree_compare/int (*@S)(BTREE *, const WT_ITEM *, const WT_ITEM *)/__wt_bt_lex_compare'],
	[],
	['init'], ['open'])

methods['btree.btree_compare_int_get'] = Api(
	'btree.btree_compare_int_get',
	'method, getter',
	['btree_compare_int/int *@S'],
	[],
	['init'], [])
methods['btree.btree_compare_int_set'] = Api(
	'btree.btree_compare_int_set',
	'method, setter, verify',
	['btree_compare_int/int @S'],
	[],
	['init'], ['open'])

methods['btree.btree_itemsize_get'] = Api(
	'btree.btree_itemsize_get',
	'method, getter',
	['intlitemsize/uint32_t *@S',
	 'leafitemsize/uint32_t *@S'],
	[],
	['init'], [])
methods['btree.btree_itemsize_set'] = Api(
	'btree.btree_itemsize_set',
	'method, setter',
	['intlitemsize/uint32_t @S',
	 'leafitemsize/uint32_t @S'],
	[],
	['init'], ['open'])

methods['btree.btree_pagesize_get'] = Api(
	'btree.btree_pagesize_get',
	'method, getter',
	['allocsize/uint32_t *@S',
	 'intlmin/uint32_t *@S',
	 'intlmax/uint32_t *@S',
	 'leafmin/uint32_t *@S',
	 'leafmax/uint32_t *@S'],
	[],
	['init'], [])
methods['btree.btree_pagesize_set'] = Api(
	'btree.btree_pagesize_set',
	'method, setter',
	['allocsize/uint32_t @S',
	 'intlmin/uint32_t @S',
	 'intlmax/uint32_t @S',
	 'leafmin/uint32_t @S',
	 'leafmax/uint32_t @S'],
	[],
	['init'], ['open'])

methods['btree.bulk_load'] = Api(
	'btree.bulk_load',
	'method, rdonly, session',
	['progress/void (*@S)(const char *, uint64_t)',
	 'cb/int (*@S)(BTREE *, WT_ITEM **, WT_ITEM **)'],
	[],
	['open'], [])

methods['btree.close'] = Api(
	'btree.close',
	'method, session',
	['flags/uint32_t @S'],
	['OSWRITE'],
	['init'], [])

methods['btree.col_del'] = Api(
	'btree.col_del',
	'method, colonly, rdonly, restart, session',
	['session/SESSION *@S',
	 'recno/uint64_t @S',
	 'flags/uint32_t @S'],
	['__NONE__'],
	['open'], [])

methods['btree.col_put'] = Api(
	'btree.col_put',
	'method, colonly, rdonly, restart, session',
	['session/SESSION *@S',
	 'recno/uint64_t @S',
	 'value/WT_ITEM *@S',
	 'flags/uint32_t @S'],
	['__NONE__'],
	['open'], [])

methods['btree.column_set'] = Api(
	'btree.column_set',
	'method, setter, verify',
	['fixed_len/uint32_t @S',
	 'dictionary/const char *@S',
	 'flags/uint32_t @S'],
	['RLE'],
	['init'], ['open'])

methods['btree.dump'] = Api(
	'btree.dump',
	'method, session',
	['stream/FILE *@S',
	 'progress/void (*@S)(const char *, uint64_t)',
	 'flags/uint32_t @S'],
	['DEBUG',
	 'PRINTABLES' ],
	['open'], [])

methods['btree.huffman_set'] = Api(
	'btree.huffman_set',
	'method, extfunc, setter',
	['huffman_table/uint8_t const *@S',
	 'huffman_table_size/u_int @S',
	 'huffman_flags/uint32_t @S'],
	[ 'ASCII_ENGLISH', 'HUFFMAN_DATA', 'HUFFMAN_KEY', 'TELEPHONE' ],
	['init'], ['open'])

methods['btree.open'] = Api(
	'btree.open',
	'method, session',
	['name/const char *@S',
	 'mode/mode_t @S',
	 'flags/uint32_t @S'],
	[ 'CREATE',
	  'RDONLY' ],
	['init'], [])

methods['btree.row_del'] = Api(
	'btree.row_del',
	'method, rdonly, restart, rowonly, session',
	['session/SESSION *@S',
	 'key/WT_ITEM *@S',
	 'flags/uint32_t @S'],
	['__NONE__'],
	['open'], [])

methods['btree.row_put'] = Api(
	'btree.row_put',
	'method, rdonly, restart, rowonly, session',
	['session/SESSION *@S',
	 'key/WT_ITEM *@S',
	 'value/WT_ITEM *@S',
	 'flags/uint32_t @S'],
	['__NONE__'],
	['open'], [])

methods['btree.stat_clear'] = Api(
	'btree.stat_clear',
	'method',
	['flags/uint32_t @S'],
	['__NONE__'],
	['open'], [])

methods['btree.stat_print'] = Api(
	'btree.stat_print',
	'method, session',
	['stream/FILE *@S',
	 'flags/uint32_t @S'],
	['__NONE__'],
	['open'], [])

methods['btree.salvage'] = Api(
	'btree.salvage',
	'method, session',
	['progress/void (*@S)(const char *, uint64_t)',
	 'flags/uint32_t @S'],
	['__NONE__'],
	['open'], [])

methods['btree.sync'] = Api(
	'btree.sync',
	'method, rdonly, session',
	['progress/void (*@S)(const char *, uint64_t)',
	 'flags/uint32_t @S'],
	['OSWRITE'],
	['open'], [])

methods['btree.verify'] = Api(
	'btree.verify',
	'method, session',
	['progress/void (*@S)(const char *, uint64_t)',
	 'flags/uint32_t @S'],
	['__NONE__'],
	['open'], [])

###################################################
# External routine flag declarations
###################################################
flags['wiredtiger_conn_init'] = [
	'MEMORY_CHECK' ]

###################################################
# Internal routine flag declarations
###################################################
flags['bt_search_col'] = [
	'WRITE' ]
flags['bt_search_key_row'] = [
	'WRITE' ]
flags['bt_tree_walk'] = [
	'WALK_CACHE' ]

###################################################
# Structure flag declarations
###################################################
flags['conn'] = [
	'MEMORY_CHECK',
	'SERVER_RUN',
	'WORKQ_RUN' ]
flags['btree'] = [
	'COLUMN',
	'RDONLY',
	'RLE' ]
flags['buf'] = [
	'BUF_INUSE' ]
