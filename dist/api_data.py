# This file is a python script that describes the WiredTiger API.

class Error:
	def __init__(self, name, desc, long_desc=None, **flags):
		self.name = name
		self.desc = desc
		self.long_desc = long_desc
		self.flags = flags

errors = [
	Error('WT_DEADLOCK', 'conflict between concurrent operations', '''
		This error is generated when an operation cannot be completed
		due to a conflict with concurrent operations.  The operation
		should be retried.  If a transaction is in progress, it should
		be rolled back and the operation retried in a new
		transaction.'''),
	Error('WT_DUPLICATE_KEY', 'attempt to insert an existing key', '''
		This error is generated when the application attempts to insert
		a record with the same key as an existing record without the
		'overwrite' configuration to WT_SESSION::open_cursor.'''),
	Error('WT_ERROR', 'non-specific WiredTiger error', '''
		This error is generated for cases that are not covered by
		specific error returns.'''),
	Error('WT_NOTFOUND', 'cursor item not found', '''
		This error indicates a cursor operation did not find a
		record to return.  This includes search and other
		operations where no record matched the cursor's search
		key such as WT_CURSOR::update or WT_CURSOR::remove.'''),
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
		the format of the data packed into key items.  See @ref
		schema_format_types for details.  By default, the key_format is
		\c 'u' and applications use WT_ITEM structures to manipulate
		raw byte arrays. By default, records are stored in row-store
		files: keys of type \c 'r' are record numbers and records
		referenced by record number are stored in column-store files''',
		type='format'),
	Config('value_format', 'u', r'''
		the format of the data packed into value items.  See @ref
		schema_format_types for details.  By default, the value_format
		is \c 'u' and applications use a WT_ITEM structure to
		manipulate raw byte arrays. Value items of type 't' are
		bitfields, and when configured with record number type keys,
		will be stored using a fixed-length store''',
		type='format'),
]

# Per-file configuration
file_config = format_meta + [
	Config('allocation_size', '512B', r'''
		the file unit allocation size, in bytes, must a power-of-two;
		smaller values decrease the file space required by overflow
		items, and the default value of 512B is a good choice absent
		requirements from the operating system or storage device''',
		min='512B', max='128MB'),
	Config('block_compressor', '', r'''
		configure a compressor for file blocks.  Permitted values are
		empty (off) or \c "bzip2", \c "snappy" or custom compression
		engine \c "name" created with WT_CONNECTION::add_compressor.
		See @ref compression for more information'''),
	Config('checksum', 'true', r'''
		configure file block checksums; if false, the block
		manager is free to not write or check block checksums.
		This can increase performance in applications where
		compression provides checksum functionality or read-only
		applications where blocks require no verification''',
		type='boolean'),
	Config('collator', '', r'''
		configure custom collation for keys.  Value must be a collator
		name created with WT_CONNECTION::add_collator'''),
	Config('huffman_key', '', r'''
		configure Huffman encoding for keys.  Permitted values
		are empty (off), \c "english", \c "utf8<file>" or \c
		"utf16<file>".  See @ref huffman for more information'''),
	Config('huffman_value', '', r'''
		configure Huffman encoding for values.  Permitted values
		are empty (off), \c "english", \c "utf8<file>" or \c
		"utf16<file>".  See @ref huffman for more information'''),
	Config('internal_key_truncate', 'true', r'''
		configure internal key truncation, discarding unnecessary
		trailing bytes on internal keys (ignored for custom
		collators)''',
		type='boolean'),
	Config('internal_page_max', '2KB', r'''
		the maximum page size for internal nodes, in bytes; the size
		must be a multiple of the allocation size and is significant
		for applications wanting to avoid excessive L2 cache misses
		while searching the tree''',
		min='512B', max='512MB'),
	Config('internal_item_max', '0', r'''
		the maximum key size stored on internal nodes, in bytes.  If
		zero, a maximum is calculated to permit at least 8 keys per
		internal page''',
		min=0),
	Config('key_gap', '10', r'''
		the maximum gap between instantiated keys in a Btree leaf page,
		constraining the number of keys processed to instantiate a
		random Btree leaf page key''',
		min='0'),
	Config('leaf_page_max', '1MB', r'''
		the maximum page size for leaf nodes, in bytes; the size must
		be a multiple of the allocation size, and is significant for
		applications wanting to maximize sequential data transfer from
		a storage device''',
		min='512B', max='512MB'),
	Config('leaf_item_max', '0', r'''
		the maximum key or value size stored on leaf nodes, in bytes.
		If zero, a size is calculated to permit at least 8 items
		(values or row store keys) per leaf page''',
		min=0),
	Config('prefix_compression', 'true', r'''
		configure row-store format key prefix compression''',
		type='boolean'),
	Config('split_pct', '75', r'''
		the Btree page split size as a percentage of the maximum Btree
		page size, that is, when a Btree page is split, it will be
		split into smaller pages, where each page is the specified
		percentage of the maximum Btree page size''',
		min='25', max='100'),
	Config('type', 'btree', r'''
		the file type''',
		choices=['btree']),
]

# File metadata, including both configurable and non-configurable (internal)
file_meta = file_config + [
	Config('checkpoint', '', r'''
		the file checkpoint entries'''),
	Config('version', '(major=0,minor=0)', r'''
		the file version'''),
]

table_only_meta = [
	Config('colgroups', '', r'''
		comma-separated list of names of column groups.  Each column
		group is stored separately, keyed by the primary key of the
		table.  If no column groups are specified, all columns are
		stored together in a single file.  All value columns in the
		table must appear in at least one column group.  Each column
		group must be created with a separate call to
		WT_SESSION::create''', type='list'),
]

colgroup_meta = column_meta + filename_meta

index_meta = column_meta + filename_meta

table_meta = format_meta + table_only_meta

# Cursor runtime config, shared by cursor.reconfigure and session.open_cursor
cursor_runtime_config = [
	Config('append', 'false', r'''
		only supported by cursors with record number keys: append the
		value as a new record, creating a new record number key''',
		type='boolean'),
	Config('overwrite', 'false', r'''
		change the behavior of the cursor's insert method to
		overwrite previously existing values''',
		type='boolean'),
]

# Connection runtime config, shared by conn.reconfigure and wiredtiger_open
connection_runtime_config = [
	Config('cache_size', '100MB', r'''
		maximum heap memory to allocate for the cache''',
		min='1MB', max='10TB'),
	Config('error_prefix', '', r'''
		prefix string for error messages'''),
	Config('eviction_target', '80', r'''
		continue evicting until the cache becomes less full than this
		(as a percentage).  Must be less than \c eviction_trigger''',
		min=10, max=99),
	Config('eviction_trigger', '95', r'''
		trigger eviction when the cache becomes this full (as a
		percentage)''',
		min=10, max=99),
	Config('verbose', '', r'''
		enable messages for various events.  Options are given as a
		list, such as <code>"verbose=[evictserver,read]"</code>''',
		type='list', choices=[
		    'block',
		    'ckpt',
		    'evict',
		    'evictserver',
		    'fileops',
		    'hazard',
		    'mutex',
		    'read',
		    'readserver',
		    'reconcile',
		    'salvage',
		    'verify',
		    'write']),
]

methods = {
'file.meta' : Method(file_meta),

'colgroup.meta' : Method(colgroup_meta),

'index.meta' : Method(index_meta),

'table.meta' : Method(table_meta),

'cursor.close' : Method([]),

'cursor.reconfigure' : Method(cursor_runtime_config),

'session.close' : Method([]),

'session.create' : Method(table_meta + file_config + filename_meta + [
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

'session.dumpfile' : Method([]),
'session.log_printf' : Method([]),

'session.open_cursor' : Method(cursor_runtime_config + [
	Config('bulk', 'false', r'''
		configure the cursor for bulk loads; bulk-load is a fast
		load path for empty objects, only empty objects may be
		bulk-loaded''',
		type='boolean'),
	Config('checkpoint', '', r'''
		the name of a checkpoint to open'''),
	Config('dump', '', r'''
		configure the cursor for dump format inputs and outputs:
		"hex" selects a simple hexadecimal format, "print"
		selects a format where only non-printing characters are
		hexadecimal encoded.  The cursor dump format is compatible
		with the @ref utility_dump and @ref utility_load commands''',
		choices=['hex', 'print']),
	Config('isolation', 'read-committed', r'''
		the isolation level for this cursor, ignored for transactional
		cursors''',
		choices=['snapshot', 'read-committed', 'read-uncommitted']),
	Config('raw', 'false', r'''
		ignore the encodings for the key and value, manage data as if
		the formats were \c "u".  See @ref cursor_raw for details''',
		type='boolean'),
	Config('statistics', 'false', r'''
		configure the cursor for statistics''',
		type='boolean'),
	Config('statistics_clear', 'false', r'''
		statistics cursors only; reset statistics counters when the
		cursor is closed''',
		type='boolean'),
]),

'session.rename' : Method([]),
'session.salvage' : Method([
	Config('force', 'false', r'''
		force salvage even of files that do not appear to be WiredTiger
		files''',
		type='boolean'),
]),
'session.truncate' : Method([]),
'session.upgrade' : Method([]),
'session.verify' : Method([]),

'session.begin_transaction' : Method([
	Config('isolation', 'snapshot', r'''
		the isolation level for this transaction''',
		choices=['read-uncommitted', 'snapshot']),
	Config('name', '', r'''
		name of the transaction for tracing and debugging'''),
	Config('sync', 'full', r'''
		how to sync log records when the transaction commits''',
		choices=['full', 'flush', 'write', 'none']),
	Config('priority', 0, r'''
		priority of the transaction for resolving conflicts.
		Transactions with higher values are less likely to abort''',
		min='-100', max='100'),
]),

'session.commit_transaction' : Method([]),
'session.rollback_transaction' : Method([]),

'session.checkpoint' : Method([
	Config('drop', '', r'''
		specify a list of checkpoints to drop.
		The list may additionally contain one of the following keys:
		\c "from=all" to drop all checkpoints,
		\c "from=<checkpoint>" to drop all checkpoints after and
		including the named checkpoint, or
		\c "to=<checkpoint>" to drop all checkpoints before and
		including the named checkpoint''', type='list'),
	Config('name', '', r'''
		if non-empty, specify a name for the checkpoint'''),
	Config('target', '', r'''
		if non-empty, checkpoint the list of objects''', type='list'),
]),

'connection.add_collator' : Method([]),
'connection.add_compressor' : Method([]),
'connection.add_data_source' : Method([]),
'connection.add_extractor' : Method([]),
'connection.close' : Method([]),
'connection.reconfigure' : Method(connection_runtime_config),

'connection.load_extension' : Method([
	Config('entry', 'wiredtiger_extension_init', r'''
		the entry point of the extension'''),
	Config('prefix', '', r'''
		a prefix for all names registered by this extension (e.g., to
		make namespaces distinct or during upgrades'''),
]),

'connection.open_session' : Method([]),

'wiredtiger_open' : Method(connection_runtime_config + [
	Config('buffer_alignment', '-1', r'''
		in-memory alignment (in bytes) for buffers used for I/O.  By
		default, a platform-specific alignment value is used (512 bytes
		on Linux systems, zero elsewhere)''',
		min='-1', max='1MB'),
	Config('create', 'false', r'''
		create the database if it does not exist''',
		type='boolean'),
	Config('direct_io', '', r'''
		Use \c O_DIRECT to access files.  Options are given as a
		list, such as <code>"direct_io=[data]"</code>''',
		type='list', choices=['data', 'log']),
	Config('extensions', '', r'''
		list of extensions to load.  Optional values are passed as the
		\c config parameter to WT_CONNECTION::load_extension.  Complex
		paths may need quoting, for example,
		<code>extensions=("/path/to/ext.so"="entry=my_entry")</code>''',
		type='list'),
	Config('hazard_max', '30', r'''
		number of simultaneous hazard references per session handle''',
		min='15'),
	Config('logging', 'false', r'''
		enable logging''',
		type='boolean'),
	Config('multiprocess', 'false', r'''
		permit sharing between processes (will automatically start an
		RPC server for primary processes and use RPC for secondary
		processes). <b>Not yet supported in WiredTiger</b>''',
		type='boolean'),
	Config('session_max', '50', r'''
		maximum expected number of sessions (including server
		threads)''',
		min='1'),
	Config('sync', 'true', r'''
		flush files to stable storage when closing or writing
		checkpoints''',
		type='boolean'),
	Config('transactional', 'true', r'''
		support transactional semantics''',
		type='boolean'),
	Config('use_environment_priv', 'false', r'''
		use the \c WIREDTIGER_CONFIG and \c WIREDTIGER_HOME environment
		variables regardless of whether or not the process is running
		with special privileges.  See @ref home for more information''',
		type='boolean'),
]),
}

flags = {
###################################################
# Internal routine flag declarations
###################################################
	'direct_io' : [ 'DIRECTIO_DATA', 'DIRECTIO_LOG' ],
	'page_free' : [ 'PAGE_FREE_IGNORE_DISK' ],
	'rec_evict' : [ 'REC_SINGLE' ],
	'verbose' : [
		'VERB_block',
		'VERB_ckpt',
		'VERB_evict',
		'VERB_evictserver',
		'VERB_fileops',
		'VERB_hazard',
		'VERB_mutex',
		'VERB_read',
		'VERB_readserver',
		'VERB_reconcile',
		'VERB_salvage',
		'VERB_verify',
		'VERB_write'
	],

###################################################
# Structure flag declarations
###################################################
	'conn' : [
		'CONN_NOSYNC',
		'CONN_TRANSACTIONAL',
		'SERVER_RUN'
	],
	'session' : [
		'SESSION_HAS_CONNLOCK',
		'SESSION_INTERNAL',
		'SESSION_SALVAGE_QUIET_ERR'
	],
}
