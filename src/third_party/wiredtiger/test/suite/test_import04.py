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
# test_import04.py
# Test success and failure scenarios for importing a table into a running database.
# 1. Attempt to import a table into a destination database where a table object of
#    that name already exists.
#    Expected outcome: FAILURE
# 2. Drop a table from a database without removing the data files, then attempt to
#    import that table into the same database.
#    Expected outcome: SUCCESS
# 3. Attempt to import a table into a destination database where the required data
#    files do not exist in the destination database directory.
#    Expected outcome: FAILURE
# 4. Attempt to import a table into a destination database without specifying the
#    exported table configuration.
#    Expected outcome: FAILURE
# 5. Attempt to import a table into a destination database without specifying the
#    exported file configuration.
#    Expected outcome: FAILURE
# 6. Attempt to import a table into a destination database with the exported
#    configuration strings supplied, the required data files are present and the
#    table object does not already exist in the destination database.
#    Expected outcome: SUCCESS

import os, random, shutil
import wiredtiger, wttest
from wtscenario import make_scenarios
from test_import01 import test_import_base

class test_import04(test_import_base):
    conn_config = 'cache_size=50MB,log=(enabled)'

    ntables = 10
    nrows = 100
    scenarios = make_scenarios([
        ('simple_table', dict(
            is_simple = True,
            keys=[k for k in range(1, nrows+1)],
            values=random.sample(range(1000000), k=nrows),
            config='key_format=r,value_format=i')),
        ('table_with_named_columns', dict(
            is_simple = False,
            keys=[k for k in range(1, 7)],
            values=[('Australia', 'Canberra', 1),('Japan', 'Tokyo', 2),('Italy', 'Rome', 3),
              ('China', 'Beijing', 4),('Germany', 'Berlin', 5),('South Korea', 'Seoul', 6)],
            config='columns=(id,country,capital,population),key_format=r,value_format=SSi')),
    ])

    # Test table projections.
    def check_projections(self, uri, keys, values):
        for i in range(0, len(keys)):
            self.check_record(uri + '(country,capital)',
                              keys[i], [values[i][0], values[i][1]])
            self.check_record(uri + '(country,population)',
                              keys[i], [values[i][0], values[i][2]])
            self.check_record(uri + '(capital,population)',
                              keys[i], [values[i][1], values[i][2]])

    def test_table_import(self):
        # Add some data and checkpoint.
        self.populate(self.ntables, self.nrows)
        self.session.checkpoint()

        # Create the target table for import tests.
        original_db_table = 'original_db_table'
        uri = 'table:' + original_db_table
        create_config = 'allocation_size=512,log=(enabled=true),' + self.config
        self.session.create(uri, create_config)

        keys = self.keys
        values = self.values
        ts = [10*k for k in range(1, len(keys)+1)]

        # Add data and perform a checkpoint.
        min_idx = 0
        max_idx = len(keys) // 3
        for i in range(min_idx, max_idx):
            self.update(uri, keys[i], values[i], ts[i])
        self.session.checkpoint()

        # Add more data and checkpoint again.
        min_idx = max_idx
        max_idx = 2*len(keys) // 3
        for i in range(min_idx, max_idx):
            self.update(uri, keys[i], values[i], ts[i])
        self.session.checkpoint()

        # Check the inserted values are in the table.
        self.check(uri, keys[:max_idx], values[:max_idx])

        # Check against projections when the table is not simple.
        if not self.is_simple:
            self.check_projections(uri, keys[:max_idx], values[:max_idx])

        # Export the metadata for the table.
        original_db_file_uri = 'file:' + original_db_table + '.wt'
        c = self.session.open_cursor('metadata:', None, None)
        original_db_table_config = c[uri]
        original_db_file_config = c[original_db_file_uri]
        c.close()

        # Close the connection.
        self.close_conn()

        # Construct the config string from the exported metadata.
        import_config = '{},import=(enabled,file_metadata=({}))'.format(
            original_db_table_config, original_db_file_config)

        # Reopen the connection, add some data and attempt to import the table. We expect
        # this to fail.
        self.conn = self.setUpConnectionOpen('.')
        self.session = self.setUpSessionOpen(self.conn)
        self.populate(self.ntables, self.nrows)
        self.session.checkpoint()

        # Bring forward the oldest to be past or equal to the timestamps we'll be importing.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(ts[max_idx]))

        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: self.session.create(uri, import_config))

        # Drop the table without removing the data files then attempt to import. We expect
        # this operation to succeed.
        self.session.drop(uri, 'remove_files=false')
        # Verify the table is dropped.
        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor(uri, None, None))
        self.session.create(uri, import_config)

        self.close_conn()

        # Create a new database, connect and populate.
        newdir = 'IMPORT_DB'
        shutil.rmtree(newdir, ignore_errors=True)
        os.mkdir(newdir)
        self.conn = self.setUpConnectionOpen(newdir)
        self.session = self.setUpSessionOpen(self.conn)
        self.populate(self.ntables, self.nrows)
        self.session.checkpoint()

        # Bring forward the oldest to be past or equal to the timestamps we'll be importing.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(ts[max_idx]))

        # Attempt to import the table before copying the file. We expect this to fail.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.create(uri, import_config), '/No such file or directory/')

        # Copy over the datafiles for the object we want to import.
        self.copy_file(original_db_table + '.wt', '.', newdir)

        # Construct the config string incorrectly by omitting the table config.
        no_table_config = 'import=(enabled,file_metadata=({}))'.format(original_db_file_config)

        # Attempt to import the table. We expect this to fail.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.create(uri, no_table_config), '/Invalid argument/')

        # Construct the config string incorrectly by omitting the file_metadata and attempt to
        # import the table. We expect this to fail.
        no_file_config = '{},import=(enabled)'.format(original_db_table_config)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.create(uri, no_file_config), '/Invalid argument/')

        # Import the table.
        self.session.create(uri, import_config)

        # Verify object.
        self.session.verify(uri)

        # Check that the previously inserted values survived the import.
        self.check(uri, keys[:max_idx], values[:max_idx])
        if not self.is_simple:
            self.check_projections(uri, keys[:max_idx], values[:max_idx])

        # Compare configuration metadata.
        c = self.session.open_cursor('metadata:', None, None)
        current_db_table_config = c[uri]
        c.close()
        self.config_compare(original_db_table_config, current_db_table_config)

        # Add some data and check that the table operates as usual after importing.
        min_idx = max_idx
        max_idx = len(keys)
        for i in range(min_idx, max_idx):
            self.update(uri, keys[i], values[i], ts[i])
        self.check(uri, keys, values)
        if not self.is_simple:
            self.check_projections(uri, keys, values)

        # Perform a checkpoint.
        self.session.checkpoint()

if __name__ == '__main__':
    wttest.run()
