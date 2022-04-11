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

# test_checkpoint20.py
#
# Test reading a checkpoint that contains prepared data.

class test_checkpoint(wttest.WiredTigerTestCase):

    format_values = [
        ('column-fix', dict(key_format='r', value_format='8t',
            extraconfig=',allocation_size=512,leaf_page_max=512')),
        ('column', dict(key_format='r', value_format='S', extraconfig='')),
        ('string_row', dict(key_format='S', value_format='S', extraconfig='')),
    ]
    stable_ts_values = [
        ('15', dict(stable_ts=15)),
        ('25', dict(stable_ts=25)),
    ]
    name_values = [
        ('named', dict(first_checkpoint='first_checkpoint')),
        ('unnamed', dict(first_checkpoint=None)),
    ]
    scenarios = make_scenarios(format_values, stable_ts_values, name_values)
        

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

    def evict(self, ds, lo, hi, value, ts):
        evict_cursor = self.session.open_cursor(ds.uri, None, "debug=(release_evict)")
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(ts))
        # Evict every 10th key. FUTURE: when that's possible, evict each page exactly once.
        for k in range(lo, hi, 10):
            v = evict_cursor[ds.key(k)]
            self.assertEqual(v, value)
            self.assertEqual(evict_cursor.reset(), 0)
        self.session.rollback_transaction()

    def do_checkpoint(self, ckpt_name):
        if ckpt_name is None:
            self.session.checkpoint()
        else:
            self.session.checkpoint('name=' + ckpt_name)

    def check(self, ds, ckpt, nrows, value, ts):
        if ckpt is None:
            ckpt = 'WiredTigerCheckpoint'
        cfg = 'checkpoint=' + ckpt
        if ts is not None:
            cfg += ',debug=(checkpoint_read_timestamp=' + self.timestamp_str(ts) + ')'
        cursor = self.session.open_cursor(ds.uri, None, cfg)
        #self.session.begin_transaction()
        count = 0
        for k, v in cursor:
            self.assertEqual(v, value)
            count += 1
        #self.session.rollback_transaction()
        self.assertEqual(count, nrows)
        cursor.close()

    def checkfail(self, ds, ckpt, key, ts):
        if ckpt is None:
            ckpt = 'WiredTigerCheckpoint'
        cfg = 'checkpoint=' + ckpt
        if ts is not None:
            cfg += ',debug=(checkpoint_read_timestamp=' + self.timestamp_str(ts) + ')'
        cursor = self.session.open_cursor(ds.uri, None, cfg)
        #self.session.begin_transaction()
        cursor.set_key(ds.key(key))
        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: cursor.search(),
            '/conflict with a prepared/')
        #self.session.rollback_transaction()
        cursor.close()

    def test_checkpoint(self):
        uri = 'table:checkpoint20'
        nrows = 10000

        # Create a table.
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config=self.extraconfig)
        ds.populate()

        if self.value_format == '8t':
            value_a = 97
            value_b = 98
            value_c = 99
        else:
            value_a = "aaaaa" * 100
            value_b = "bbbbb" * 100
            value_c = "ccccc" * 100

        # Pin oldest and stable timestamps to 5.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(5) +
            ',stable_timestamp=' + self.timestamp_str(5))

        # Write some data at time 10.
        self.large_updates(uri, ds, nrows, value_a, 10)

        # Prepare some more data at time 20.
        session2 = self.conn.open_session()
        cursor2 = session2.open_cursor(uri)
        session2.begin_transaction()
        for i in range(nrows // 2 + 1, nrows):
            cursor2[ds.key(i)] = value_b
        session2.prepare_transaction('prepare_timestamp=' + self.timestamp_str(20))

        # Evict the lot. Otherwise the checkpoint won't write the prepared data.
        # Read at 10 to do the eviction to avoid tripping on the prepared transaction.
        self.evict(ds, 1, nrows + 1, value_a, 10)

        # Checkpoint.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(self.stable_ts))
        self.do_checkpoint(self.first_checkpoint)

        # Commit the prepared transaction so it isn't in the way.
        session2.timestamp_transaction('commit_timestamp=' + self.timestamp_str(20))
        session2.commit_transaction('durable_timestamp=' + self.timestamp_str(30))

        # Read the checkpoint.
        # We decided that checkpoint cursors should always use ignore_prepare, so we
        # should always see value_a.
        self.check(ds, self.first_checkpoint, nrows, value_a, 10)
        self.check(ds, self.first_checkpoint, nrows, value_a, 20)
        self.check(ds, self.first_checkpoint, nrows, value_a, None)

        # Without ignore_prepare, we'd want to check that one of the prepared keys fails.
        #self.checkfail(ds, self.first_checkpoint, nrows // 2 + 1, 20)
        #if self.stable_ts >= 20:
        #    self.checkfail(ds, self.first_checkpoint, nrows // 2 + 1, None)

if __name__ == '__main__':
    wttest.run()
