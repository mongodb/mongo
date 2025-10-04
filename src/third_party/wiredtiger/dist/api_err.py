#!/usr/bin/env python3

# Output C #defines for errors into wiredtiger.h.in and the associated error
# message code in strerror.c.

import os, sys, textwrap
from dist import compare_srcfile, format_srcfile
from common_functions import filter_if_fast

# Checks if return section begins and updates error defines
def check_write_errors(tfile, skip, err_type, errors):
    if line.count(err_type + ' return section: END'):
        tfile.write(line)
        skip = 0
    elif line.count(err_type + ' return section: BEGIN'):
        skip = 1
        tfile.write(' */\n')
        for err in errors:
            if 'undoc' in err.flags:
                tfile.write('/*! @cond internal */\n')
            tfile.write('/*!%s.%s */\n' %
                (('\n * ' if err.long_desc else ' ') +
            err.desc[0].upper() + err.desc[1:],
                ''.join('\n * ' + l for l in textwrap.wrap(
            textwrap.dedent(err.long_desc).strip(), 77)) +
        '\n' if err.long_desc else ''))
            tfile.write('#define\t%s\t(%d)\n' % (err.name, err.value))
            if 'undoc' in err.flags:
                tfile.write('/*! @endcond */\n')
        tfile.write('/*\n')
    return skip

def write_err_cases(tfile, err_type, errors):
    tfile.write('''
    \t/* Check for WiredTiger specific %s. */
    \tswitch (error) {
    ''' % err_type)
    for err in errors:
        tfile.write('\tcase ' + err.name + ':\n')
        tfile.write('\t\treturn ("' + err.name + ': ' + err.desc + '");\n')
    tfile.write('''\t}\n''')

# Checks if lines should be skipped and updates documentation block
def check_write_document_errors(tfile, skip, err_type, errors):
    if line.count(f'IGNORE_BUILT_BY_API_{err_type}_END'):
        tfile.write(line)
        skip = 0
    elif line.count(f'IGNORE_BUILT_BY_API_{err_type}_BEGIN'):
        tfile.write('@endif\n\n')
        skip = 1
        for err in errors:
            if 'undoc' in err.flags:
                continue
            tfile.write(
                '@par \\c ' + err.name.upper() + '\n' +
                " ".join(err.long_desc.split()) + '\n\n')
    return skip

if not [f for f in filter_if_fast([
            "../src/conn/api_strerror.c",
            "../src/docs/error-handling.dox",
            "../src/include/wiredtiger.h.in",
        ], prefix="../")]:
    sys.exit(0)


class Error:
    def __init__(self, name, value, desc, long_desc=None, **flags):
        self.name = name
        self.value = value
        self.desc = desc
        self.long_desc = long_desc
        self.flags = flags

