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
import wiredtiger, wttest
from wiredtiger import stat
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_hs03.py
# Ensure checkpoints don't read too unnecessary history store entries.
class test_hs03(wttest.WiredTigerTestCase):
    # Force a small cache.
    conn_config = 'cache_size=50MB,statistics=(fast)'
    session_config = 'isolation=snapshot'
    format_values = [
        ('column', dict(key_format='r', value_format='u')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('integer-row', dict(key_format='i', value_format='u')),
        ('string-row', dict(key_format='S', value_format='u'))
    ]
    scenarios = make_scenarios(format_values)

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def large_updates(self, session, uri, value, ds, nrows, nops):
        # Update a large number of records, we'll hang if the history store table
        # isn't doing its thing.
        cursor = session.open_cursor(uri)
        for i in range(nrows + 1, nrows + nops + 1):
            session.begin_transaction()
            cursor[ds.key(i)] = value
            session.commit_transaction('commit_timestamp=' + self.timestamp_str(i))
        cursor.close()

    def test_checkpoint_hs_reads(self):
        # Create a small table.
        uri = "table:test_hs03"
        nrows = 100
        ds = SimpleDataSet(self, uri, nrows, key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        if self.value_format == '8t':
            bigvalue = 97
            bigvalue2 = 100
        else:
            bigvalue = b"aaaaa" * 100
            bigvalue2 = b"ddddd" * 100

        # Initially load huge data.
        cursor = self.session.open_cursor(uri)
        for i in range(1, 10000):
            cursor[ds.key(nrows + i)] = bigvalue
        cursor.close()
        self.session.checkpoint()

        # Check to see the history store working with old timestamp.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        hs_writes_start = self.get_stat(stat.conn.cache_write_hs)
        self.large_updates(self.session, uri, bigvalue2, ds, nrows, 10000)

        # If the test sizing is correct, the history will overflow the cache.
        self.session.checkpoint()
        hs_writes = self.get_stat(stat.conn.cache_write_hs) - hs_writes_start
        self.assertGreaterEqual(hs_writes, 0)

        for ts in range(2, 4):
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(ts))

            # Now just update one record and checkpoint again.
            self.large_updates(self.session, uri, bigvalue2, ds, nrows, 1)

            hs_reads_start = self.get_stat(stat.conn.cache_hs_read)
            self.session.checkpoint()
            hs_reads = self.get_stat(stat.conn.cache_hs_read) - hs_reads_start

            # Since we're dealing with eviction concurrent with checkpoints
            # and skewing is controlled by a heuristic, we can't put too tight
            # a bound on this.
            self.assertLessEqual(hs_reads, 200)

if __name__ == '__main__':
    wttest.run()
