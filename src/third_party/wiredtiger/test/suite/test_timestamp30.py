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
# test_timestamp30.py
#   Timestamps: the pinned-timestamp lag statistics must never report a
#   negative value when a timestamp trails the oldest timestamp.

import wttest
from wiredtiger import stat

class test_timestamp30(wttest.WiredTigerTestCase):
    conn_config = 'statistics=(all)'

    def get_stat(self, which):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        val = stat_cursor[which][2]
        stat_cursor.close()
        return val

    # The durable timestamp is not required to lead the oldest timestamp: an
    # application may advance oldest without ever setting durable, and may even
    # move durable backwards. When durable trails oldest the "pinned by the
    # oldest timestamp" statistic must report 0 rather than an underflowed
    # (negative) value.
    def test_pinned_timestamp_oldest_no_underflow(self):
        # Advance oldest and stable while leaving the durable timestamp unset.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(100) +
            ',stable_timestamp=' + self.timestamp_str(100))
        self.assertGreaterEqual(
            self.get_stat(stat.conn.txn_pinned_timestamp_oldest), 0)
        self.assertGreaterEqual(
            self.get_stat(stat.conn.txn_pinned_timestamp_lag), 0)

        # Explicitly move the durable timestamp behind oldest.
        self.conn.set_timestamp('durable_timestamp=' + self.timestamp_str(50))
        self.assertGreaterEqual(
            self.get_stat(stat.conn.txn_pinned_timestamp_oldest), 0)

        # Once durable leads oldest the statistic reflects the real lag.
        self.conn.set_timestamp('durable_timestamp=' + self.timestamp_str(300))
        self.assertEqual(
            self.get_stat(stat.conn.txn_pinned_timestamp_oldest), 300 - 100)