# We don't want our error returns to conflict with any other package,
# so use an uncommon range, specifically, -31,800 to -31,999.
#
# These numbers cannot change without breaking backward compatibility,
# and are listed in error value order.
errors = [
    Error('WT_ROLLBACK', -31800,
        'conflict between concurrent operations', '''
        This error is generated when an operation cannot be completed
        due to a conflict with concurrent operations.  The operation
        may be retried; if a transaction is in progress, it should be
        rolled back and the operation retried in a new transaction.'''),
    Error('WT_DUPLICATE_KEY', -31801,
        'attempt to insert an existing key', '''
        This error is generated when the application attempts to insert
        a record with the same key as an existing record without the
        'overwrite' configuration to WT_SESSION::open_cursor.'''),
    Error('WT_ERROR', -31802,
        'non-specific WiredTiger error', '''
        This error is returned when an error is not covered by a specific error
        return. The operation may be retried; if a transaction is in progress,
        it should be rolled back and the operation retried in a new transaction.'''),
    Error('WT_NOTFOUND', -31803,
        'item not found', '''
        This error indicates an operation did not find a value to
        return.  This includes cursor search and other operations
        where no record matched the cursor's search key such as
        WT_CURSOR::update or WT_CURSOR::remove.'''),
    Error('WT_PANIC', -31804,
        'WiredTiger library panic', '''
        This error indicates an underlying problem that requires a database
        restart. The application may exit immediately, no further WiredTiger
        calls are required (and further calls will themselves immediately
        fail).'''),
    Error('WT_RESTART', -31805,
        'restart the operation (internal)', undoc=True),
    Error('WT_RUN_RECOVERY', -31806,
        'recovery must be run to continue', '''
        This error is generated when ::wiredtiger_open is configured to return
        an error if recovery is required to use the database.'''),
    Error('WT_CACHE_FULL', -31807,
        'operation would overflow cache', '''
        This error is generated when wiredtiger_open is configured to run in-memory, and a
        data modification operation requires more than the configured cache size to complete.
        The operation may be retried; if a transaction is in progress, it should be rolled back
        and the operation retried in a new transaction.'''),
    Error('WT_PREPARE_CONFLICT', -31808,
        'conflict with a prepared update', '''
        This error is generated when the application attempts to read an
        updated record which is part of a transaction that has been prepared
        but not yet resolved.'''),
    Error('WT_TRY_SALVAGE', -31809,
        'database corruption detected', '''
        This error is generated when corruption is detected in an on-disk file.
        During normal operations, this may occur in rare circumstances as a
        result of a system crash. The application may choose to salvage the
        file or retry wiredtiger_open with the 'salvage=true' configuration
        setting.'''),
]

# To ensure our sub-level error returns do not conflict with any other
# package or the error returns, we use the range -32,000 to -32,199.
#
# These numbers cannot change without breaking backward compatibility,
# and are listed in error value order.
sub_errors = [
    Error('WT_NONE', -32000,
        'No additional context', '''
        This sub-level error code is returned by default and indicates that no
        further context exists or is necessary.'''),
    Error('WT_BACKGROUND_COMPACT_ALREADY_RUNNING', -32001,
        "Background compaction is already running", '''
        This sub-level error returns when the user tries to reconfigure background
        compaction while it is already running.'''),
    Error('WT_CACHE_OVERFLOW', -32002,
        "Cache capacity has overflown", '''
        This sub-level error indicates that the configured cache has exceeded full
        capacity.'''),
    Error('WT_WRITE_CONFLICT', -32003,
        "Write conflict between concurrent operations", '''
        This sub-level error indicates that there is a write conflict on the same
        page between concurrent operations.'''),
    Error('WT_OLDEST_FOR_EVICTION', -32004,
        "Transaction has the oldest pinned transaction ID", '''
        This sub-level error indicates that a given transaction has the oldest
        transaction ID and needs to be rolled back.'''),
    Error('WT_CONFLICT_BACKUP', -32005,
        "Conflict performing operation due to running backup", '''
        This sub-level error indicates that there is a conflict performing the operation
        because of a running backup in the system.'''),
    Error('WT_CONFLICT_DHANDLE', -32006,
        "Another thread currently holds the data handle of the table", '''
        This sub-level error indicates that a concurrent operation is holding the data
        handle of the table.'''),
    Error('WT_CONFLICT_SCHEMA_LOCK', -32007,
        "Conflict performing schema operation", '''
        This sub-level error indicates that a concurrent operation is performing a schema
        type operation or currently holds the schema lock.'''),
    Error('WT_UNCOMMITTED_DATA', -32008,
        "Table has uncommitted data", '''
        This sub-level error returns when the table has uncommitted data.'''),
    Error('WT_DIRTY_DATA', -32009,
        "Table has dirty data", '''
        This sub-level error returns when the table has dirty content.'''),
    Error('WT_CONFLICT_TABLE_LOCK', -32010,
        "Another thread currently holds the table lock", '''
        This sub-level error indicates that a concurrent operation is performing
        a table operation.'''),
    Error('WT_CONFLICT_CHECKPOINT_LOCK', -32011,
        "Another thread currently holds the checkpoint lock", '''
        This sub-level error indicates that a concurrent operation is performing
        a checkpoint.'''),
    Error('WT_MODIFY_READ_UNCOMMITTED', -32012,
        "Read-uncommitted readers do not support reconstructing a record with modifies", '''
        This sub-level error indicates that a reader with uncommitted isolation 
        is trying to reconstruct a record with modifies. This is not supported.'''),
    Error('WT_CONFLICT_LIVE_RESTORE', -32013,
        "Conflict performing operation due to an in-progress live restore", '''
        This sub-level error indicates that there is a conflict performing the operation
        because of a running live restore in the system.'''),
    Error('WT_CONFLICT_DISAGG', -32014,
        "Conflict with disaggregated storage", '''
        This sub-level error indicates that an operation or configuration conflicts with
        disaggregated storage.'''),     
]

