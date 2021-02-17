#!/usr/bin/env python
#
# Public Domain 2014-2020 MongoDB, Inc.
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

import wiredtiger, wtscenario, wttest
from wiredtiger import stat
from wtdataset import SimpleDataSet

# test_tiered04.py
#    Basic tiered storage API test.
class test_tiered04(wttest.WiredTigerTestCase):
    uri = "table:test_tiered04"

    auth_token = "test_token"
    extension_name = "test"
    retention = 600
    def conn_config(self):
        return \
          'statistics=(fast),' + \
          'tiered_storage=(enabled,local_retention=%d,' % self.retention + \
          'name=%s,' % self.extension_name + \
          'auth_token=%s)' % self.auth_token

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    # Test calling the flush_tier API.
    def test_tiered(self):
        self.session.create(self.uri, 'key_format=S')
        # The retention is in minutes. The stat is in seconds.
        retain = self.get_stat(stat.conn.tiered_retention)
        self.assertEqual(retain, self.retention)
        self.session.flush_tier(None)
        self.session.flush_tier('force=true')
        calls = self.get_stat(stat.conn.flush_tier)
        self.assertEqual(calls, 2)
        new = self.retention * 2
        config = 'tiered_storage=(local_retention=%d)' % new
        self.conn.reconfigure(config)
        self.session.flush_tier(None)
        retain = self.get_stat(stat.conn.tiered_retention)
        calls = self.get_stat(stat.conn.flush_tier)
        self.assertEqual(retain, new)
        self.assertEqual(calls, 3)

if __name__ == '__main__':
    wttest.run()
