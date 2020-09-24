#!/usr/bin/env python
#
# Public Domain 2014-2020 MongoDB, Inc.
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

def timestamp_str(t):
    return '%x' % t

class test_import02(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=50MB,log=(enabled),statistics=(all)'
    session_config = 'isolation=snapshot'

    def update(self, uri, key, value, commit_ts):
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        cursor[key] = value
        self.session.commit_transaction('commit_timestamp=' + timestamp_str(commit_ts))
        cursor.close()

    # Helper for populating a database to simulate importing files into an existing database.
    def populate(self):
        # Create file:test_import02_[1-100].
        for fileno in range(1, 100):
            uri = 'file:test_import02_{}'.format(fileno)
            self.session.create(uri, 'key_format=i,value_format=S')
            cursor = self.session.open_cursor(uri)
            # Insert keys [1-100] with value 'foo'.
            for key in range(1, 100):
                cursor[key] = 'foo'
            cursor.close()

    def copy_file(self, file_name, old_dir, new_dir):
        old_path = os.path.join(old_dir, file_name)
        if os.path.isfile(old_path) and "WiredTiger.lock" not in file_name and \
            "Tmplog" not in file_name and "Preplog" not in file_name:
            shutil.copy(old_path, new_dir)

    # The cases where 'file_metadata' is empty or the config option itself is missing entirely are
    # almost identical. Let's capture this in a helper and call them from each test.
    def no_metadata_helper(self, import_config):
        original_db_file = 'original_db_file'
        uri = 'file:' + original_db_file

        create_config = 'allocation_size=512,key_format=u,log=(enabled=true),value_format=u'
        self.session.create(uri, create_config)

        key1 = b'1'
        key2 = b'2'
        value1 = b'\x01\x02aaa\x03\x04'
        value2 = b'\x01\x02bbb\x03\x04'

        # Add some data.
        self.update(uri, key1, value1, 10)
        self.update(uri, key2, value2, 20)

        # Perform a checkpoint.
        self.session.checkpoint()

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

        # Import the file.
        # Since we need "file_metadata" without the "repair" option, we should expect an error here.
        with self.expectedStderrPattern(
            'file:original_db_file: import requires that \'file_metadata\' is specified'):
            self.assertRaisesException(wiredtiger.WiredTigerError,
                lambda: self.session.create(uri, import_config))

    def test_file_import_empty_metadata(self):
        self.no_metadata_helper('import=(enabled,repair=false,file_metadata="")')

    def test_file_import_no_metadata(self):
        self.no_metadata_helper('import=(enabled,repair=false)')

    def test_file_import_existing_uri(self):
        original_db_file = 'original_db_file'
        uri = 'file:' + original_db_file

        create_config = 'allocation_size=512,key_format=u,log=(enabled=true),value_format=u'
        self.session.create(uri, create_config)

        key1 = b'1'
        key2 = b'2'

        value1 = b'\x01\x02aaa\x03\x04'
        value2 = b'\x01\x02bbb\x03\x04'

        # Add some data.
        self.update(uri, key1, value1, 10)
        self.update(uri, key2, value2, 20)

        # Perform a checkpoint.
        self.session.checkpoint()

        # Export the metadata for the table.
        c = self.session.open_cursor('metadata:', None, None)
        original_db_file_config = c[uri]
        c.close()

        self.printVerbose(3, '\nFILE CONFIG\n' + original_db_file_config)

        # Make a bunch of files and fill them with data.
        self.populate()

        # Contruct the config string.
        import_config = 'import=(enabled,repair=false,file_metadata=(' + \
            original_db_file_config + '))'

        # Try to import the file even though it already exists in our database.
        # We should get an error back.
        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: self.session.create(uri, import_config))

    def test_import_file_missing_file(self):
        original_db_file = 'original_db_file'
        uri = 'file:' + original_db_file

        # Make a bunch of files and fill them with data.
        self.populate()

        self.session.checkpoint()

        # Export the metadata for one of the files we made.
        # We just need an example of what a file configuration would typically look like.
        c = self.session.open_cursor('metadata:', None, None)
        example_db_file_config = c['file:test_import02_1']
        c.close()

        self.printVerbose(3, '\nFILE CONFIG\n' + example_db_file_config)

        # Contruct the config string.
        import_config = 'import=(enabled,repair=false,file_metadata=(' + \
            example_db_file_config + '))'

        # Try to import a file that doesn't exist on disk.
        # We should get an error back.
        with self.expectedStderrPattern(
            'file:original_db_file: attempted to import file that does not exist'):
            self.assertRaisesException(wiredtiger.WiredTigerError,
                lambda: self.session.create(uri, import_config))
