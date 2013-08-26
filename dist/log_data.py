# Data for log.py, describes the format of log records

# There are a small number of main log record types.
# 
# Some log record types, such as transaction commit, also include a list of
# "log operations" within the same log record.  Both log record types and log
# operations are described here.

class LogRecordType:
	def __init__(self, name, fields):
		self.name = name
		self.fields = fields

	def macro_name(self):
		return 'WT_LOGREC_%s' % self.name.upper()

	def prname(self):
		return '__logrec_print_' + self.name

rectypes = [
	LogRecordType('invalid', []),
	LogRecordType('checkpoint', [('string', 'uri'), ('int', 'start')]),
	LogRecordType('commit', [('uint64', 'txnid')]),
	LogRecordType('debug', [('string', 'message')]),
]

class LogOperationType:
	def __init__(self, name, fields):
		self.name = name
		self.fields = fields

	def macro_name(self):
		return 'WT_LOGOP_%s' % self.name.upper()

optypes = [
	LogRecordType('invalid', []),
	LogOperationType('col_put',
		[('string', 'uri'), ('recno', 'recno'), ('item', 'value')]),
	LogOperationType('col_remove',
		[('string', 'uri'), ('recno', 'recno')]),
	LogOperationType('row_put',
		[('string', 'uri'), ('item', 'key'), ('item', 'value')]),
	LogOperationType('row_remove',
		[('string', 'uri'), ('item', 'key')]),
]
