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

from wtdataset import SimpleDataSet
from wtscenario import make_scenarios
from rollback_to_stable_util import test_rollback_to_stable_base

# test_rollback_to_stable46.py
# Test that rollback to stable removes all keys when the stable timestamp is earlier than
# all commit timestamps. This test specifically tests the case where all updates in the page are saved to disk via reconciliation
# and the page is marked as clean, then RTS is performed.
# It checks that RTS should not skip any pages with unstable on-disk updates and correctly roll back all of them.
class test_rollback_to_stable46(test_rollback_to_stable_base):

    format_values = [
        ('column', dict(key_format='i', value_format='S')),
        ('row_integer', dict(key_format='i', value_format='S')),
    ]

    value_format='S'

    in_memory_values = [
        ('no_inmem', dict(in_memory=False)),
        ('inmem', dict(in_memory=True))
    ]

    worker_thread_values = [
        ('0', dict(threads=0)),
        ('4', dict(threads=4)),
        ('8', dict(threads=8))
    ]

    scenarios = make_scenarios(format_values, in_memory_values, worker_thread_values)
    def conn_config(self):
        config = 'cache_size=50MB,statistics=(all)'
        if self.in_memory:
            config += ',in_memory=true'
        return config

    def test_rollback_to_stable(self):
        # Create a table.
        uri = "table:rollback_to_stable46"
        ds_config = ',log=(enabled=false)' if self.in_memory else ''
        ds = SimpleDataSet(self, uri, 0,
            key_format=self.key_format, value_format=self.value_format, config=ds_config)
        ds.populate()
        nrows = 5000

        value_a = "aaaaa" * 100
        value_b = "bbbbb" * 100
        # Pin oldest and stable to timestamp 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        # Insert 5000 record at time 20
        session = self.session
        cursor = session.open_cursor(uri)
        session.begin_transaction()

        for i in range(1,nrows+1):
            cursor[ds.key(i)] = value_a

        session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))
        cursor.close()

        # Call eviction to trigger reconciliation. These updates should be written to the disk image.
        self.evict_cursor(uri, nrows, value_a)

        # Insert another 2000 records in the next page(s)
        cursor = session.open_cursor(uri)
        session.begin_transaction()
        for i in range(nrows+1, nrows+2001):
            cursor[ds.key(i)] = ds.value(value_b)

        session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))
        cursor.close()

        # Verify data is visible and correct.
        self.check(value_a, uri, nrows, 20)

        session.begin_transaction('read_timestamp=' + self.timestamp_str(30))
        cursor = session.open_cursor(uri)
        for i in range(nrows+1, nrows+2001):
            cursor.set_key(ds.key(i))
            self.assertEqual(cursor.search(), 0)
            self.assertEqual(cursor.get_value(), ds.value(value_b))
        session.commit_transaction()

        cursor.close()
        # Do a checkpoint
        if not self.in_memory:
            self.session.checkpoint()

        # Rollback to stable timestamp 10, everything should be deleted
        self.conn.rollback_to_stable('threads=' + str(self.threads))

        # Verify all data is invisible.
        self.check(value_a, uri, 0, 20)
        self.check(value_b, uri, 0, 30)

