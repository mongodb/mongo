# This file is a python script that describes the WiredTiger API.

class Error:
	def __init__(self, name, desc, **flags):
		self.name = name
		self.desc = desc
		self.flags = flags

errors = [
	Error('WT_DEADLOCK', 'conflict with concurrent operation'),
	Error('WT_DUPLICATE_KEY', 'attempt to insert an existing key'),
	Error('WT_ERROR', 'non-specific WiredTiger error'),
	Error('WT_NOTFOUND', 'item not found'),
	Error('WT_RESTART', 'restart the operation (internal)', undoc=True),
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

	def __cmp__(self, other):
		return cmp(self.name, other.name)

# All schema objects can have column names (optional for simple tables).
column_meta = [
	Config('columns', '', r'''
		list of the column names.  Comma-separated list of the form
		<code>(column[,...])</code>.  For tables, the number of entries
		must match the total number of values in \c key_format and \c
		value_format.  For colgroups and indices, all column names must
		appear in the list of columns for the table''',
		type='list'),
]

filename_meta = [
	Config('filename', '', r'''
		override the default filename derived from the object name'''),
]

format_meta = column_meta + [
	Config('key_format', 'u', r'''
		the format of the data packed into key items.  See @ref packing
		for details.  By default, applications use the WT_ITEM struct
		to manipulate raw byte arrays.  Key items of type \c 'r' are
		record numbers and records will be stored using a column
		store''',
		type='format'),
	Config('value_format', 'u', r'''
		the format of the data packed into value items.  See @ref
		packing for details.  By default, applications use the WT_ITEM
		struct to manipulate raw byte arrays.  Value items of type
		\c 't' are bitfields, and in column stores will be stored using
		a fixed-length store''',
		type='format'),
]

# Per-file configuration
file_meta = format_meta + [
	Config('allocation_size', '512B', r'''
		file unit allocation size, in bytes''',
		min='512B', max='128MB'),
	Config('huffman_key', '', r'''
		use Huffman encoding for Btree keys.  Permitted values are
		empty (off), \c "english" or \c "<filename>".  See @ref huffman
		for more details'''),
	Config('huffman_value', '', r'''
		use Huffman encoding for Btree values.  Permitted values are
		empty (off), \c "english" or \c "<filename>".  See @ref huffman
		for more details'''),
	Config('internal_key_truncate', 'true', r'''
		configure the Btree for truncation of internal keys, discarding
		unnecessary trailing bytes on internal keys''',
		type='boolean'),
	Config('internal_node_max', '2KB', r'''
		maximum page size for internal nodes, in bytes''',
		min='512B', max='512MB'),
	Config('internal_node_min', '2KB', r'''
		minimum page size for internal nodes, in bytes''',
		min='512B', max='512MB'),
	Config('key_gap', '10', r'''
		configure the maximum gap between instantiated keys in a Btree
		leaf page, constraining the number of keys processed to
		instantiate a random Btree leaf page key''',
		min='0'),
	Config('leaf_node_max', '1MB', r'''
		maximum page size for leaf nodes, in bytes''',
		min='512B', max='512MB'),
	Config('leaf_node_min', '32KB', r'''
		minimum page size for leaf nodes, in bytes''',
		min='512B', max='512MB'),
	Config('block_compressor', '', r'''
		use indicated compressor for database blocks.  Permitted
		values are empty (off) or \c "<name>".  See @ref compressors
		for more details.'''),
	Config('prefix_compression', 'true', r'''
		configure the Btree for prefix compression, storing keys as a
		count of bytes matching the previous key plus a unique
		suffix''',
		type='boolean'),
	Config('split_min', 'false', r'''
		configure minimal splits in Btree reconciliation code
		(debugging only)''',
		type='boolean'),
	Config('split_pct', '75', r'''
		configure the Btree page split size as a percentage of the
		maximum Btree page size, that is, when a Btree page is split,
		it will be split into smaller pages, where each page is the
		specified percentage of the maximum Btree page size''',
		min='0', max='100'),
	Config('type', 'btree', r'''
		the file type''',
		choices=['btree']),
]

table_only_meta = [
	Config('colgroups', '', r'''
		comma-separated list of names of column groups.  Each column
		group is stored separately, keyed by the primary key of the
		table.  If no column groups are specified, all columns are
		stored together in a single file.  All value columns in the
		table must appear in at least one column group.  Each column
		group must be created with a separate call to
		WT_SESSION::create'''),
]

colgroup_meta = column_meta + filename_meta

index_meta = column_meta + filename_meta

table_meta = format_meta + table_only_meta

methods = {
'file.meta' : Method(file_meta),

'colgroup.meta' : Method(colgroup_meta),

'index.meta' : Method(index_meta),

'table.meta' : Method(table_meta),

'cursor.close' : Method([
	Config('clear', 'false', r'''
		for statistics cursors, reset statistics counters.''',
		type='boolean'),
]),

'session.close' : Method([]),

'session.create' : Method(table_meta + file_meta + filename_meta + [
	Config('exclusive', 'false', r'''
		fail if the object exists.  When false (the default), if the
		object exists, check that its settings match the specified
		configuration''',
		type='boolean'),
]),

'session.drop' : Method([
	Config('force', 'false', r'''
		return success if the object does not exist''',
		type='boolean'),
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
		ignore the encodings for the key and value, manage data as if
		the formats were \c "u"''',
		type='boolean'),
	Config('statistics', 'false', r'''
		configure the cursor for statistics''',
		type='boolean'),
]),

'session.rename' : Method([]),
'session.salvage' : Method([
	Config('force', 'false', r'''
		force salvage even of files that do not appear to be WiredTiger
		files''',
		type='boolean'),
]),
'session.sync' : Method([]),
'session.truncate' : Method([]),
'session.verify' : Method([]),
'session.dumpfile' : Method([]),

'session.begin_transaction' : Method([
	Config('isolation', 'read-committed', r'''
		the isolation level for this transaction''',
		choices=['serializable', 'snapshot', 'read-committed',
		    'read-uncommitted']),
	Config('name', '', r'''
		name of the transaction for tracing and debugging'''),
	Config('sync', 'full', r'''
		how to sync log records when the transaction commits''',
		choices=["full", "flush", "write", "none"]),
	Config('priority', 0, r'''
		priority of the transaction for resolving conflicts.
		Transactions with higher values are less likely to abort''',
		min='-100', max='100'),
]),

'session.commit_transaction' : Method([]),
'session.rollback_transaction' : Method([]),

'session.checkpoint' : Method([
	Config('archive', 'false', r'''
		remove log files no longer required for transactional
		durability''',
		type='boolean'),
	Config('flush_cache', 'true', r'''
		flush the cache''',
		type='boolean'),
	Config('flush_log', 'true', r'''
		flush the log to disk''',
		type='boolean'),
	Config('log_size', '0', r'''
		only proceed if more than the specified number of bytes of log
		records have been written since the last checkpoint''',
		min='0'),
	Config('force', 'false', r'''
		write a new checkpoint even if nothing has changed since the
		last one''',
		type='boolean'),
	Config('timeout', '0', r'''
		only proceed if more than the specified number of milliseconds
		have elapsed since the last checkpoint''',
		min='0'),
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
		a prefix for all names registered by this extension (e.g., to
		make namespaces distinct or during upgrades'''),
]),

'connection.open_session' : Method([]),

'wiredtiger_open' : Method([
	Config('cache_size', '20MB', r'''
		maximum heap memory to allocate for the cache''',
		min='1MB', max='10TB'),
	Config('create', 'false', r'''
		create the database if it does not exist''',
		type='boolean'),
	Config('exclusive', 'false', r'''
		fail if the database already exists''',
		type='boolean'),
	Config('extensions', '', r'''
		list of extensions to load.  Optional values override the name
		of the default entry function \c wiredtiger_init''',
		type='list'),
	Config('error_prefix', '', r'''
		Prefix string for error messages'''),
	Config('hazard_max', '30', r'''
		number of simultaneous hazard references per session handle''',
		min='15'),
	Config('logging', 'false', r'''
		enable logging''',
		type='boolean'),
	Config('multiprocess', 'false', r'''
		permit sharing between processes (will automatically start an
		RPC server for primary processes and use RPC for secondary
		processes)''',
		type='boolean'),
	Config('multithread', 'false', r'''
		permit sharing between threads of control (will automatically
		serialize as necessary to allow multiple threads of control
		simultaneous access to the database)''',
		type='boolean'),
	Config('session_max', '50', r'''
		maximum expected number of sessions (including server
		threads)''',
		min='1'),
	Config('verbose', '', r'''
		enable messages for various events.  Options are given as a
		list, such as <code>"verbose=[evictserver,read]"</code>''',
		type='list',
		    choices=['allocate', 'evictserver', 'fileops', 'hazard',
		    'mutex', 'read', 'readserver', 'reconcile', 'salvage',
		    'write']),
]),
}

flags = {
###################################################
# Internal routine flag declarations
###################################################
	'block_read' : [ 'ERR_QUIET', 'VERIFY' ],
	'page_free' : [ 'PAGE_FREE_IGNORE_DISK' ],
	'page_reconcile' : [ 'REC_EVICT', 'REC_LOCKED', 'REC_SALVAGE' ],
	'tree_walk' : [ 'WALK_CACHE' ],
	'verbose' : [
		'VERB_ALLOCATE',
		'VERB_EVICTSERVER',
		'VERB_FILEOPS',
		'VERB_HAZARD',
		'VERB_MUTEX',
		'VERB_READ',
		'VERB_READSERVER',
		'VERB_RECONCILE',
		'VERB_SALVAGE',
		'VERB_WRITE'
	],

###################################################
# Structure flag declarations
###################################################
	'conn' : [ 'MULTITHREAD', 'SERVER_RUN', 'WORKQ_RUN' ],
	'buf' : [ 'BUF_INUSE' ],
	'session' : [ 'SESSION_INTERNAL' ],
}
