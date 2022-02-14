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
# test_import01.py
# Import a file into a running database for the following scenarios:
# - The source database and destination database are different.
# - The source database and destination database are the same.

import os, random, re, shutil, string
import wttest

# Shared base class used by import tests.
class test_import_base(wttest.WiredTigerTestCase):

    # Insert or update a key/value at the supplied timestamp.
    def update(self, uri, key, value, ts):
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        if type(value) in [list, tuple]:
            cursor.set_key(key)
            cursor.set_value(*value)
            cursor.insert()
        else:
            cursor[key] = value
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        cursor.close()

    def delete(self, uri, key, ts):
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        cursor.set_key(key)
        self.assertEqual(0, cursor.remove())
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        cursor.close()

    # Verify the specified key/value is visible at the supplied timestamp.
    def check_record(self, uri, key, value):
        cursor = self.session.open_cursor(uri)
        cursor.set_key(key)
        self.assertEqual(0, cursor.search())
        self.assertEqual(value, cursor.get_value())
        cursor.close()

    # Verify a range of records/timestamps.
    def check(self, uri, keys, values):
        for i in range(len(keys)):
            if type(values[i]) in [tuple]:
                self.check_record(uri, keys[i], list(values[i]))
            else:
                self.check_record(uri, keys[i], values[i])

    # We know the ID can be different between configs, so just remove it from comparison.
    # Everything else should be the same.
    def config_compare(self, aconf, bconf):
        a = re.sub('id=\d+,?', '', aconf)
        a = (re.sub('\w+=\(.*?\)+,?', '', a).strip(',').split(',') +
             re.findall('\w+=\(.*?\)+', a))
        b = re.sub('id=\d+,?', '', bconf)
        b = (re.sub('\w+=\(.*?\)+,?', '', b).strip(',').split(',') +
             re.findall('\w+=\(.*?\)+', b))
        self.assertTrue(a.sort() == b.sort())

    # Populate a database with N tables, each having M rows.
    def populate(self, ntables, nrows):
        for table in range(0, ntables):
            uri = 'table:test_import_{}'.format(
                ''.join(random.choice(string.ascii_letters) for i in range(10)))
            self.session.create(uri, 'key_format=i,value_format=S')
            cursor = self.session.open_cursor(uri)
            for key in range(0, nrows):
                cursor[key] = 'value_{}_{}'.format(table, key)
            cursor.close()

    # Copy a file from a source directory to a destination directory.
    def copy_file(self, file_name, src_dir, dest_dir):
        src_path = os.path.join(src_dir, file_name)
        if os.path.isfile(src_path) and "WiredTiger.lock" not in file_name:
            shutil.copy(src_path, dest_dir)

# test_import01
class test_import01(test_import_base):
    conn_config = 'cache_size=50MB'

    original_db_file = 'original_db_file'
    uri = 'file:' + original_db_file

    nrows = 100
    ntables = 10
    keys = [b'1', b'2', b'3', b'4', b'5', b'6']
    values = [b'\x01\x02aaa\x03\x04', b'\x01\x02bbb\x03\x04', b'\x01\x02ccc\x03\x04',
              b'\x01\x02ddd\x03\x04', b'\x01\x02eee\x03\x04', b'\x01\x02fff\x03\x04']
    ts = [10*k for k in range(1, len(keys)+1)]
    create_config = 'allocation_size=512,key_format=u,value_format=u'

    def test_file_import(self):
        self.session.create(self.uri, self.create_config)

        # Add data and perform a checkpoint.
        min_idx = 0
        max_idx = len(self.keys) // 3
        for i in range(min_idx, max_idx):
            self.update(self.uri, self.keys[i], self.values[i], self.ts[i])
        self.session.checkpoint()

        # Add more data and checkpoint again.
        min_idx = max_idx
        max_idx = 2*len(self.keys) // 3
        for i in range(min_idx, max_idx):
            self.update(self.uri, self.keys[i], self.values[i], self.ts[i])
        self.session.checkpoint()

        # Export the metadata for the table.
        c = self.session.open_cursor('metadata:', None, None)
        original_db_file_config = c[self.uri]
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

        # Make a bunch of files and fill them with data.
        self.populate(self.ntables, self.nrows)
        self.session.checkpoint()

        # Bring forward the oldest to be past or equal to the timestamps we'll be importing.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(self.ts[max_idx]))

        # Copy over the datafiles for the object we want to import.
        self.copy_file(self.original_db_file, '.', newdir)

        # Contruct the config string.
        import_config = 'import=(enabled,repair=false,file_metadata=(' + \
            original_db_file_config + '))'

        # Import the file.
        self.session.create(self.uri, import_config)

        # Verify object.
        self.verifyUntilSuccess(self.session, self.uri, None)

        # Check that the previously inserted values survived the import.
        self.check(self.uri, self.keys[:max_idx], self.values[:max_idx])

        # Compare configuration metadata.
        c = self.session.open_cursor('metadata:', None, None)
        current_db_file_config = c[self.uri]
        c.close()
        self.config_compare(original_db_file_config, current_db_file_config)

        # Add some data and check that the table operates as usual after importing.
        min_idx = max_idx
        max_idx = len(self.keys)
        for i in range(min_idx, max_idx):
            self.update(self.uri, self.keys[i], self.values[i], self.ts[i])
        self.check(self.uri, self.keys, self.values)

        # Perform a checkpoint.
        self.session.checkpoint()

    def test_file_import_dropped_file(self):
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

        # Make a copy of the data file that we're about to drop.
        backup_dir = 'BACKUP'
        shutil.rmtree(backup_dir, ignore_errors=True)
        os.mkdir(backup_dir)
        self.copy_file(self.original_db_file, '.', backup_dir)

        # Drop the table.
        # We'll be importing it back into our database shortly.
        self.session.drop(self.uri)

        # Now copy it back to our database directory.
        self.copy_file(self.original_db_file, backup_dir, '.')

        # Contruct the config string.
        import_config = 'import=(enabled,repair=false,file_metadata=(' + \
            original_db_file_config + '))'

        # Import the file.
        self.session.create(self.uri, import_config)

        # Verify object.
        self.verifyUntilSuccess(self.session, self.uri, None)

        # Check that the previously inserted values survived the import.
        self.check(self.uri, self.keys, self.values)

        # Compare configuration metadata.
        c = self.session.open_cursor('metadata:', None, None)
        current_db_file_config = c[self.uri]
        c.close()
        self.config_compare(original_db_file_config, current_db_file_config)

if __name__ == '__main__':
    wttest.run()
