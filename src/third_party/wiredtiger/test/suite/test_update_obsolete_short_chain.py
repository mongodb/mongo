#!/usr/bin/env python
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as compiled
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

import wttest
from wiredtiger import stat
from wtdataset import SimpleDataSet

# test_update_obsolete_short_chain.py
# Regression test for short update chains in __wt_update_serial.
class test_update_obsolete_short_chain(wttest.WiredTigerTestCase):
    conn_config = 'statistics=(all)'

    def get_stat(self, stat_id):
        stat_cursor = self.session.open_cursor('statistics:')
        value = stat_cursor[stat_id][2]
        stat_cursor.close()
        return value

    def update_with_ts(self, uri, key, value, ts):
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        cursor[key] = value
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        cursor.close()

    def pin_timestamps(self, ts):
        self.conn.set_timestamp(
            'oldest_timestamp=' + self.timestamp_str(ts) +
            ',stable_timestamp=' + self.timestamp_str(ts))

    def test_short_chain_no_prune_then_prune(self):
        uri = 'table:update_obsolete_short_chain'

        # Disable table logging so timestamped updates are valid.
        ds = SimpleDataSet(self, uri, 0, key_format='i', value_format='S',
            config='log=(enabled=false)')
        ds.populate()

        key = 1
        self.update_with_ts(uri, key, 'value-a', 10)
        self.pin_timestamps(10)

        removed_start = self.get_stat(stat.conn.cache_obsolete_updates_removed)

        # After this update, the chain is [new] -> [prior] -> NULL.
        # There is nothing to prune.
        self.update_with_ts(uri, key, 'value-b', 20)
        self.pin_timestamps(20)
        removed_after_second = self.get_stat(stat.conn.cache_obsolete_updates_removed)
        self.assertEqual(removed_after_second, removed_start)

        # Grow the chain to length >= 3. Obsolete updates are then eligible
        # for cleanup during reconciliation.
        self.update_with_ts(uri, key, 'value-c', 30)
        self.pin_timestamps(30)

        self.session.checkpoint()
        removed_after_checkpoint = self.get_stat(stat.conn.cache_obsolete_updates_removed)
        self.assertGreater(removed_after_checkpoint, removed_after_second)

        cursor = self.session.open_cursor(uri)
        self.assertEqual(cursor[key], 'value-c')
        cursor.close()

if __name__ == '__main__':
    wttest.run()
