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

from wiredtiger import stat
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios
from helper import simulate_crash_restart
from test_rollback_to_stable01 import test_rollback_to_stable_base

# test_rollback_to_stable29.py
# Test that the rollback to stable to verify the history store order when an out of order to a tombstone.
class test_rollback_to_stable29(test_rollback_to_stable_base):
    conn_config = 'cache_size=25MB,statistics=(all),statistics_log=(json,on_close,wait=1)'

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('row_integer', dict(key_format='i', value_format='S')),
    ]

    scenarios = make_scenarios(format_values)

    def test_rollback_to_stable(self):
        uri = 'table:test_rollback_to_stable29'
        nrows = 100

        if self.value_format == '8t':
            value_a = 97
            value_b = 98
            value_c = 99
            value_d = 100
        else:
            value_a = 'a' * 100
            value_b = 'b' * 100
            value_c = 'c' * 100
            value_d = 'd' * 100

        # Create our table.
        ds = SimpleDataSet(self, uri, 0, key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        # Pin oldest and stable to timestamp 1.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        self.large_updates(uri, value_a, ds, nrows, False, 10)
        self.large_removes(uri, ds, nrows, False, 30)
        self.large_updates(uri, value_b, ds, nrows, False, 40)
        self.check(value_b, uri, nrows, None, 40)
        self.large_updates(uri, value_c, ds, nrows, False, 50)
        self.check(value_c, uri, nrows, None, 50)
        self.evict_cursor(uri, nrows, value_c)

        # Insert an out of order update.
        self.session.breakpoint()
        self.large_updates(uri, value_d, ds, nrows, False, 20)

        self.check(value_a, uri, nrows, None, 10)
        self.check(value_d, uri, nrows, None, 40)
        self.check(value_d, uri, nrows, None, 50)
        self.check(value_d, uri, nrows, None, 20)

        # Pin stable to timestamp 10.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()

        # Simulate a crash by copying to a new directory(RESTART).
        simulate_crash_restart(self, ".", "RESTART")

        self.check(value_a, uri, nrows, None, 10)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        hs_removed = stat_cursor[stat.conn.txn_rts_hs_removed][2]
        stat_cursor.close()

        self.assertGreaterEqual(hs_removed, 3 * nrows)

if __name__ == '__main__':
    wttest.run()
