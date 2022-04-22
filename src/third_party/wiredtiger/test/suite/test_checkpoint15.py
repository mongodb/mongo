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

# test_checkpoint15.py
#
# Make sure each checkpoint has its own timestamp info by writing out
# multiple checkpoints with different times and reading all of them.

class test_checkpoint(wttest.WiredTigerTestCase):
    session_config = 'isolation=snapshot'

    format_values = [
        ('column-fix', dict(key_format='r', value_format='8t',
            extraconfig=',allocation_size=512,leaf_page_max=512')),
        ('column', dict(key_format='r', value_format='S', extraconfig='')),
        ('string_row', dict(key_format='S', value_format='S', extraconfig='')),
    ]
    name_values = [
        ('nnn', dict(first_checkpoint='first_checkpoint',
                    second_checkpoint='second_checkpoint',
                    third_checkpoint='third_checkpoint')),
        ('nnu', dict(first_checkpoint='first_checkpoint',
                    second_checkpoint='second_checkpoint',
                    third_checkpoint=None)),
        # These don't work because there's no way to open an unnamed checkpoint if it's
        # not the most recent checkpoint.
        #('nun', dict(first_checkpoint='first_checkpoint',
        #            second_checkpoint=None,
        #            third_checkpoint='third_checkpoint')),
        #('unn', dict(first_checkpoint=None,
        #            second_checkpoint='second_checkpoint',
        #            third_checkpoint='third_checkpoint')),
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

    def do_checkpoint(self, ckpt_name):
        if ckpt_name is None:
            self.session.checkpoint()
        else:
            self.session.checkpoint('name=' + ckpt_name)

    def check(self, ds, ckpt, nrows, value, ts):
        if ckpt is None:
            ckpt = 'WiredTigerCheckpoint'
        if ts is None:
            tsstr = ''
        else:
            tsstr = ',debug=(checkpoint_read_timestamp=' + self.timestamp_str(ts) + ')'
        cursor = self.session.open_cursor(ds.uri, None, 'checkpoint=' + ckpt + tsstr)
        #self.session.begin_transaction()
        count = 0
        for k, v in cursor:
            self.assertEqual(v, value)
            count += 1
        self.assertEqual(count, nrows)
        #self.session.rollback_transaction()
        cursor.close()

    def checkfail(self, ds, ckpt, ts):
        if ckpt is None:
            ckpt = 'WiredTigerCheckpoint'
        if ts is None:
            tsstr = ''
        else:
            tsstr = ',debug=(checkpoint_read_timestamp=' + self.timestamp_str(ts) + ')'
        def tryit():
            return self.session.open_cursor(ds.uri, None, 'checkpoint=' + ckpt + tsstr)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: tryit(), '/before the checkpoint oldest/')

    def test_checkpoint(self):
        uri = 'table:checkpoint15'
        nrows = 1000

        # Create a table.
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config=self.extraconfig)
        ds.populate()

        if self.value_format == '8t':
            value_a = 97
            value_b = 98
            value_c = 99
            value_d = 100
        else:
            value_a = "aaaaa" * 100
            value_b = "bbbbb" * 100
            value_c = "ccccc" * 100
            value_d = "ddddd" * 100

        # Set oldest and stable to 5.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(5) +
            ',stable_timestamp=' + self.timestamp_str(5))

        # Write some stable baseline data at time 10.
        self.large_updates(uri, ds, nrows, value_a, 10)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()

        # Write some more data at time 20.
        self.large_updates(uri, ds, nrows, value_b, 20)

        # Mark this data stable and checkpoint it.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        self.do_checkpoint(self.first_checkpoint)

        # Rinse and repeat twice more. Move oldest forward too.
        self.large_updates(uri, ds, nrows, value_c, 30)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(15))
        self.do_checkpoint(self.second_checkpoint)

        self.large_updates(uri, ds, nrows, value_d, 40)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(25))
        self.do_checkpoint(self.third_checkpoint)

        # Read the checkpoints.
        self.check(ds, self.first_checkpoint, nrows, value_a, 10)
        self.check(ds, self.first_checkpoint, nrows, value_b, 20)
        self.check(ds, self.first_checkpoint, nrows, value_b, None)

        self.checkfail(ds, self.second_checkpoint, 10)
        self.check(ds, self.second_checkpoint, nrows, value_b, 20)
        self.check(ds, self.second_checkpoint, nrows, value_c, 30)
        self.check(ds, self.second_checkpoint, nrows, value_c, None)

        self.checkfail(ds, self.third_checkpoint, 10)
        self.checkfail(ds, self.third_checkpoint, 20)
        self.check(ds, self.third_checkpoint, nrows, value_c, 30)
        self.check(ds, self.third_checkpoint, nrows, value_d, 40)
        self.check(ds, self.third_checkpoint, nrows, value_d, None)

if __name__ == '__main__':
    wttest.run()
