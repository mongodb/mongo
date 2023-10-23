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
import random
from wtscenario import make_scenarios
from wtdataset import SimpleDataSet, simple_value

# test_app_thread_evict01.py
# Test to trigger application threads to perform eviction.

@wttest.skip_for_hook("timestamp", "This test uses dataset and hooks assume that timestamp are used and fails.")
class test_app_thread_evict01(wttest.WiredTigerTestCase):
    uri = "table:test_app_thread_evict001"
    format_values = [
    ('row_integer', dict(key_format='i', value_format='S')),
    ]

    conn_config = "cache_size=50MB,statistics=(all),statistics_log=(wait=1,json=true,on_close=true)," \
        "eviction=(threads_max=1),eviction_updates_trigger=5"
    rows = 20000
    scenarios = make_scenarios(format_values)

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def insert_range(self,uri,rows):
        cursor = self.session.open_cursor(uri)

        for i in range(rows):
            self.session.begin_transaction()
            cursor.set_key(i+1)
            value = simple_value(cursor, i) + 'abcdef' * random.randint(500,1000)
            cursor.set_value((value))
            cursor.insert()
            self.session.commit_transaction()

    def test_app_thread_evict01(self):
        self.skipTest("This test fails randomly when it cannot pull application threads to perform eviction.")
        num_app_evict_snapshot_refreshed = 0

        format='key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(self.uri, format)
        # Create our table.
        ds = SimpleDataSet(self, self.uri, 1000, key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        # Start a long running transaction.
        session2 = self.conn.open_session()
        session2.begin_transaction()
        cursor = self.session.open_cursor(self.uri)
        key = 350
        cursor.set_key(key)
        cursor.set_value(str(key))
        cursor.insert()

        # For our target stat to be incremented we need our application thread to evict a page, but
        # this is probabilistic as the application thread is always racing against the internal
        # eviction threads. Give the application thread a few chances to beat the internal thread
        for _ in range(0, 10):
            for i in range(500):
                self.insert_range(self.uri, i)

            cursor.set_key(key)
            self.assertEquals(cursor.search(), 0)

            num_app_evict_snapshot_refreshed = self.get_stat(wiredtiger.stat.conn.application_evict_snapshot_refreshed)
            if num_app_evict_snapshot_refreshed > 0:
                break

        self.assertGreater(self.get_stat(wiredtiger.stat.conn.application_evict_snapshot_refreshed), 0)

if __name__ == '__main__':
    wttest.run()
