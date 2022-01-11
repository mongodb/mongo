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

import wttest
from suite_subprocess import suite_subprocess
from helper import compare_files

# test_util21.py
# Ensure that wt dump can dump obsolete data in the history store.
class test_util21(wttest.WiredTigerTestCase, suite_subprocess):
    conn_config = 'cache_size=50MB'

    def add_data_with_timestamp(self, uri, value, ts):
        # Apply a series of updates with commit timestamp.
        cursor = self.session.open_cursor(uri)
        for i in range(1, 5):
            self.session.begin_transaction()
            cursor[str(i)] = value
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        cursor.close()

    def test_dump_obsolete_data(self):
        uri = 'table:test_util21'
        create_params = 'key_format=S,value_format=S'
        self.session.create(uri, create_params)

        value1 = 'a' * 100
        value2 = 'b' * 100
        value3 = 'c' * 100
        value4 = 'd' * 100

        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))

        self.add_data_with_timestamp(uri, value1, 2)
        self.add_data_with_timestamp(uri, value2, 3)
        self.add_data_with_timestamp(uri, value3, 5)
        self.add_data_with_timestamp(uri, value4, 7)
        # Perform checkpoint, to clean the dirty pages and place values on disk.
        self.session.checkpoint()

        # Set stable timestamp, so we don't lose data when closing/opening connection when using wt dump.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))

        # Call dump on the values before the oldest timestamp is set
        self.runWt(['dump', 'file:WiredTigerHS.wt'], outfilename="before_oldest")

        # Set oldest timestamp, and checkpoint, the obsolete data should not removed as
        # the pages are clean.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(6))
        self.session.checkpoint()
        self.runWt(['dump', 'file:WiredTigerHS.wt'], outfilename="after_oldest")

        self.assertEqual(True, compare_files(self, "before_oldest", "after_oldest"))

if __name__ == '__main__':
    wttest.run()
