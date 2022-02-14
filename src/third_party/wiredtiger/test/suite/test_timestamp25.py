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
# test_timestamp25.py
#   Timestamps: backward compatible oldest and stable names.
#

import wttest
from suite_subprocess import suite_subprocess

class test_timestamp25(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_timestamp25'
    uri = 'table:' + tablename

    def test_short_names(self):
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(100))
        self.assertTimestampsEqual(self.conn.query_timestamp("get=oldest"), self.timestamp_str(100))
        self.assertTimestampsEqual(\
            self.conn.query_timestamp("get=oldest_timestamp"), self.timestamp_str(100))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(100))
        self.assertTimestampsEqual(self.conn.query_timestamp("get=stable"), self.timestamp_str(100))
        self.assertTimestampsEqual(\
            self.conn.query_timestamp("get=stable_timestamp"), self.timestamp_str(100))

if __name__ == '__main__':
    wttest.run()
