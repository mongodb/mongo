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

import wiredtiger, wttest
from rollback_to_stable_util import test_rollback_to_stable_base
from wtdataset import SimpleDataSet
from wiredtiger import stat
from wtscenario import make_scenarios

# test_rollback_to_stable01.py
# Test that rollback to stable clears the remove operation.
class test_rollback_to_stable01(test_rollback_to_stable_base):
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

    dryrun_values = [
        ('no_dryrun', dict(dryrun=False)),
        ('dryrun', dict(dryrun=True)),
    ]

    scenarios = make_scenarios(format_values, in_memory_values, prepare_values, dryrun_values)

    def conn_config(self):
        config = 'cache_size=50MB,statistics=(all),verbose=(rts:5)'
        if self.in_memory:
            config += ',in_memory=true'
        return config

    def test_rollback_to_stable(self):
        nrows = 10000

        # Create a table.
        uri = "table:rollback_to_stable01"
        ds_config = 'log=(enabled=false)' if self.in_memory else ''
        ds = SimpleDataSet(self, uri, 0,
            key_format=self.key_format, value_format=self.value_format, config=ds_config)
        ds.populate()

        if self.value_format == '8t':
            valuea = 97
        else:
            valuea = "aaaaa" * 100

        # Pin oldest and stable to timestamp 1.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        self.large_updates(uri, valuea, ds, nrows, self.prepare, 10)
        # Check that all updates are seen.
        self.check(valuea, uri, nrows, None, 11 if self.prepare else 10)

        # Remove all keys with newer timestamp.
        self.large_removes(uri, ds, nrows, self.prepare, 20)
        # Check that the no keys should be visible.
        self.check(valuea, uri, 0, nrows, 21 if self.prepare else 20)

        # Pin stable to timestamp 20 if prepare otherwise 10.
        if self.prepare:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        else:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        # Checkpoint to ensure that all the updates are flushed to disk.
        if not self.in_memory:
            self.session.checkpoint()

        self.conn.rollback_to_stable("dryrun={}".format("true" if self.dryrun else "false"))
        # Check that the new updates are only seen after the update timestamp.
        self.session.breakpoint()
        if self.dryrun:
            self.check(0, uri, nrows if self.value_format == '8t' else 0, None, 20)
        else:
            self.check(valuea, uri, nrows, None, 20)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        calls = stat_cursor[stat.conn.txn_rts][2]
        hs_removed = stat_cursor[stat.conn.txn_rts_hs_removed][2]
        keys_removed = stat_cursor[stat.conn.txn_rts_keys_removed][2]
        keys_restored = stat_cursor[stat.conn.txn_rts_keys_restored][2]
        keys_restored_dryrun = stat_cursor[stat.conn.txn_rts_keys_restored_dryrun][2]
        pages_visited = stat_cursor[stat.conn.txn_rts_pages_visited][2]
        upd_aborted = stat_cursor[stat.conn.txn_rts_upd_aborted][2]
        upd_aborted_dryrun = stat_cursor[stat.conn.txn_rts_upd_aborted_dryrun][2]
        stat_cursor.close()

        self.assertEqual(calls, 1)
        self.assertEqual(hs_removed, 0)
        self.assertEqual(keys_removed, 0)
        if self.dryrun:
            self.assertEqual(upd_aborted, 0)
            self.assertEqual(upd_aborted_dryrun + keys_restored_dryrun, nrows)
        elif self.in_memory:
            self.assertEqual(upd_aborted + upd_aborted_dryrun, nrows)
        else:
            self.assertEqual(upd_aborted + keys_restored, nrows)
        self.assertGreaterEqual(keys_restored, 0)
        self.assertGreater(pages_visited, 0)

if __name__ == '__main__':
    wttest.run()
