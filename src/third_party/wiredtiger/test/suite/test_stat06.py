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
from time import sleep
from wiredtiger import stat

# test_stat06.py
#    Check that statistics are started or stopped when intended
class test_stat06(wttest.WiredTigerTestCase):

    def test_stats_on(self):
        self.close_conn()
        self.conn = self.wiredtiger_open(None, "statistics=(fast)")
        self.stats_gathered(True)

    def test_stats_off(self):
        self.close_conn()
        self.conn = self.wiredtiger_open(None, "statistics=(none),statistics_log=(json)")
        self.stats_gathered(False)

    def stats_gathered(self, stats_expected):
        self.session = self.conn.open_session()
        self.session.create("table:foo", None)
        self.session.create("table:bar", None)
        sleep(2)
        if stats_expected:
            stat_cursor = self.session.open_cursor('statistics:', None, None)
            self.assertTrue(stat_cursor[stat.conn.file_open][2] > 0)
        else:
            msg = '/database statistics configuration/'
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
                self.session.open_cursor('statistics:', None, None), msg)

if __name__ == '__main__':
    wttest.run()
