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
# test_import09.py
# Import a table with the repair option (no exported metadata).

import os, random, shutil
from test_import01 import test_import_base
from wtscenario import make_scenarios
import wttest

@wttest.skip_for_hook("tiered", "Fails with tiered storage")
class test_import09(test_import_base):
    nrows = 100
    ntables = 1

    # To test the sodium encryptor, we use secretkey= rather than
    # setting a keyid, because for a "real" (vs. test-only) encryptor,
    # keyids require some kind of key server, and (a) setting one up
    # for testing would be a nuisance and (b) currently the sodium
    # encryptor doesn't support any anyway.
    #
    # It expects secretkey= to provide a hex-encoded 256-bit chacha20 key.
    # This key will serve for testing purposes.
    sodium_testkey = '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef'

    allocsizes = [
        ('512', dict(allocsize='512')),
        ('1024', dict(allocsize='1024')),
        ('2048', dict(allocsize='2048')),
        ('4096', dict(allocsize='4096')),
    ]
    compressors = [
       ('none', dict(compressor='none')),
       ('nop', dict(compressor='nop')),
       ('lz4', dict(compressor='lz4')),
       ('snappy', dict(compressor='snappy')),
       ('zlib', dict(compressor='zlib')),
       ('zstd', dict(compressor='zstd')),
    ]
    encryptors = [
       ('none', dict(encryptor='none', encryptor_args='')),
       ('nop', dict(encryptor='nop', encryptor_args='')),
       ('rotn', dict(encryptor='rotn', encryptor_args='')),
       ('sodium', dict(encryptor='sodium', encryptor_args=',secretkey=' + sodium_testkey)),
    ]
    tables = [
        ('simple_table', dict(
            keys = [k for k in range(1, nrows+1)],
            values = random.sample(range(1000000), k=nrows),
            config = 'key_format=r,value_format=i')),
       ('table_with_named_columns', dict(
           keys = [k for k in range(1, 7)],
           values = [('Australia', 'Canberra', 1),('Japan', 'Tokyo', 2),('Italy', 'Rome', 3),
             ('China', 'Beijing', 4),('Germany', 'Berlin', 5),('South Korea', 'Seoul', 6)],
           config = 'columns=(id,country,capital,population),key_format=r,value_format=SSi')),
    ]
    scenarios = make_scenarios(tables, allocsizes, compressors, encryptors)

    # Load the compressor extension, skip the test if missing.
    def conn_extensions(self, extlist):
        extlist.skip_if_missing = True
        extlist.extension('compressors', self.compressor)
        extlist.extension('encryptors', self.encryptor)

    def conn_config(self):
        return 'cache_size=50MB,encryption=(name={})'.format(self.encryptor + self.encryptor_args)

    def test_import_table_repair(self):
        # Add some tables & data and checkpoint.
        self.populate(self.ntables, self.nrows)
        self.session.checkpoint()

        # Create the table targeted for import.
        original_db_table = 'original_db_table'
        uri = 'table:' + original_db_table
        create_config = ('allocation_size={},block_compressor={},'
            'encryption=(name={}),') + self.config
        self.session.create(uri,
            create_config.format(self.allocsize, self.compressor, self.encryptor))

        keys = self.keys
        values = self.values
        ts = [10*k for k in range(1, len(keys)+1)]

        # Add data to our target table and perform a checkpoint.
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

        # Export the file and table metadata so we can verify our repair later.
        original_db_file_uri = 'file:' + original_db_table + '.wt'
        c = self.session.open_cursor('metadata:', None, None)
        original_db_table_config = c[uri]
        original_db_file_config = c[original_db_file_uri]
        c.close()

        self.printVerbose(3, '\nFile configuration:\n' + original_db_file_config)
        self.printVerbose(3, '\nTable configuration:\n' + original_db_table_config)

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

        # Copy over the datafile for the table we want to import.
        self.copy_file(original_db_table + '.wt', '.', newdir)

        # Construct the config string.
        import_config = 'import=(enabled,repair=true)'

        # Import the file.
        self.session.create(uri, import_config)

        # Verify object.
        self.verifyUntilSuccess(self.session, uri, None)

        # Check that the previously inserted values survived the import.
        self.check(uri, keys[:max_idx], values[:max_idx])

        # Compare configuration metadata.
        c = self.session.open_cursor('metadata:', None, None)
        new_db_file_config = c[original_db_file_uri]
        new_db_table_config = c[uri]
        c.close()
        self.config_compare(original_db_file_config, new_db_file_config)
        self.config_compare(original_db_table_config, new_db_table_config)

        # Add some data and check that the table operates as usual after importing.
        min_idx = max_idx
        max_idx = len(keys)
        for i in range(min_idx, max_idx):
            self.update(uri, keys[i], values[i], ts[i])
        self.check(uri, keys, values)

        # Perform a checkpoint.
        self.session.checkpoint()
