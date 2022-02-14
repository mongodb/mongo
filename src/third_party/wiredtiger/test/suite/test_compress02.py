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

from helper import copy_wiredtiger_home
import wttest
from wtdataset import SimpleDataSet

# test_compress02.py
#   This test checks that the compression level can be reconfigured after restart if
#   we are using zstd as the block compressor. Tables created before reconfiguration
#   will still use the previous compression level.
#
class test_compress02(wttest.WiredTigerTestCase):
    # Create a table.
    uri = "table:test_compress02"
    nrows = 1000

    def conn_config(self):
        config = 'builtin_extension_config={zstd={compression_level=6}},cache_size=10MB'
        return config

    def large_updates(self, uri, value, ds, nrows):
        # Update a large number of records.
        session = self.session
        cursor = session.open_cursor(uri)
        for i in range(0, nrows):
            session.begin_transaction()
            cursor[ds.key(i)] = value
            session.commit_transaction()
        cursor.close()

    def check(self, check_value, uri, nrows):
        session = self.session
        session.begin_transaction()
        cursor = session.open_cursor(uri)
        count = 0
        for k, v in cursor:
            self.assertEqual(v, check_value)
            count += 1
        session.commit_transaction()
        self.assertEqual(count, nrows)

    # Load the compression extension, skip the test if missing
    def conn_extensions(self, extlist):
        extlist.skip_if_missing = True
        extlist.extension('compressors', 'zstd')

    @wttest.zstdtest('Skip zstd on pcc and zseries machines')
    def test_compress02(self):
        ds = SimpleDataSet(self, self.uri, 0, key_format="S", value_format="S",config='block_compressor=zstd')
        ds.populate()
        valuea = "aaaaa" * 100

        cursor = self.session.open_cursor(self.uri)
        self.large_updates(self.uri, valuea, ds, self.nrows)

        self.check(valuea, self.uri, self.nrows)
        self.session.checkpoint()

        #Simulate a crash by copying to a new directory(RESTART).
        copy_wiredtiger_home(self, ".", "RESTART")

        # Close the connection and reopen it with a different zstd compression level configuration.
        restart_config = 'builtin_extension_config={zstd={compression_level=9}},cache_size=10MB'
        self.close_conn()
        self.reopen_conn("RESTART", restart_config)

        # Open the new directory.
        self.session = self.setUpSessionOpen(self.conn)

        # Check the table contains the last checkpointed value.
        self.check(valuea, self.uri, self.nrows)

if __name__ == '__main__':
    wttest.run()
