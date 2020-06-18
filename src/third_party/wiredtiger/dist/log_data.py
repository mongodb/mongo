# Data for log.py, describes the format of log records

# There are a small number of main log record types.
#
# Some log record types, such as transaction commit, also include a list of
# "log operations" within the same log record.  Both log record types and log
# operations are described here.

class LogRecordType:
    def __init__(self, name, desc, fields):
        self.name = name
        self.desc = desc
        self.fields = fields

    def macro_name(self):
        return 'WT_LOGREC_%s' % self.name.upper()

    def prname(self):
        return '__logrec_print_' + self.name

#
# If you add a new record type you must also add its record type value in
# src/include/wiredtiger.in.  The values cannot be generated because they must
# never change after they're written in a log file.
#
rectypes = [
    # A database-wide checkpoint.
    LogRecordType('checkpoint', 'checkpoint', [
        ('WT_LSN', 'ckpt_lsn'), ('uint32', 'nsnapshot'), ('item', 'snapshot')]),

    # Common case: a transaction commit
    LogRecordType('commit', 'transaction commit', [('uint64', 'txnid')]),

    # Mark the start / end of a file sync operation (usually when a file is
    # closed).  These log records aren't required during recovery, but we use
    # the allocated LSN to reduce the amount of work recovery has to do, and
    # they are useful for debugging recovery.
    LogRecordType('file_sync', 'file sync', [
        ('uint32_id', 'fileid'), ('int', 'start')]),

    # Debugging message in the log
    LogRecordType('message', 'message', [('string', 'message')]),

    # System (internal) log record
    LogRecordType('system', 'system', []),
]

class LogOperationType:
    def __init__(self, name, desc, fields):
        self.name = name
        self.desc = desc
        self.fields = fields

    def macro_name(self):
        return 'WT_LOGOP_%s' % self.name.upper()

#
# If you add a new operation type you must also add its type value in
# src/include/wiredtiger.in.  The values cannot be generated because they must
# never change after they're written in a log file.
#
optypes = [
# commit operations
    LogOperationType('col_modify', 'column modify',
        [('uint32_id', 'fileid'), ('recno', 'recno'), ('item', 'value')]),
    LogOperationType('col_put', 'column put',
        [('uint32_id', 'fileid'), ('recno', 'recno'), ('item', 'value')]),
    LogOperationType('col_remove', 'column remove',
        [('uint32_id', 'fileid'), ('recno', 'recno')]),
    LogOperationType('col_truncate', 'column truncate',
        [('uint32_id', 'fileid'), ('recno', 'start'), ('recno', 'stop')]),
    LogOperationType('row_modify', 'row modify',
        [('uint32_id', 'fileid'), ('item', 'key'), ('item', 'value')]),
    LogOperationType('row_put', 'row put',
        [('uint32_id', 'fileid'), ('item', 'key'), ('item', 'value')]),
    LogOperationType('row_remove', 'row remove',
        [('uint32_id', 'fileid'), ('item', 'key')]),
    LogOperationType('row_truncate', 'row truncate',
        [('uint32_id', 'fileid'), ('item', 'start'), ('item', 'stop'),
            ('uint32', 'mode')]),

# system operations
    LogOperationType('checkpoint_start', 'checkpoint start', []),
    LogOperationType('prev_lsn', 'previous LSN',
        [('WT_LSN', 'prev_lsn')]),

# diagnostic operations
# Operations used only for diagnostic purposes should be have their type
# values in the diagnostic range in src/include/wiredtiger.in so that they
# are always ignored by recovery.
    #
    # We need to know the base size/type of a 'struct timespec'. Cast its
    # parts to uint64_t and split it into seconds and nanoseconds.
    #
    LogOperationType('txn_timestamp', 'txn_timestamp',
        [('uint64', 'time_sec'), ('uint64', 'time_nsec'),
            ('uint64', 'commit_ts'), ('uint64', 'durable_ts'),
            ('uint64', 'first_commit_ts'), ('uint64', 'prepare_ts'),
            ('uint64', 'read_ts')]),
]
