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
#
# [TEST_TAGS]
# rollback_to_stable
# aggregated_time_windows
# [END_TAGS]

from rollback_to_stable_util import test_rollback_to_stable_base
from wiredtiger import stat
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_rollback_to_stable18.py
# Test the rollback to stable shouldn't skip any pages that don't have aggregated time window.
class test_rollback_to_stable18(test_rollback_to_stable_base):

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('row_integer', dict(key_format='i', value_format='S')),
    ]

    prepare_values = [
        ('no_prepare', dict(prepare=False)),
        ('prepare', dict(prepare=True))
    ]

    scenarios = make_scenarios(format_values, prepare_values)

    def conn_config(self):
        config = 'cache_size=50MB,in_memory=true,statistics=(all),' \
                 'eviction_dirty_trigger=10,eviction_updates_trigger=10,verbose=(rts:5)'
        return config

    def test_rollback_to_stable(self):
        nrows = 10000

        # Create a table.
        uri = "table:rollback_to_stable18"
        ds_config = ',log=(enabled=false)'
        ds = SimpleDataSet(self, uri, 0,
            key_format=self.key_format, value_format=self.value_format, config=ds_config)
        ds.populate()

        if self.value_format == '8t':
            value_a = 97
        else:
            value_a = "aaaaa" * 100

        # Pin oldest and stable to timestamp 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        # Perform several updates.
        self.large_updates(uri, value_a, ds, nrows, self.prepare, 20)

        # Perform several removes.
        self.large_removes(uri, ds, nrows, self.prepare, 30)

        # Verify data is visible and correct.
        self.check(value_a, uri, nrows, None, 21 if self.prepare else 20)
        self.check(None, uri, 0, nrows, 31 if self.prepare else 30)

        # Configure debug behavior on a cursor to evict the page positioned on when the reset API is used.
        evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")

        # Search for the key so we position our cursor on the page that we want to evict.
        evict_cursor.set_key(1)
        evict_cursor.search()
        evict_cursor.reset()
        evict_cursor.close()

        # Pin stable and oldest to timestamp 30 if prepare otherwise 20.
        if self.prepare:
            self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(30) +
                ',stable_timestamp=' + self.timestamp_str(30))
        else:
            self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(20) +
                ',stable_timestamp=' + self.timestamp_str(20))

        # Perform rollback to stable.
        self.conn.rollback_to_stable()

        # Verify data is not visible.
        self.check(value_a, uri, nrows, None, 30)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        calls = stat_cursor[stat.conn.txn_rts][2]
        upd_aborted = stat_cursor[stat.conn.txn_rts_upd_aborted][2]
        self.assertEqual(calls, 1)
        self.assertEqual(upd_aborted, nrows)
