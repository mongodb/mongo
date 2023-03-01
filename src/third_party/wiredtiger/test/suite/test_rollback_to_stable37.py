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
from rollback_to_stable_util import test_rollback_to_stable_base

# test_rollback_to_stable37.py
# Test that the rollback to stable to restore proper stable update from history store when a no timestamp
# update has rewritten the history store data.
class test_rollback_to_stable37(test_rollback_to_stable_base):
    conn_config = 'cache_size=1GB,statistics=(all),statistics_log=(json,on_close,wait=1),log=(enabled=false),verbose=(rts:5)'

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('row_integer', dict(key_format='i', value_format='S')),
    ]

    dryrun_values = [
        ('no_dryrun', dict(dryrun=False)),
        ('dryrun', dict(dryrun=True))
    ]

    scenarios = make_scenarios(format_values, dryrun_values)

    def test_rollback_to_stable(self):
        uri = 'table:test_rollback_to_stable37'
        nrows = 1000

        if self.value_format == '8t':
            value_a = 97
            value_b = 98
            value_c = 99
            value_d = 100
        else:
            value_a = 'a' * 10
            value_b = 'b' * 10
            value_c = 'c' * 10
            value_d = 'd' * 10

        # Create our table.
        ds = SimpleDataSet(self, uri, 0, key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        # Pin oldest and stable to timestamp 1.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        # Insert 300 updates to the same key.
        for i in range (20, 320):
            if self.value_format == '8t':
                self.large_updates(uri, value_a, ds, nrows, False, i)
            else:
                self.large_updates(uri, value_a + str(i), ds, nrows, False, i)

        old_reader_session = self.conn.open_session()
        old_reader_session.begin_transaction('read_timestamp=' + self.timestamp_str(10))

        self.large_updates(uri, value_b, ds, nrows, False, 2000)
        self.check(value_b, uri, nrows, None, 2000)

        self.evict_cursor(uri, nrows, value_b)

        # Insert update without a timestamp.
        self.large_updates(uri, value_c, ds, nrows, False, 0)
        self.check(value_c, uri, nrows, None, 0)

        self.evict_cursor(uri, nrows, value_c)

        self.large_updates(uri, value_d, ds, nrows, False, 3000)
        self.check(value_d, uri, nrows, None, 3000)

        old_reader_session.rollback_transaction()
        self.session.checkpoint()

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(2000))
        self.session.checkpoint()

        self.conn.rollback_to_stable('dryrun={}'.format('true' if self.dryrun else 'false'))

        self.check(value_c, uri, nrows, None, 1000)
        self.check(value_c, uri, nrows, None, 2000)

        if self.dryrun:
            self.check(value_d, uri, nrows, None, 3000)
        else:
            self.check(value_c, uri, nrows, None, 3000)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        keys_removed = stat_cursor[stat.conn.txn_rts_keys_removed][2]
        stat_cursor.close()

        self.assertEqual(keys_removed, 0)

if __name__ == '__main__':
    wttest.run()
