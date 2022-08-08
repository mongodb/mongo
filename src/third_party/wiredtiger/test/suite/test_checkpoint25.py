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
from wiredtiger import stat
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_checkpoint25.py
#
# Test reading a checkpoint that contains fast-delete pages.
# This version uses timestamps.

class test_checkpoint(wttest.WiredTigerTestCase):
    conn_config = 'statistics=(all)'

    format_values = [
        ('string_row', dict(key_format='S', value_format='S', extraconfig='')),
        ('column-fix', dict(key_format='r', value_format='8t',
            extraconfig=',allocation_size=512,leaf_page_max=512')),
        ('column', dict(key_format='r', value_format='S', extraconfig='')),
    ]
    name_values = [
        # Reopening and unnamed checkpoints will not work as intended because the reopen makes
        # a new checkpoint.
        ('named', dict(first_checkpoint='first_checkpoint', do_reopen=False)),
        ('named_reopen', dict(first_checkpoint='first_checkpoint', do_reopen=True)),
        ('unnamed', dict(first_checkpoint=None, do_reopen=False)),
    ]
    advance_oldest_values = [
        ('no_advance_oldest', dict(advance_oldest=False)),
        ('advance_oldest', dict(advance_oldest=True)),
    ]
    advance_stable_values = [
        ('no_advance_stable', dict(advance_stable=False)),
        ('advance_stable', dict(advance_stable=True)),
    ]
    scenarios = make_scenarios(
        format_values, name_values, advance_oldest_values, advance_stable_values)
        

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

    def do_truncate(self, ds, lo, hi, read_ts, commit_ts):
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(read_ts))
        locursor = self.session.open_cursor(ds.uri)
        hicursor = self.session.open_cursor(ds.uri)
        locursor.set_key(ds.key(lo))
        hicursor.set_key(ds.key(hi))
        self.session.truncate(None, locursor, hicursor, None)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))

    def do_checkpoint(self, ckpt_name):
        if ckpt_name is None:
            self.session.checkpoint()
        else:
            self.session.checkpoint('name=' + ckpt_name)

    def check(self, ds, ckpt, nrows, zeros, value, ts):
        if ckpt is None:
            ckpt = 'WiredTigerCheckpoint'
        cfg = 'checkpoint=' + ckpt
        if ts is not None:
            cfg += ',debug=(checkpoint_read_timestamp=' + self.timestamp_str(ts) + ')'
        cursor = self.session.open_cursor(ds.uri, None, cfg)
        count = 0
        zcount = 0
        for k, v in cursor:
            if self.value_format == '8t' and v == 0:
                zcount += 1
            else:
                self.assertEqual(v, value)
                count += 1
        self.assertEqual(count, nrows)
        self.assertEqual(zcount, zeros if self.value_format == '8t' else 0)
        cursor.close()

    def test_checkpoint(self):
        uri = 'table:checkpoint25'
        nrows = 10000

        # Create a table.
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config=self.extraconfig)
        ds.populate()

        if self.value_format == '8t':
            value_a = 97
        else:
            value_a = "aaaaa" * 100

        # Pin oldest and stable timestamps to 5.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(5) +
            ',stable_timestamp=' + self.timestamp_str(5))

        # Write some data at time 10.
        self.large_updates(uri, ds, nrows, value_a, 10)

        # Mark it stable.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))

        # Reopen the connection (which checkpoints it) so it's all on disk and not in memory.
        self.reopen_conn()

        # Truncate half of it at time 20.
        self.do_truncate(ds, nrows // 4 + 1, nrows // 4 + nrows // 2, 10, 20)

        # Mark that stable so it appears in the checkpoint.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))

        # Check stats to make sure we fast-deleted at least one page.
        # (Except for FLCS, where it's not supported and we should fast-delete zero pages.)
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        fastdelete_pages = stat_cursor[stat.conn.rec_page_delete_fast][2]
        if self.value_format == '8t':
            self.assertEqual(fastdelete_pages, 0)
        else:
            self.assertGreater(fastdelete_pages, 0)

        # Take a checkpoint.
        self.do_checkpoint(self.first_checkpoint)

        if self.advance_oldest:
            self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(20))
        if self.advance_stable:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))
        if self.do_reopen:
            self.reopen_conn()

        # Read the checkpoint.
        nonzeros = nrows // 2
        zeros = nrows - nonzeros
        self.check(ds, self.first_checkpoint, nrows, 0, value_a, 15)
        self.check(ds, self.first_checkpoint, nonzeros, zeros, value_a, 25)
        self.check(ds, self.first_checkpoint, nonzeros, zeros, value_a, None) # default read ts
        self.check(ds, self.first_checkpoint, nonzeros, zeros, value_a, 0) # no read ts

if __name__ == '__main__':
    wttest.run()
