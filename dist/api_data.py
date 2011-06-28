# This file is a python script that describes the WiredTiger API.

errors = [
	('WT_DEADLOCK', 'conflict with concurrent operation'),
	('WT_ERROR', 'non-specific WiredTiger error'),
	('WT_NOTFOUND', 'item not found'),
	('WT_READONLY', 'attempt to modify a read-only value'),
	('WT_RESTART', 'restart the operation (internal)'),
	('WT_TOOSMALL', 'buffer too small'),
]

class Method:
	def __init__(self, config, **flags):
		self.config = config
		self.flags = flags

class Config:
	def __init__(self, name, default, desc, **flags):
		self.name = name
		self.default = default
		self.desc = desc
		self.flags = flags

methods = {
'cursor.close' : Method([]),

'session.close' : Method([]),

'session.create' : Method([
	Config('allocation_size', '512B', r'''
		file unit allocation size, in bytes''', min='512B', max='128MB'),
	Config('btree_huffman_key', '', r'''
		use Huffman encoding for Btree keys.  Permitted values are empty (off),
		\c "english" or \c "<filename>".  See @ref huffman for more details.'''),
	Config('btree_huffman_value', '', r'''
		use Huffman encoding for Btree values.  Permitted values are empty (off),
		\c "english" or \c "<filename>".  See @ref huffman for more details.'''),
	Config('btree_key_gap', '10', r'''
		configure the maximum gap between instantiated keys in a Btree leaf page,
		constraining the number of keys processed to instantiate a random Btree
		leaf page key''', min='0'),
	Config('btree_internal_key_truncate', 'true', r'''
		configure the Btree for truncation of internal keys, discarding unnecessary
		trailing bytes on internal keys''', type='boolean'),
	Config('btree_prefix_compression', 'true', r'''
		configure the Btree for prefix compression, storing keys as a count of
		bytes matching the previous key plus a unique suffix''', type='boolean'),
	Config('btree_split_min', 'false', r'''
		configure minimal splits in Btree reconciliation code (debugging only)''',
		type='boolean'),
	Config('btree_split_pct', '75', r'''
		configure the Btree page split size as a percentage of the maximum Btree
		page size, that is, when a Btree page is split, it will be split into
		smaller pages, where each page is the specified percentage of the maximum
		Btree page size''', min='0', max='100'),
	Config('colgroup.name', '', r'''
		named group of columns to store together.  Comma-separated list of
		the form <code>(column[,...])</code>.  Each column group is stored
		separately, keyed by the primary key of the table.  Any column that
		does not appear in a column group is stored in a default, unnamed,
		column group for the table'''),
	Config('columns', '', r'''
		list of the column names.  Comma-separated list of the form
		<code>(column[,...])</code>.  The number of entries must match the
		total number of values in \c key_format and \c value_format''',
		type='list'),
	Config('exclusive', 'false', r'''
		fail if the table exists (if "no", the default, verifies that the
		table exists and has the specified schema''', type='boolean'),
	Config('index.name', '', r'''
		named index on a list of columns.  Comma-separated list of the form
		<code>(column[,...])</code>'''),
	Config('intl_node_max', '2KB', r'''
		maximum page size for internal nodes, in bytes''',
		min='512B', max='512MB'),
	Config('intl_node_min', '2KB', r'''
		minimum page size for internal nodes, in bytes''',
		min='512B', max='512MB'),
	Config('key_format', 'u', r'''
		the format of the data packed into key items.  See @ref packing for
		details.  If not set, a default value of \c "u" is assumed, and
		applications use the WT_ITEM struct to manipulate raw byte arrays''',
		type='format'),
	Config('leaf_node_max', '1MB', r'''
		maximum page size for leaf nodes, in bytes''',
		min='512B', max='512MB'),
	Config('leaf_node_min', '32KB', r'''
		minimum page size for leaf nodes, in bytes''',
		min='512B', max='512MB'),
	Config('runlength_encoding', 'false', r'''
		compress repeated adjacent values''', type='boolean'),
	Config('value_format', 'u', r'''
		the format of the data packed into value items.  See @ref packing
		for details.  If not set, a default value of \c "u" is assumed, and
		applications use the WT_ITEM struct to manipulate raw byte arrays''',
		type='format'),
]),

'session.drop' : Method([
	Config('force', 'false', r'''
		return success if the object does not exist''', type='boolean'),
	]),

'session.log_printf' : Method([]),

'session.open_cursor' : Method([
	Config('bulk', 'false', r'''
		configure the cursor for bulk loads''',
		type='boolean'),
	Config('dump', 'false', r'''
		configure the cursor for dump format inputs and outputs''',
		type='boolean'),
	Config('isolation', 'read-committed', r'''
		the isolation level for this cursor.
		Ignored for transactional cursors''',
		choices=['snapshot', 'read-committed', 'read-uncommitted']),
	Config('overwrite', 'false', r'''
		if an existing key is inserted, overwrite the existing value''',
		type='boolean'),
	Config('printable', 'false', r'''
		for dump cursors, pass through printable bytes unmodified''',
		type='boolean'),
	Config('raw', 'false', r'''
		ignore the encodings for the key and value, manage data as if the
		formats were \c "u"''',
		type='boolean'),
]),

'session.rename' : Method([]),
'session.salvage' : Method([]),
'session.sync' : Method([]),
'session.truncate' : Method([]),
'session.verify' : Method([]),
'session.dumpfile' : Method([]),

'session.begin_transaction' : Method([
	Config('isolation', 'read-committed', r'''
		the isolation level for this transaction; default "serializable"''',
		choices=['serializable', 'snapshot', 'read-committed',
			'read-uncommitted']),
	Config('name', '', r'''
		name of the transaction for tracing and debugging'''),
	Config('sync', 'full', r'''
		how to sync log records when the transaction commits"''',
		choices=["full", "flush", "write", "none"]),
	Config('priority', 0, r'''
		priority of the transaction for resolving conflicts, an integer
		between -100 and 100.  Transactions with higher values are less likely
		to abort''', min='-100', max='100'),
]),

'session.commit_transaction' : Method([]),
'session.rollback_transaction' : Method([]),

'session.checkpoint' : Method([
	Config('archive', 'false', r'''
		remove log files no longer required for transactional durability''',
		type='boolean'),
	Config('force', 'false', r'''
		write a new checkpoint even if nothing has changed since the last
		one''', type='boolean'),
	Config('flush_cache', 'true', r'''
		flush the cache''', type='boolean'),
	Config('flush_log', 'true', r'''
		flush the log to disk''', type='boolean'),
	Config('log_size', '0', r'''
		only proceed if more than the specified number of bytes of log
		records have been written since the last checkpoint''', min='0'),
	Config('timeout', '0', r'''
		only proceed if more than the specified number of milliseconds have
		elapsed since the last checkpoint''', min='0'),
]),

'connection.add_cursor_type' : Method([]),
'connection.add_collator' : Method([]),
'connection.add_compressor' : Method([]),
'connection.add_extractor' : Method([]),
'connection.close' : Method([]),

'connection.load_extension' : Method([
	Config('entry', 'wiredtiger_extension_init', r'''
		the entry point of the extension'''),
	Config('prefix', '', r'''
		a prefix for all names registered by this extension (e.g., to make
		namespaces distinct or during upgrades'''),
]),

'connection.open_session' : Method([]),

'wiredtiger_open' : Method([
	Config('cache_size', '20MB', r'''
		maximum heap memory to allocate for the cache''',
		min='1MB', max='10TB'),
	Config('create', 'false', r'''
		create the database if it does not exist''', type='boolean'),
	Config('exclusive', 'false', r'''
		fail if the database already exists''', type='boolean'),
	Config('extensions', '[]', r'''
		list of extensions to load.  Optional values override the name of the
		default entry function \c "wiredtiger_init"''', type='list'),
	Config('error_prefix', '', r'''
		Prefix string for error messages'''),
	Config('hazard_max', '15', r'''
		number of hazard references per session''', min='3'),
	Config('logging', 'false', r'''
		enable logging''', type='boolean'),
	Config('session_max', '50', r'''
		maximum expected number of sessions (including server threads)''',
		min='1'),
	Config('multiprocess', 'false', r'''
		permit sharing between processes (will automatically start an RPC
		server for primary processes and use RPC for secondary processes)''',
		type='boolean'),
	Config('verbose', '', r'''
		enable messages for various events.  Options are given as
		a list, such as \c "verbose=[evict,read]"''',
		type='list', choices=['evict', 'fileops', 'hazard', 'mutex', 'read', 'salvage']),
]),
}

flags = {
###################################################
# Internal routine flag declarations
###################################################
	'bt_dump' : [ 'DUMP_PRINT' ],
	'bt_search_col' : [ 'WRITE' ],
	'bt_search_key_row' : [ 'WRITE' ],
	'bt_tree_walk' : [ 'WALK_CACHE' ],
	'page_free' : [ 'PAGE_FREE_IGNORE_DISK' ],
	'page_reconcile' : [ 'REC_EVICT', 'REC_LOCKED', 'REC_SALVAGE' ],
	'verbose' : [
		'VERB_EVICT',
		'VERB_FILEOPS',
		'VERB_HAZARD',
		'VERB_MUTEX',
		'VERB_READ',
		'VERB_SALVAGE'
	],

###################################################
# Structure flag declarations
###################################################
	'conn' : [ 'SERVER_RUN', 'WORKQ_RUN' ],
	'buf' : [ 'BUF_INUSE' ],
	'session' : [ 'SESSION_INTERNAL' ],
}