# Update the #defines in the wiredtiger.h.in file.
tmp_file = '__tmp_api_err' + str(os.getpid())
tfile = open(tmp_file, 'w')
skip = 0
for line in open('../src/include/wiredtiger.h.in', 'r'):
    if not skip:
        tfile.write(line)
    skip = check_write_errors(tfile, skip, 'Error', errors)
    skip = check_write_errors(tfile, skip, 'Sub-level error', sub_errors)
tfile.close()
compare_srcfile(tmp_file, '../src/include/wiredtiger.h.in')

# Output the wiredtiger_strerror and wiredtiger_sterror_r code.
tmp_file = '__tmp_api_err' + str(os.getpid())
tfile = open(tmp_file, 'w')
tfile.write('''/* DO NOT EDIT: automatically built by dist/api_err.py. */

#include "wt_internal.h"

/*
 * Historically, there was only the wiredtiger_strerror call because the POSIX
 * port didn't need anything more complex; Windows requires memory allocation
 * of error strings, so we added the WT_SESSION.strerror method. Because we
 * want wiredtiger_strerror to continue to be as thread-safe as possible, errors
 * are split into two categories: WiredTiger's or the system's constant strings
 * and Everything Else, and we check constant strings before Everything Else.
 */

/*
 * __wt_wiredtiger_error --
 *\tReturn a constant string for POSIX-standard and WiredTiger errors.
 */
const char *
__wt_wiredtiger_error(int error)
{''')

write_err_cases(tfile, 'errors', errors)
write_err_cases(tfile, 'sub-level errors', sub_errors)

tfile.write('''\n\t/* Windows strerror doesn't support ENOTSUP. */
\tif (error == ENOTSUP)
\t\treturn ("Operation not supported");

\t/*
\t * Check for 0 in case the underlying strerror doesn't handle it, some
\t * historically didn't.
\t */
\tif (error == 0)
\t\treturn ("Successful return: 0");

\t/* POSIX errors are non-negative integers. */
\tif (error > 0)
\t\treturn (strerror(error));

\treturn (NULL);
}

/*
 * wiredtiger_strerror --
 *\tReturn a string for any error value, non-thread-safe version.
 */
const char *
wiredtiger_strerror(int error)
{
\tstatic char buf[128];

\treturn (__wt_strerror(NULL, error, buf, sizeof(buf)));
}

/*
 * __wt_is_valid_sub_level_error --
 *\tReturn true if the provided error falls within the valid range for sub level error codes, 
 *\treturn false otherwise.
 */
bool
__wt_is_valid_sub_level_error(int sub_level_err)
{
\treturn (sub_level_err <= -32000 && sub_level_err > -32200);
}
''')
tfile.close()
format_srcfile(tmp_file)
compare_srcfile(tmp_file, '../src/conn/api_strerror.c')

# Update the error documentation block.
doc = '../src/docs/error-handling.dox'
tmp_file = '__tmp_api_err' + str(os.getpid())
tfile = open(tmp_file, 'w')
skip = 0
for line in open(doc, 'r'):
    if not skip:
        tfile.write(line)
    skip = check_write_document_errors(tfile, skip, 'ERR', errors)
    skip = check_write_document_errors(tfile, skip, 'SUB_ERR', sub_errors)
    
tfile.close()
format_srcfile(tmp_file)
compare_srcfile(tmp_file, doc)
