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
    # A database-wide checkpoint.
    LogRecordType('checkpoint', [
        ('WT_LSN', 'ckpt_lsn'), ('uint32', 'nsnapshot'), ('item', 'snapshot')]),

    # Common case: a transaction commit
    LogRecordType('commit', [('uint64', 'txnid')]),

    # Mark the start / end of a file sync operation (usually when a file is
    # closed).  These log records aren't required during recovery, but we use
    # the allocated LSN to reduce the amount of work recovery has to do, and
    # they are useful for debugging recovery.
    LogRecordType('file_sync', [('uint32', 'fileid'), ('int', 'start')]),

    # Debugging message in the log
    LogRecordType('message', [('string', 'message')]),
]

class LogOperationType:
    def __init__(self, name, fields):
        self.name = name
        self.fields = fields

    def macro_name(self):
        return 'WT_LOGOP_%s' % self.name.upper()

optypes = [
    LogOperationType('col_put',
        [('uint32', 'fileid'), ('recno', 'recno'), ('item', 'value')]),
    LogOperationType('col_remove',
        [('uint32', 'fileid'), ('recno', 'recno')]),
    LogOperationType('col_truncate',
        [('uint32', 'fileid'), ('recno', 'start'), ('recno', 'stop')]),
    LogOperationType('row_put',
        [('uint32', 'fileid'), ('item', 'key'), ('item', 'value')]),
    LogOperationType('row_remove',
        [('uint32', 'fileid'), ('item', 'key')]),
    LogOperationType('row_truncate',
        [('uint32', 'fileid'), ('item', 'start'), ('item', 'stop'),
            ('uint32', 'mode')]),
]
