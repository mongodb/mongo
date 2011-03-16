# Data for config.py, describes all configuration key / value pairs

class Config:
	def __init__(self, name, default, desc):
		self.name = name
		self.default = default
		self.desc = desc

config_types = {
'cursor_close' : [],
'truncate_table' : [],
'verify_table' : [],
'commit_transaction' : [],
'rollback_transaction' : [],
'session_close' : [],

'open_cursor' : [
	Config('isolation', 'read-committed', r'''
		the isolation level for this cursor, one of "snapshot" or
		"read-committed" or "read-uncommitted".  Ignored for transactional
		cursors'''),
	Config('overwrite', 'false', r'''
		if an existing key is inserted, overwrite the existing value'''),
	Config('raw', 'false', r'''
		ignore the encodings for the key and value, manage data as if the
		formats were \c "u"'''),
],

'create_table' : [
	Config('columns', '', r'''
		list of the column names.  Comma-separated list of the form
		<code>(column[,...])</code>.  The number of entries must match the
		total number of values in \c key_format and \c value_format.'''),
	Config('colgroup.name', '', r'''
		named group of columns to store together.  Comma-separated list of
		the form <code>(column[,...])</code>.  Each column group is stored
		separately, keyed by the primary key of the table.  Any column that
		does not appear in a column group is stored in a default, unnamed,
		column group for the table.'''),
	Config('exclusive', 'false', r'''
		fail if the table exists (if "no", the default, verifies that the
		table exists and has the specified schema.'''),
	Config('index.name', '', r'''
		named index on a list of columns.  Comma-separated list of the form
		<code>(column[,...])</code>.'''),
	Config('key_format', '', r'''
		the format of the data packed into key items.  See @ref packing for
		details.  If not set, a default value of \c "u" is assumed, and
		applications use the WT_ITEM struct to manipulate raw byte arrays.'''),
	Config('value_format', '', r'''
		the format of the data packed into value items.  See @ref packing
		for details.  If not set, a default value of \c "u" is assumed, and
		applications use the WT_ITEM struct to manipulate raw byte arrays.'''),
],

'rename_table' : [],

'begin_transaction' : [
	Config('isolation', 'read-committed', r'''
		the isolation level for this transaction, one of "serializable",
		"snapshot", "read-committed" or "read-uncommitted"; default
		"serializable"'''),
	Config('name', '', r'''
		name of the transaction for tracing and debugging'''),
	Config('sync', 'full', r'''
		how to sync log records when the transaction commits, one of
		"full", "flush", "write" or "none"'''),
	Config('priority', 0, r'''
		priority of the transaction for resolving conflicts, an integer
		between -100 and 100.  Transactions with higher values are less likely
		to abort'''),
],

'checkpoint' : [
	Config('archive', 'false', r'''
		remove log files no longer required for transactional durabilty'''),
	Config('force', 'false', r'''
		write a new checkpoint even if nothing has changed since the last
		one'''),
	Config('flush_cache', 'true', r'''
		flush the cache'''),
	Config('flush_log', 'true', r'''
		flush the log'''),
	Config('log_size', '0', r'''
		only proceed if more than the specified number of bytes of log
		records have been written since the last checkpoint'''),
	Config('timeout', '0', r'''
		only proceed if more than the specified number of milliseconds have
		elapsed since the last checkpoint'''),
],

'add_cursor_type' : [],
'add_collator' : [],
'add_extractor' : [],
'connection_close' : [],

'load_extension' : [
	Config('entry', 'wiredtiger_extension_init', r'''
		the entry point of the extension'''),
	Config('prefix', '', r'''
		a prefix for all names registered by this extension (e.g., to make
		namespaces distinct or during upgrades'''),
],

'wiredtiger_open' : [
	Config('cache_size', '20MB', r'''
		maximum heap memory to allocate for the cache'''),
	Config('create', 'false', r'''
		create the database if it does not exist'''),
	Config('data_update_min', '8KB', r'''
		minimum update buffer size for a session'''),
	Config('data_update_max', '32KB', r'''
		maximum update buffer size for a session'''),
	Config('exclusive', 'false', r'''
		fail if the database already exists'''),
	Config('error_prefix', '', r'''
		Prefix string for error messages'''),
	Config('hazard_size', '15', r'''
		number of hazard references per session'''),
	Config('logging', 'false', r'''
		enable logging'''),
	Config('session_max', '50', r'''
		maximum expected number of sessions (including server threads)'''),
	Config('multiprocess', 'false', r'''
		permit sharing between processes (will automatically start an RPC
		server for primary processes and use RPC for secondary processes)'''),
	Config('verbose', '', r'''
		enable messages for various events.  One or more of "all", "evict",
		"fileops", "hazard", "mutex", "read".  Multiple options are given as
		a list such as \c "verbose=[evict,read]"'''),
],
}
