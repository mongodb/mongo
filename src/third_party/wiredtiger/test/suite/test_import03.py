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
# test_import03.py
# Import a table into a running database.

import os, random, shutil
from wtscenario import make_scenarios
from test_import01 import test_import_base

class test_import03(test_import_base):
    conn_config = 'cache_size=50MB'

    ntables = 10
    nrows = 100
    scenarios = make_scenarios([
        ('simple_table', dict(
            is_simple = True,
            keys = [k for k in range(1, nrows+1)],
            values = random.sample(range(1000000), k=nrows),
            config = 'key_format=r,value_format=i')),
        ('table_with_named_columns', dict(
            is_simple = False,
            keys = [k for k in range(1, 7)],
            values = [('Australia', 'Canberra', 1),('Japan', 'Tokyo', 2),('Italy', 'Rome', 3),
              ('China', 'Beijing', 4),('Germany', 'Berlin', 5),('South Korea', 'Seoul', 6)],
            config = 'columns=(id,country,capital,population),key_format=r,value_format=SSi')),
    ])

    # Test something table specific like a projection.
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

        original_db_table = 'original_db_table'
        uri = 'table:' + original_db_table
        create_config = 'allocation_size=512,' + self.config
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

        # Export the metadata for the table.
        original_db_file_uri = 'file:' + original_db_table + '.wt'
        c = self.session.open_cursor('metadata:', None, None)
        original_db_table_config = c[uri]
        original_db_file_config = c[original_db_file_uri]
        c.close()

        self.printVerbose(3, '\nFile configuration:\n' + original_db_file_config)
        self.printVerbose(3, '\nTable configuration:\n' + original_db_table_config)

        # Contruct the config string.
        import_config = '{},import=(enabled,repair=false,file_metadata=({}))'.format(
            original_db_table_config, original_db_file_config)

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
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(ts[max_idx]))

        # Copy over the datafiles for the object we want to import.
        self.copy_file(original_db_table + '.wt', '.', newdir)

        # Import the table.
        self.session.create(uri, import_config)

        # Verify object.
        self.session.verify(uri)

        # Check that the previously inserted values survived the import.
        self.check(uri, keys[:max_idx], values[:max_idx])

        # Check against projections when the table is not simple.
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
