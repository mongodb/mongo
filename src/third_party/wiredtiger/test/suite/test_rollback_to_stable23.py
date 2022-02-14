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

from helper import simulate_crash_restart
from test_rollback_to_stable01 import test_rollback_to_stable_base
from wiredtiger import stat
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

def mod_val(value, char, location, nbytes=1):
    return value[0:location] + char + value[location+nbytes:]

# test_rollback_to_stable23.py
# Test to verify that search operation uses proper base update while returning modifies from
# the history store after the on-disk update is removed by the rollback to stable. Since FLCS
# inherently doesn't support modify, there's no need to run this on FLCS. (Note that
# self.value_format needs to exist anyway for the base class to use.)
class test_rollback_to_stable23(test_rollback_to_stable_base):

    key_format_values = [
        ('column', dict(key_format='r')),
        ('row_integer', dict(key_format='i')),
    ]
    value_format='S'

    prepare_values = [
        ('no_prepare', dict(prepare=False)),
        ('prepare', dict(prepare=True))
    ]

    scenarios = make_scenarios(key_format_values, prepare_values)

    def conn_config(self):
        config = 'cache_size=50MB,statistics=(all),statistics_log=(json,on_close,wait=1)'
        return config

    def check_with_set_key(self, ds, check_value, uri, nrows, read_ts):
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction("read_timestamp = " + self.timestamp_str(read_ts))
        for i in range(1, nrows + 1):
            cursor.set_key(ds.key(i))
            self.assertEquals(cursor.search(), 0)
            self.assertEquals(cursor.get_value(), check_value)
        cursor.close()
        self.session.commit_transaction()

    def test_rollback_to_stable(self):
        nrows = 1000

        # Create a table without logging.
        uri = "table:rollback_to_stable23"
        ds = SimpleDataSet(self, uri, 0, key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        # Pin oldest and stable to timestamp 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        value_a = "aaaaa" * 100

        value_modQ = mod_val(value_a, 'Q', 0)
        value_modR = mod_val(value_modQ, 'R', 1)
        value_modS = mod_val(value_modR, 'S', 2)
        value_modT = mod_val(value_modS, 'T', 3)

        # Perform a combination of modifies and updates.
        self.large_updates(uri, value_a, ds, nrows, self.prepare, 20)
        self.large_modifies(uri, 'Q', ds, 0, 1, nrows, self.prepare, 30)
        self.large_modifies(uri, 'R', ds, 1, 1, nrows, self.prepare, 40)
        self.large_modifies(uri, 'S', ds, 2, 1, nrows, self.prepare, 50)
        self.large_modifies(uri, 'T', ds, 3, 1, nrows, self.prepare, 60)

        # Verify data is visible and correct.
        self.check(value_a, uri, nrows, None, 20)
        self.check(value_modQ, uri, nrows, None, 30)
        self.check(value_modR, uri, nrows, None, 40)
        self.check(value_modS, uri, nrows, None, 50)
        self.check(value_modT, uri, nrows, None, 60)

        # Pin stable to timestamp 60 if prepare otherwise 50.
        if self.prepare:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(60))
        else:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))

        # Checkpoint the database.
        self.session.checkpoint()

        # Simulate a server crash and restart.
        simulate_crash_restart(self, ".", "RESTART")

        # Check that the correct data is seen at and after the stable timestamp.
        self.check_with_set_key(ds, value_a, uri, nrows, 20)
        self.check_with_set_key(ds, value_modQ, uri, nrows, 30)
        self.check_with_set_key(ds, value_modR, uri, nrows, 40)
        self.check_with_set_key(ds, value_modS, uri, nrows, 50)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        hs_removed = stat_cursor[stat.conn.txn_rts_hs_removed][2]
        hs_restore_updates = stat_cursor[stat.conn.txn_rts_hs_restore_updates][2]
        upd_aborted = stat_cursor[stat.conn.txn_rts_upd_aborted][2]
        stat_cursor.close()

        self.assertEqual(hs_restore_updates, nrows)
        if self.prepare:
            self.assertGreaterEqual(upd_aborted, 0)
        else:
            self.assertEqual(upd_aborted, 0)
        self.assertGreaterEqual(hs_removed, nrows)

if __name__ == '__main__':
    wttest.run()
