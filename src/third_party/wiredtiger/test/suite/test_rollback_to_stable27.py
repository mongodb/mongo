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

from test_rollback_to_stable01 import test_rollback_to_stable_base
from wiredtiger import stat, Modify, WT_NOTFOUND
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_rollback_to_stable27.py
#
# Test mixing timestamped and non-timestamped updates on the same VLCS RLE cell.
class test_rollback_to_stable27(test_rollback_to_stable_base):
    session_config = 'isolation=snapshot'

    # Run it all on row-store as well as a control group: if something odd arises from the
    # RLE cell handling it won't happen in row-store.
    key_format_values = [
        ('column', dict(key_format='r')),
        ('integer_row', dict(key_format='i')),
    ]

    in_memory_values = [
        ('no_inmem', dict(in_memory=False)),
        ('inmem', dict(in_memory=True))
    ]

    scenarios = make_scenarios(key_format_values, in_memory_values)

    def conn_config(self):
        if self.in_memory:
            return 'in_memory=true'
        else:
            return 'in_memory=false'

    # Evict the page to force reconciliation.
    def evict(self, uri, key, check_value):
        evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        v = evict_cursor[1]
        self.assertEqual(v, check_value)
        self.assertEqual(evict_cursor.reset(), 0)
        self.session.rollback_transaction()
        evict_cursor.close()

    def test_rollback_to_stable(self):
        nrows = 10

        # Create a table without logging.
        uri = "table:rollback_to_stable27"
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format="S", config='log=(enabled=false)')
        ds.populate()

        value_a = "aaaaa" * 10
        value_b = "bbbbb" * 10

        # Pin oldest and stable to timestamp 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        # Write aaaaaa to all the keys at time 20.
        self.large_updates(uri, value_a, ds, nrows, False, 20)

        # Evict the page to force reconciliation.
        self.evict(uri, 1, value_a)

        # Ideally here we'd check to make sure we actually have a single RLE cell, because
        # if not the rest of the work isn't going to do much good. Maybe via stats...?

        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        cursor[7] = value_b
        self.session.commit_transaction()
        cursor.close()

        # Now roll back.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(15))
        self.conn.rollback_to_stable()

        # The only thing we should see (at any time) is value_b at key 7.
        cursor = self.session.open_cursor(uri)
        for ts in [10, 20, 30]:
            self.session.begin_transaction('read_timestamp=' + self.timestamp_str(ts))
            for k, v in cursor:
                self.assertEqual(k, 7)
                self.assertEqual(v, value_b)
            self.session.rollback_transaction()
        cursor.close()

if __name__ == '__main__':
    wttest.run()
