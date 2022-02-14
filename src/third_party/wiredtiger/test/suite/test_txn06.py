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
# test_txn06.py
#   Transactions: test long-running snapshots

from suite_subprocess import suite_subprocess
from wtdataset import SimpleDataSet
import wttest
from wtscenario import make_scenarios

class test_txn06(wttest.WiredTigerTestCase, suite_subprocess):
    conn_config = 'verbose=[transaction]'
    tablename = 'test_txn06'
    uri = 'table:' + tablename
    source_uri = 'table:' + tablename + "_src"
    nrows = 100000

    format_values = [
        ('row', dict(key_format = 'S', value_format='S')),
        ('var', dict(key_format = 'r', value_format='S')),
        ('fix', dict(key_format = 'r', value_format='8t')),
    ]
    scenarios = make_scenarios(format_values)

    def test_long_running(self):
        # Populate a table
        ds = SimpleDataSet(self, self.source_uri, self.nrows,
            key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        # Now scan the table and copy the rows into a new table. The cursor will keep the snapshot
        # in self.session pinned while the inserts cause new IDs to be allocated.
        format = "key_format={},value_format={}".format(self.key_format, self.value_format)
        c_src = self.session.create(self.uri, format)
        c_src = self.session.open_cursor(self.source_uri)
        insert_session = self.conn.open_session()
        c = insert_session.open_cursor(self.uri)
        for k, v in c_src:
            c[k] = v

        # We were trying to generate a message matching this pattern.
        self.captureout.checkAdditionalPattern(self, "old snapshot")

if __name__ == '__main__':
    wttest.run()
