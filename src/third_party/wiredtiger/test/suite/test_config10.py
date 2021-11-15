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
# test_config10.py
#   Test the debug configuration setting on the session which evicts
#   pages as they are released.
#

import wiredtiger, wttest
from wiredtiger import stat
from wtscenario import make_scenarios
from wtdataset import SimpleDataSet

class test_config10(wttest.WiredTigerTestCase):
    conn_config = 'statistics=(fast)'
    session_config = 'isolation=snapshot'
    key_format_values = [
        ('column', dict(key_format='r')),
        ('integer_row', dict(key_format='i')),
    ]

    scenarios = make_scenarios(key_format_values)

    def test_config10(self):
        uri = 'table:test_config10'
        nrows = 10000

        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format="S", config='log=(enabled=false)')
        ds.populate()

        # Pin oldest and stable timestamps to 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        value = 'abcd' * 1000

        s = self.conn.open_session()
        cursor = s.open_cursor(uri)

        # Insert some values to fill up the cache, but not exceeding 80% of it.
        # Then, do a checkpoint. Here, we don't expect eviction to trigger.
        for i in range(1, nrows):
            s.begin_transaction()
            cursor[i] = value + str(i)
            s.commit_transaction()

        s.checkpoint()
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        prev_cache_usage = stat_cursor[stat.conn.cache_bytes_inuse][2]
        stat_cursor.close()

        cursor.close()

        # Reconfigure the session to use the new debug configuration that evicts
        # pages as released.
        s.reconfigure("debug=(release_evict_page=true)")
        cursor = s.open_cursor(uri)

        # We walk through and read all the content, but this time we expect pages
        # to be evicted, and hence cache usage to at least halve.
        for i in range(1, nrows):
            s.begin_transaction()
            self.assertEqual(cursor[i], value + str(i))
            s.rollback_transaction()

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        cache_usage = stat_cursor[stat.conn.cache_bytes_inuse][2]
        self.assertGreater(prev_cache_usage, cache_usage * 2)
        stat_cursor.close()

if __name__ == '__main__':
    wttest.run()
