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
# test_import02.py
# Error conditions when trying to import files.

import os, shutil
import wiredtiger, wttest
from test_import01 import test_import_base

class test_import02(test_import_base):
    conn_config = 'cache_size=50MB,log=(enabled)'

    original_db_file = 'original_db_file'
    uri = 'file:' + original_db_file

    nrows = 100
    ntables = 10
    keys = [b'1', b'2', b'3', b'4', b'5', b'6']
    values = [b'\x01\x02aaa\x03\x04', b'\x01\x02bbb\x03\x04', b'\x01\x02ccc\x03\x04',
              b'\x01\x02ddd\x03\x04', b'\x01\x02eee\x03\x04', b'\x01\x02fff\x03\x04']
    ts = [10*k for k in range(1, len(keys)+1)]
    create_config = 'allocation_size=512,key_format=u,log=(enabled=true),value_format=u'

    # The cases where 'file_metadata' is empty or the config option itself is missing entirely are
    # almost identical. Let's capture this in a helper and call them from each test.
    def no_metadata_helper(self, import_config):
        self.session.create(self.uri, self.create_config)

        # Add data and perform a checkpoint.
        for i in range(0, len(self.keys)):
            self.update(self.uri, self.keys[i], self.values[i], self.ts[i])
        self.session.checkpoint()

        # Close the connection.
        self.close_conn()

        # Create a new database and connect to it.
        newdir = 'IMPORT_DB'
        shutil.rmtree(newdir, ignore_errors=True)
        os.mkdir(newdir)
        self.conn = self.setUpConnectionOpen(newdir)
        self.session = self.setUpSessionOpen(self.conn)

        # Bring forward the oldest to be past or equal to the timestamps we'll be importing.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(self.ts[-1]))

        # Copy over the datafiles for the object we want to import.
        self.copy_file(self.original_db_file, '.', newdir)

        # Import the file.
        # Since we need "file_metadata" without the "repair" option, we should expect an error here.
        with self.expectedStderrPattern(
            'file:original_db_file: import requires that \'file_metadata\' is specified'):
            self.assertRaisesException(wiredtiger.WiredTigerError,
                lambda: self.session.create(self.uri, import_config))

    def test_file_import_empty_metadata(self):
        self.no_metadata_helper('import=(enabled,repair=false,file_metadata="")')

    def test_file_import_no_metadata(self):
        self.no_metadata_helper('import=(enabled,repair=false)')

    def test_file_import_existing_uri(self):
        self.session.create(self.uri, self.create_config)

        # Add data and perform a checkpoint.
        for i in range(0, len(self.keys)):
            self.update(self.uri, self.keys[i], self.values[i], self.ts[i])
        self.session.checkpoint()

        # Export the metadata for the table.
        c = self.session.open_cursor('metadata:', None, None)
        original_db_file_config = c[self.uri]
        c.close()

        self.printVerbose(3, '\nFile configuration:\n' + original_db_file_config)

        # Make a bunch of files and fill them with data.
        self.populate(self.ntables, self.nrows)

        # Bring forward the oldest to be past or equal to the timestamps we'll be importing.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(self.ts[-1]))

        # Contruct the config string.
        import_config = 'import=(enabled,repair=false,file_metadata=(' + \
            original_db_file_config + '))'

        # Try to import the file even though it already exists in our database.
        # We should get an error back.
        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: self.session.create(self.uri, import_config))

    def test_import_file_missing_file(self):
        # Make a bunch of files and fill them with data.
        self.populate(self.ntables, self.nrows)
        self.session.checkpoint()

        # Export the metadata for one of the files we made.
        # We just need an example of what a file configuration would typically look like.
        cursor = self.session.open_cursor('metadata:', None, None)
        for k, v in cursor:
            if k.startswith('table:'):
                example_db_file_config = cursor[k]
                break
        cursor.close()

        self.printVerbose(3, '\nFile configuration:\n' + example_db_file_config)

        # Contruct the config string.
        import_config = 'import=(enabled,repair=false,file_metadata=(' + \
            example_db_file_config + '))'

        # Try to import a file that doesn't exist on disk.
        # We should get an error back.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.create(self.uri, import_config), '/No such file or directory/')
