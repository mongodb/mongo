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
from wiredtiger import stat, WT_NOTFOUND
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_rollback_to_stable19.py
# Test that rollback to stable aborts both insert and remove updates from a single prepared transaction
class test_rollback_to_stable19(test_rollback_to_stable_base):

    in_memory_values = [
        ('no_inmem', dict(in_memory=False)),
        ('inmem', dict(in_memory=True))
    ]

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('row_integer', dict(key_format='i', value_format='S')),
    ]

    restart_options = [
        ('shutdown', dict(crash=False)),
        ('crash', dict(crash=True)),
    ]

    scenarios = make_scenarios(in_memory_values, format_values, restart_options)

    def conn_config(self):
        config = 'cache_size=50MB,statistics=(all),eviction_dirty_trigger=10,' \
                 'eviction_updates_trigger=10'
        if self.in_memory:
            config += ',in_memory=true'
        return config

    def test_rollback_to_stable_no_history(self):
        nrows = 1000

        # Create a table.
        uri = "table:rollback_to_stable19"
        ds_config = ',log=(enabled=false)' if self.in_memory else ''
        ds = SimpleDataSet(self, uri, 0,
            key_format=self.key_format, value_format=self.value_format, config=ds_config)
        ds.populate()

        if self.value_format == '8t':
            valuea = 97
        else:
            valuea = "aaaaa" * 100

        # Pin oldest and stable timestamps to 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        # Perform several updates and removes.
        s = self.conn.open_session()
        cursor = s.open_cursor(uri)
        s.begin_transaction()
        for i in range(1, nrows + 1):
            cursor[ds.key(i)] = valuea
            cursor.set_key(i)
            cursor.remove()
        cursor.close()
        s.prepare_transaction('prepare_timestamp=' + self.timestamp_str(20))

        # Configure debug behavior on a cursor to evict the page positioned on when the reset API
        # is used.
        evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")

        # Search for the key so we position our cursor on the page that we want to evict.
        self.session.begin_transaction("ignore_prepare = true")
        evict_cursor.set_key(1)
        if self.value_format == '8t':
            # In FLCS deleted values read back as 0.
            self.assertEquals(evict_cursor.search(), 0)
            self.assertEquals(evict_cursor.get_value(), 0)
        else:
            self.assertEquals(evict_cursor.search(), WT_NOTFOUND)
        evict_cursor.reset()
        evict_cursor.close()
        self.session.commit_transaction()

        # Search to make sure the data is not visible (or, in FLCS, that it's zero)
        self.session.begin_transaction("ignore_prepare = true")
        cursor2 = self.session.open_cursor(uri)
        cursor2.set_key(1)
        if self.value_format == '8t':
            self.assertEquals(cursor2.search(), 0)
            self.assertEquals(cursor2.get_value(), 0)
        else:
            self.assertEquals(cursor2.search(), WT_NOTFOUND)
        self.session.commit_transaction()
        cursor2.close()

        # Pin stable timestamp to 20.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        if not self.in_memory:
            self.session.checkpoint()

        if not self.in_memory:
            if self.crash:
                simulate_crash_restart(self, ".", "RESTART")
            else:
                # Close and reopen the connection
                self.reopen_conn()
        else:
            s.rollback_transaction()

        # Verify data is not visible.
        self.check(valuea, uri, 0, nrows, 20)
        self.check(valuea, uri, 0, nrows, 30)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        upd_aborted = stat_cursor[stat.conn.txn_rts_upd_aborted][2]
        keys_removed = stat_cursor[stat.conn.txn_rts_keys_removed][2]

        # After restart (not crash) the stats for the aborted updates will be 0, as the updates
        # will be aborted during shutdown, and on startup there will be no updates to be aborted.
        # This is similar case with keys removed.
        if self.in_memory or not self.crash:
            self.assertEqual(upd_aborted, 0)
            self.assertEqual(keys_removed, 0)
        else:
            self.assertGreater(upd_aborted, 0)
            self.assertGreater(keys_removed, 0)

        stat_cursor.close()

    def test_rollback_to_stable_with_history(self):
        nrows = 1000

        # Create a table.
        uri = "table:rollback_to_stable19"
        ds_config = ',log=(enabled=false)' if self.in_memory else ''
        ds = SimpleDataSet(self, uri, 0,
            key_format=self.key_format, value_format=self.value_format, config=ds_config)
        ds.populate()

        if self.value_format == '8t':
            valuea = 97
            valueb = 98
        else:
            valuea = "aaaaa" * 100
            valueb = "bbbbb" * 100

        # Pin oldest and stable timestamps to 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        # Perform several updates.
        self.large_updates(uri, valuea, ds, nrows, 0, 20)

        # Perform several removes.
        self.large_removes(uri, ds, nrows, 0, 30)

        # Perform several updates and removes.
        s = self.conn.open_session()
        cursor = s.open_cursor(uri)
        s.begin_transaction()
        for i in range(1, nrows + 1):
            cursor[ds.key(i)] = valueb
            cursor.set_key(i)
            cursor.remove()
        cursor.close()
        s.prepare_transaction('prepare_timestamp=' + self.timestamp_str(40))

        # Configure debug behavior on a cursor to evict the page positioned on when the reset API
        # is used.
        evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")

        # Search for the key so we position our cursor on the page that we want to evict.
        self.session.begin_transaction("ignore_prepare = true")
        evict_cursor.set_key(1)
        if self.value_format == '8t':
            # In FLCS deleted values read back as 0.
            self.assertEquals(evict_cursor.search(), 0)
            self.assertEquals(evict_cursor.get_value(), 0)
        else:
            self.assertEquals(evict_cursor.search(), WT_NOTFOUND)
        evict_cursor.reset()
        evict_cursor.close()
        self.session.commit_transaction()

        # Search to make sure the data is not visible (or, in FLCS, that it's zero)
        self.session.begin_transaction("ignore_prepare = true")
        cursor2 = self.session.open_cursor(uri)
        cursor2.set_key(1)
        if self.value_format == '8t':
            self.assertEquals(cursor2.search(), 0)
            self.assertEquals(cursor2.get_value(), 0)
        else:
            self.assertEquals(cursor2.search(), WT_NOTFOUND)
        self.session.commit_transaction()
        cursor2.close()

        # Pin stable timestamp to 40.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))
        if not self.in_memory:
            self.session.checkpoint()

        if not self.in_memory:
            if self.crash:
                simulate_crash_restart(self, ".", "RESTART")
            else:
                # Close and reopen the connection
                self.reopen_conn()
        else:
            s.rollback_transaction()

        # Verify data.
        self.check(valuea, uri, nrows, None, 20)
        self.check(valuea, uri, 0, nrows, 30)
        self.check(valuea, uri, 0, nrows, 40)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        upd_aborted = stat_cursor[stat.conn.txn_rts_upd_aborted][2]
        hs_removed = stat_cursor[stat.conn.txn_rts_hs_removed][2]

        # After restart (not crash) the stats for the aborted updates and history store removed
        # will be 0, as the updates aborted and history store removed will occur during shutdown,
        # and on startup there will be no updates to be removed.
        if self.in_memory or not self.crash:
            self.assertEqual(hs_removed, 0)
            self.assertEqual(upd_aborted, 0)
        else:
            self.assertGreater(hs_removed, 0)
            self.assertGreater(upd_aborted, 0)

if __name__ == '__main__':
    wttest.run()
