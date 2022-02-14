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
from test_rollback_to_stable01 import test_rollback_to_stable_base

def mod_val(value, char, location, nbytes=1):
    return value[0:location] + char + value[location+nbytes:]

# test_rollback_to_stable04.py
# Test that rollback to stable always replaces the on-disk value with a full update
# from the history store.
class test_rollback_to_stable04(test_rollback_to_stable_base):

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('row_integer', dict(key_format='i', value_format='S')),
    ]

    in_memory_values = [
        ('no_inmem', dict(in_memory=False)),
        ('inmem', dict(in_memory=True))
    ]

    prepare_values = [
        ('no_prepare', dict(prepare=False)),
        ('prepare', dict(prepare=True))
    ]

    scenarios = make_scenarios(format_values, in_memory_values, prepare_values)

    def conn_config(self):
        config = 'cache_size=500MB,statistics=(all)'
        if self.in_memory:
            config += ',in_memory=true'
        return config

    def test_rollback_to_stable(self):
        nrows = 1000

        # Create a table.
        uri = "table:rollback_to_stable04"
        ds = SimpleDataSet(self, uri, 0, key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        if self.value_format == '8t':
            value_a = 97 # 'a'
            value_b = 98 # 'b'
            value_c = 99 # 'c'
            value_d = 100 # 'd'

            # No modifies in FLCS; do ordinary updates instead.
            value_modQ = 81 # 'Q'
            value_modR = 82 # 'R'
            value_modS = 83 # 'S'
            value_modT = 84 # 'T'
            value_modW = 87 # 'W'
            value_modX = 88 # 'X'
            value_modY = 89 # 'Y'
            value_modZ = 90 # 'Z'
        else:
            value_a = "aaaaa" * 100
            value_b = "bbbbb" * 100
            value_c = "ccccc" * 100
            value_d = "ddddd" * 100

            value_modQ = mod_val(value_a, 'Q', 0)
            value_modR = mod_val(value_modQ, 'R', 1)
            value_modS = mod_val(value_modR, 'S', 2)
            value_modT = mod_val(value_c, 'T', 3)
            value_modW = mod_val(value_d, 'W', 4)
            value_modX = mod_val(value_a, 'X', 5)
            value_modY = mod_val(value_modX, 'Y', 6)
            value_modZ = mod_val(value_modY, 'Z', 7)

        # Pin oldest and stable to timestamp 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        # Perform a combination of modifies and updates.
        self.large_updates(uri, value_a, ds, nrows, self.prepare, 20)
        self.large_modifies(uri, 'Q', ds, 0, 1, nrows, self.prepare, 30)
        self.large_modifies(uri, 'R', ds, 1, 1, nrows, self.prepare, 40)
        self.large_modifies(uri, 'S', ds, 2, 1, nrows, self.prepare, 50)
        self.large_updates(uri, value_b, ds, nrows, self.prepare, 60)
        self.large_updates(uri, value_c, ds, nrows, self.prepare, 70)
        self.large_modifies(uri, 'T', ds, 3, 1, nrows, self.prepare, 80)
        self.large_updates(uri, value_d, ds, nrows, self.prepare, 90)
        self.large_modifies(uri, 'W', ds, 4, 1, nrows, self.prepare, 100)
        self.large_updates(uri, value_a, ds, nrows, self.prepare, 110)
        self.large_modifies(uri, 'X', ds, 5, 1, nrows, self.prepare, 120)
        self.large_modifies(uri, 'Y', ds, 6, 1, nrows, self.prepare, 130)
        self.large_modifies(uri, 'Z', ds, 7, 1, nrows, self.prepare, 140)

        # Verify data is visible and correct.
        self.check(value_a, uri, nrows, None, 20)
        self.check(value_modQ, uri, nrows, None, 30)
        self.check(value_modR, uri, nrows, None, 40)
        self.check(value_modS, uri, nrows, None, 50)
        self.check(value_b, uri, nrows, None, 60)
        self.check(value_c, uri, nrows, None, 70)
        self.check(value_modT, uri, nrows, None, 80)
        self.check(value_d, uri, nrows, None, 90)
        self.check(value_modW, uri, nrows, None, 100)
        self.check(value_a, uri, nrows, None, 110)
        self.check(value_modX, uri, nrows, None, 120)
        self.check(value_modY, uri, nrows, None, 130)
        self.check(value_modZ, uri, nrows, None, 140)

        # Pin stable to timestamp 40 if prepare otherwise 30.
        if self.prepare:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))
        else:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))

        # Checkpoint to ensure the data is flushed, then rollback to the stable timestamp.
        if not self.in_memory:
            self.session.checkpoint()
        self.conn.rollback_to_stable()

        # Check that the correct data is seen at and after the stable timestamp.
        self.check(value_modQ, uri, nrows, None, 30)
        self.check(value_modQ, uri, nrows, None, 150)
        self.check(value_a, uri, nrows, None, 20)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        calls = stat_cursor[stat.conn.txn_rts][2]
        hs_removed = stat_cursor[stat.conn.txn_rts_hs_removed][2]
        hs_sweep = stat_cursor[stat.conn.txn_rts_sweep_hs_keys][2]
        keys_removed = stat_cursor[stat.conn.txn_rts_keys_removed][2]
        keys_restored = stat_cursor[stat.conn.txn_rts_keys_restored][2]
        pages_visited = stat_cursor[stat.conn.txn_rts_pages_visited][2]
        upd_aborted = stat_cursor[stat.conn.txn_rts_upd_aborted][2]
        stat_cursor.close()

        self.assertEqual(calls, 1)
        self.assertEqual(keys_removed, 0)
        self.assertEqual(keys_restored, 0)
        self.assertGreater(pages_visited, 0)
        if self.in_memory:
            self.assertEqual(upd_aborted, nrows * 11)
            self.assertEqual(hs_removed + hs_sweep, 0)
        else:
            self.assertGreaterEqual(upd_aborted + hs_removed + hs_sweep, nrows * 11)

if __name__ == '__main__':
    wttest.run()
