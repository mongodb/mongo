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

from test_gc01 import test_gc_base
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_gc05.py
# Verify a locked checkpoint is not removed during garbage collection.

class test_gc05(test_gc_base):
    conn_config = 'cache_size=50MB,statistics=(all)'

    format_values = [
        ('column', dict(key_format='r', value_format='S', extraconfig='')),
        ('column_fix', dict(key_format='r', value_format='8t',
            extraconfig=',allocation_size=512,leaf_page_max=512')),
        ('integer_row', dict(key_format='i', value_format='S', extraconfig='')),
    ]
    named_values = [
        ('named', dict(named=True)),
        ('anonymous', dict(named=False)),
    ]
    scenarios = make_scenarios(format_values, named_values)

    def test_gc(self):
        uri = "table:gc05"
        create_params = 'value_format=S,key_format=i'
        self.session.create(uri, create_params)

        nrows = 10000
        value_u = "uuuuu" * 100
        value_v = "vvvvv" * 100
        value_w = "wwwww" * 100
        value_x = "xxxxx" * 100
        value_y = "yyyyy" * 100
        value_z = "zzzzz" * 100
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config=self.extraconfig)
        ds.populate()

        # Set the oldest and stable timestamps to 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        # Insert values with varying timestamps.
        self.large_updates(uri, value_x, ds, nrows, 20)
        self.large_updates(uri, value_y, ds, nrows, 30)
        self.large_updates(uri, value_z, ds, nrows, 40)

        # Move stable to 35 so there's something to checkpoint.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(35))

        # Perform a checkpoint.
        if self.named:
            self.session.checkpoint("name=checkpoint_one")
        else:
            self.session.checkpoint()

        # Check statistics.
        self.check_gc_stats()

        # Open a cursor to the checkpoint just performed.
        if self.named:
            ckpt_cursor = self.session.open_cursor(uri, None, "checkpoint=checkpoint_one")
        else:
            ckpt_cursor = self.session.open_cursor(uri, None, "checkpoint=WiredTigerCheckpoint")

        # Move the oldest and stable timestamps to 40.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(40) +
            ',stable_timestamp=' + self.timestamp_str(40))

        # Insert values with varying timestamps.
        self.large_updates(uri, value_u, ds, nrows, 50)
        self.large_updates(uri, value_v, ds, nrows, 60)
        self.large_updates(uri, value_w, ds, nrows, 70)

        # Move the oldest and stable timestamps to 70.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(70) +
            ',stable_timestamp=' + self.timestamp_str(70))

        # Perform a checkpoint.
        self.session.checkpoint()
        self.check_gc_stats()

        # Verify the open checkpoint still exists and contains the expected values.
        for i in range(0, nrows):
            ckpt_cursor.set_key(i)
            self.assertEqual(ckpt_cursor.search(), 0)
            self.assertEqual(ckpt_cursor.get_value(), value_y)

        # Close checkpoint cursor.
        ckpt_cursor.close()

        if self.named:
            # If we named the checkpoint, it should still exist and still have the same values.
            ckpt_cursor = self.session.open_cursor(uri, None, "checkpoint=checkpoint_one")
            for i in range(0, nrows):
                ckpt_cursor.set_key(i)
                self.assertEqual(ckpt_cursor.search(), 0)
                self.assertEqual(ckpt_cursor.get_value(), value_y)
        else:
            # If we didn't, reopening should get the most recent checkpoint.
            ckpt_cursor = self.session.open_cursor(uri, None, "checkpoint=WiredTigerCheckpoint")
            for i in range(0, nrows):
                ckpt_cursor.set_key(i)
                self.assertEqual(ckpt_cursor.search(), 0)
                self.assertEqual(ckpt_cursor.get_value(), value_w)


if __name__ == '__main__':
    wttest.run()
