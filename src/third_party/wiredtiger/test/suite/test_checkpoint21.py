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

# test_checkpoint21.py
#
# Test reading a checkpoint that contains data from a committed but not
# durable transaction.
#
# Because transactions are allowed to commit before stable if they prepared
# after stable but stable then advanced, but their durable timestamp is required
# to be after stable as of commit, the commit time can be before the durable
# time. If a checkpoint occurs in this window, there is a bit of a dilemma: from
# one point of view, the data from these transactions should *not* be visible
# because that's what happens if we crash and recover to the same checkpoint.
# From another, it *should* because otherwise (and if we don't crash), reading
# from the checkpoint and the live database at the same read timestamp will
# produce different results.
#
# It is not clear what the proper solution is; for the time being the expedient
# approach is to allow the data to appear. As far as I know the entirety of such
# transactions should always be written out by the checkpoint (because they're
# committed) -- we will therefore never see a torn transaction, which is the
# most important consideration.
#
# This test sets up such a transaction, evicts half of it, then checkpoints the
# rest, and checks that it is all visible by reading the checkpoint.

class test_checkpoint(wttest.WiredTigerTestCase):

    format_values = [
        ('column-fix', dict(key_format='r', value_format='8t',
            extraconfig=',allocation_size=512,leaf_page_max=512')),
        ('column', dict(key_format='r', value_format='S', extraconfig='')),
        ('string_row', dict(key_format='S', value_format='S', extraconfig='')),
    ]
    name_values = [
        ('named', dict(first_checkpoint='first_checkpoint')),
        ('unnamed', dict(first_checkpoint=None)),
    ]
    scenarios = make_scenarios(format_values, name_values)
        

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
        uri = 'table:checkpoint21'
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
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            cursor[ds.key(i)] = value_b
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(20))

        # Move stable up to 30.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))

        # Commit the transaction at 25 but make it durable at 35.
        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(25))
        self.session.commit_transaction('durable_timestamp=' + self.timestamp_str(35))

        # Evict half the pages to make sure they get written with the updated values.
        self.evict(ds, 1, nrows // 2 + 1, value_b, 25)

        # Checkpoint the rest while stable is still 30.
        self.do_checkpoint(self.first_checkpoint)

        # Read the checkpoint.
        self.check(ds, self.first_checkpoint, nrows, value_a, 15)
        self.check(ds, self.first_checkpoint, nrows, value_b, 25)
        self.check(ds, self.first_checkpoint, nrows, value_b, None) # default read ts
        self.check(ds, self.first_checkpoint, nrows, value_b, 0) # no read ts

if __name__ == '__main__':
    wttest.run()
