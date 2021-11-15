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
# checkpoint:history_store
# [END_TAGS]
#
# test_checkpoint03.py
#   Test that checkpoint writes out updates to the history store file.
#

from suite_subprocess import suite_subprocess
import wiredtiger, wttest
from wiredtiger import stat
from wtscenario import make_scenarios

class test_checkpoint03(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_checkpoint03'
    conn_config = 'statistics=(all)'
    uri = 'table:' + tablename
    session_config = 'isolation=snapshot, '

    format_values = [
        ('column-fix', dict(key_format='r', value_format='8t')),
        ('column', dict(key_format='r', value_format='i')),
        ('integer_row', dict(key_format='i', value_format='i')),
    ]

    scenarios = make_scenarios(format_values)

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def test_checkpoint_writes_to_hs(self):
        # Create a basic table.
        config = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(self.uri, config)
        self.session.begin_transaction()
        self.conn.set_timestamp('oldest_timestamp=1')

        # Insert 3 updates in separate transactions.
        cur1 = self.session.open_cursor(self.uri)
        cur1[1] = 1
        self.session.commit_transaction('commit_timestamp=2')

        self.session.begin_transaction()
        cur1[1] = 2
        self.session.commit_transaction('commit_timestamp=3')

        self.session.begin_transaction()
        cur1[1] = 3
        self.session.commit_transaction('commit_timestamp=4')

        # Call checkpoint.
        self.session.checkpoint()

        # Validate that we wrote to history store, note that the history store statistic is not
        # counting how many writes we did, just that we did write. Hence for multiple writes it may
        # only increment a single time.
        hs_writes = self.get_stat(stat.conn.cache_write_hs)
        self.assertGreaterEqual(hs_writes, 1)

        # Add a new update.
        self.session.begin_transaction()
        cur1[1] = 4
        self.session.commit_transaction('commit_timestamp=5')
        self.session.checkpoint()

        # Check that we wrote something to the history store in the last checkpoint we ran, as we
        # should've written the previous update.
        hs_writes = self.get_stat(stat.conn.cache_write_hs)
        self.assertGreaterEqual(hs_writes, 2)

        # Close the connection.
        self.close_conn()

        # Open a new connection and validate that we see the latest update as part of the datafile.
        conn2 = self.setUpConnectionOpen('.')
        session2 = self.setUpSessionOpen(conn2)
        session2.create(self.uri, 'key_format={},value_format=i'.format(self.key_format))

        cur2 = session2.open_cursor(self.uri)
        cur2.set_key(1)
        cur2.search()
        self.assertEqual(cur2.get_value(), 4)

if __name__ == '__main__':
    wttest.run()
