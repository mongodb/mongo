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

import threading, time
import wttest
import wiredtiger
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_checkpoint30.py
#
# Test reading a cursor when the aggregate time window is visible to the snapshot
# but not all deleted keys on-disk version are not visible.
@wttest.skip_for_hook("tiered", "Fails with tiered storage")
class test_checkpoint(wttest.WiredTigerTestCase):
    format_values = [
        ('column', dict(key_format='r', value_format='S', extraconfig='')),
        ('string_row', dict(key_format='S', value_format='S', extraconfig='')),
    ]
    ckpt_precision = [
        ('fuzzy', dict(ckpt_config='precise_checkpoint=false')),
        ('precise', dict(ckpt_config='precise_checkpoint=true')),
    ]
    scenarios = make_scenarios(format_values, ckpt_precision)

    def conn_config(self):
        return 'cache_size=50MB,statistics=(all),' + self.ckpt_config

    def large_updates(self, uri, ds, nrows, value, ts):
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            cursor[ds.key(i)] = value
            if i % 101 == 0:
                self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
                self.session.begin_transaction()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        cursor.close()

    def large_removes(self, uri, ds, start_row, nrows, ts):
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(start_row, nrows + 1):
            cursor.set_key(ds.key(i))
            cursor.remove()
            if i % 101 == 0:
                self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
                self.session.begin_transaction()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        cursor.close()

    def evict(self, ds, lo, hi, value, ts):
        evict_cursor = self.session.open_cursor(ds.uri, None, "debug=(release_evict)")
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(ts))
        # Evict every 10th key. FUTURE: when that's possible, evict each page exactly once.
        for k in range(lo, hi, 10):
            v = evict_cursor[ds.key(k)]
            self.assertEqual(v, value)
            self.assertEqual(evict_cursor.reset(), 0)
        self.session.rollback_transaction()

    def check(self, session_local, ds, nrows, value, ts):
        cursor = session_local.open_cursor(ds.uri)
        count = 0
        for k, v in cursor:
            self.assertEqual(v, value)
            count += 1
        self.assertEqual(count, nrows)
        cursor.close()

    def test_checkpoint(self):
        uri = 'table:checkpoint30'
        nrows = 100

        # Create a table.
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config=self.extraconfig)
        ds.populate()

        value_a = "aaaaa" * 100
        value_b = "bbbbb" * 100

        # Pin oldest and stable timestamps to 5.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(5) +
            ',stable_timestamp=' + self.timestamp_str(5))

        # Write some data at time 10.
        self.large_updates(uri, ds, nrows, value_a, 10)
        self.check(self.session, ds, nrows, value_a, 10)

        # Remove the key-1 at time 20 and keep this transaction open until a reader is started.
        session2 = self.conn.open_session()
        cursor = session2.open_cursor(uri)
        session2.begin_transaction()
        cursor.set_key(ds.key(1))
        cursor.remove()

        # Remove the remaining keys with another transaction and commit.
        self.large_removes(uri, ds, 2, nrows, 20)
        self.check(self.session, ds, 1, value_a, 20)

        # Start the reader transaction. According to the snapshot visibility, except key-1
        # rest of the keys are removed ignoring the read timestamp.
        session3 = self.conn.open_session()
        session3.begin_transaction()

        # Commit the key-1 remove transaction.
        session2.commit_transaction('commit_timestamp=' + self.timestamp_str(25))
        cursor.close()

        # Reader transaction cannot see the remove on key-1.
        self.check(session3, ds, 1, value_a, 25)

        # Evict the data.
        self.evict(ds, 1, nrows + 1, value_a, 10)

        # Reader transaction cannot see the remove on key-1 after eviction.
        self.check(session3, ds, 1, value_a, 25)

        # Checkpoint.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(25))
        self.session.checkpoint()

        # Reader transaction cannot see the remove on key-1 after the checkpoint.
        self.check(session3, ds, 1, value_a, 25)
