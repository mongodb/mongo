# Output C #defines for errors into wiredtiger.in and the associated error
# message code in strerror.c.

import re, textwrap
from dist import compare_srcfile, format_srcfile

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
        This error is returned when an error is not covered by a
        specific error return.'''),
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
        This error is generated when wiredtiger_open is configured
        to return an error if recovery is required to use the database.'''),
    Error('WT_CACHE_FULL', -31807,
        'operation would overflow cache', '''
        This error is only generated when wiredtiger_open is configured
        to run in-memory, and an insert or update operation requires
        more than the configured cache size to complete, or when an
        application thread fails to do eviction within cache_max_wait_ms.
        The operation may be retried; if a transaction is in progress, it
        should be rolled back and the operation retried in a new transaction.'''),
    Error('WT_PREPARE_CONFLICT', -31808,
        'conflict with a prepared update', '''
        This error is generated when the application attempts to update
        an already updated record which is in prepared state. An updated
        record will be in prepared state, when the transaction that performed
        the update is in prepared state.'''),
    Error('WT_TRY_SALVAGE', -31809,
        'database corruption detected', '''
        This error is generated when corruption is detected in an on-disk file.
        During normal operations, this may occur in rare circumstances as a
        result of a system crash. The application may choose to salvage the
        file or retry wiredtiger_open with the 'salvage=true' configuration
        setting.'''),
]

# Update the #defines in the wiredtiger.in file.
tmp_file = '__tmp'
tfile = open(tmp_file, 'w')
skip = 0
for line in open('../src/include/wiredtiger.in', 'r'):
    if not skip:
        tfile.write(line)
    if line.count('Error return section: END'):
        tfile.write(line)
        skip = 0
    elif line.count('Error return section: BEGIN'):
        tfile.write(' */\n')
        skip = 1
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
tfile.close()
compare_srcfile(tmp_file, '../src/include/wiredtiger.in')

# Output the wiredtiger_strerror and wiredtiger_sterror_r code.
tmp_file = '__tmp'
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
{
\t/*
\t * Check for WiredTiger specific errors.
\t */
\tswitch (error) {
''')

for err in errors:
    tfile.write('\tcase ' + err.name + ':\n')
    tfile.write('\t\treturn ("' + err.name + ': ' + err.desc + '");\n')
tfile.write('''\t}

\t/* Windows strerror doesn't support ENOTSUP. */
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
''')
tfile.close()
format_srcfile(tmp_file)
compare_srcfile(tmp_file, '../src/conn/api_strerror.c')

# Update the error documentation block.
doc = '../src/docs/error-handling.dox'
tmp_file = '__tmp'
tfile = open(tmp_file, 'w')
skip = 0
for line in open(doc, 'r'):
    if not skip:
        tfile.write(line)
    if line.count('IGNORE_BUILT_BY_API_ERR_END'):
        tfile.write(line)
        skip = 0
    elif line.count('IGNORE_BUILT_BY_API_ERR_BEGIN'):
        tfile.write('@endif\n\n')
        skip = 1

        for err in errors:
            if 'undoc' in err.flags:
                continue
            tfile.write(
                '@par <code>' + err.name.upper() + '</code>\n' +
                " ".join(err.long_desc.split()) + '\n\n')
tfile.close()
format_srcfile(tmp_file)
compare_srcfile(tmp_file, doc)
