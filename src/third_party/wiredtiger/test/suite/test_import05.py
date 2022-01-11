#!/usr/bin/env python
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.
#
# test_import05.py
# Error conditions when trying to import files with timestamps past the configured global timestamp.

import os, shutil
import wiredtiger
from wtscenario import make_scenarios
from test_import01 import test_import_base

class test_import05(test_import_base):
    conn_config = 'cache_size=50MB,log=(enabled)'

    ntables = 10
    nrows = 100
    keys = [b'1', b'2', b'3', b'4', b'5', b'6']
    values = [b'\x01\x02aaa\x03\x04', b'\x01\x02bbb\x03\x04', b'\x01\x02ccc\x03\x04',
              b'\x01\x02ddd\x03\x04', b'\x01\x02eee\x03\x04', b'\x01\x02fff\x03\x04']
    ts = [10*k for k in range(1, len(keys)+1)]
    optypes = [
        ('insert', dict(op_type='insert')),
        ('delete', dict(op_type='delete')),
    ]
    import_types = [
        ('file_metadata', dict(repair=False)),
        ('repair', dict(repair=True)),
    ]
    compare_timestamps = [
        ('oldest', dict(global_ts='oldest')),
        ('stable', dict(global_ts='stable')),
    ]
    scenarios = make_scenarios(optypes, import_types, compare_timestamps)

    def test_file_import_ts_past_global_ts(self):
        original_db_file = 'original_db_file'
        uri = 'file:' + original_db_file
        create_config = 'allocation_size=512,key_format=u,log=(enabled=true),value_format=u'
        self.session.create(uri, create_config)

        # Add data and perform a checkpoint.
        # We're inserting everything EXCEPT the last record.
        for i in range(0, len(self.keys) - 1):
            self.update(uri, self.keys[i], self.values[i], self.ts[i])

        self.session.checkpoint()

        # Place the last insert/delete.
        # We also want to check that a stop timestamp later than oldest will prevent imports. In the
        # delete case, we should use the last timestamp in our data set and use it delete the first
        # key we inserted.
        if self.op_type == 'insert':
            self.update(uri, self.keys[-1], self.values[-1], self.ts[-1])
        else:
            self.assertEqual(self.op_type, 'delete')
            self.delete(uri, self.keys[0], self.ts[-1])

        # Perform a checkpoint.
        self.session.checkpoint()

        # Export the metadata for the table.
        c = self.session.open_cursor('metadata:', None, None)
        original_db_file_config = c[uri]
        c.close()

        self.printVerbose(3, '\nFile configuration:\n' + original_db_file_config)

        # Close the connection.
        self.close_conn()

        # Create a new database and connect to it.
        newdir = 'IMPORT_DB'
        shutil.rmtree(newdir, ignore_errors=True)
        os.mkdir(newdir)
        self.conn = self.setUpConnectionOpen(newdir)
        self.session = self.setUpSessionOpen(self.conn)

        # Copy over the datafiles for the object we want to import.
        self.copy_file(original_db_file, '.', newdir)

        # Contruct the config string.
        if self.repair:
            if self.global_ts == 'stable':
                import_config = 'import=(enabled,repair=true,compare_timestamp=stable_timestamp)'
            else:
                import_config = 'import=(enabled,repair=true)'
        else:
            if self.global_ts == 'stable':
                import_config = 'import=(enabled,repair=false,compare_timestamp=stable_timestamp,file_metadata=(' + \
                    original_db_file_config + '))'
            else:
                import_config = 'import=(enabled,repair=false,file_metadata=(' + \
                    original_db_file_config + '))'

        # Create error pattern. Depending on the situation, we substitute a different timestamp into
        # error message to check against.
        error_pattern = 'import found aggregated {} timestamp newer than the current'

        # Now begin trying to import the file.
        #
        # Since we haven't set oldest (and it defaults to 0), we're expecting an error here as the
        # table has timestamps past 0.
        #
        # Start timestamps get checked first so that's the error msg we expect.
        error_msg = error_pattern.format('newest start durable')

        with self.expectedStderrPattern(error_msg):
            self.assertRaisesException(wiredtiger.WiredTigerError,
                lambda: self.session.create(uri, import_config))

        # Place the oldest timestamp just BEFORE the last insert/delete we made.
        #
        # The table we're importing had an operation past this point so we're still expecting an
        # error.
        if self.global_ts == 'stable':
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(self.ts[-1] - 1))   
        else: 
            self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(self.ts[-1] - 1))

        # If our latest operation was an insert, we're expecting it to complain about the aggregated
        # start timestamp whereas if we did a delete, we should expect it to complain about stop.
        error_msg = error_pattern.format(
            'newest start durable' if self.op_type == 'insert' else 'newest stop durable')

        with self.expectedStderrPattern(error_msg):
            self.assertRaisesException(wiredtiger.WiredTigerError,
                lambda: self.session.create(uri, import_config))

        # Now place global timestamp equal to the last insert/delete we made. This should succeed
        # since all of our aggregated timestamps are now equal to or behind the global timestamp.
        if self.global_ts == 'stable':
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(self.ts[-1]))   
        else: 
            self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(self.ts[-1]))
        self.session.create(uri, import_config)
