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
# checkpoint:metadata
# [END_TAGS]

from helper import copy_wiredtiger_home
import wiredtiger, wttest
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios
from wiredtiger import stat

# test_checkpoint_snapshot01.py
#   Checkpoint snapshot - Create multiple sessions which creates snapshots and
#   checkpoint to save the snapshot details in metadata file.
#

class test_checkpoint_snapshot01(wttest.WiredTigerTestCase):
    uri = "table:test_checkpoint_snapshot01"
    conn_config = 'cache_size=50MB'

    format_values = [
        ('column-fix', dict(key_format='r', value_format='8t')),
        ('column', dict(key_format='r', value_format='u')),
        ('row_string', dict(key_format='S', value_format='u')),
    ]

    scenarios = make_scenarios(format_values)

    nsessions = 5
    nkeys = 40
    nrows = 100

    def test_checkpoint_snapshot(self):

       # Create a table.
        ds = SimpleDataSet(self, self.uri, self.nrows, \
                key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        if self.value_format == '8t':
            value = 86
        else:
            value = b"aaaaa" * 100

        sessions = [0] * self.nsessions
        cursors = [0] * self.nsessions

        for j in range (0, self.nsessions):
            sessions[j] = self.conn.open_session()
            cursors[j] = sessions[j].open_cursor(self.uri)
            sessions[j].begin_transaction()

            start = (j * self.nkeys)
            end = start + self.nkeys

            for i in range(start, end):
                cursors[j].set_key(ds.key(self.nrows + i))
                cursors[j].set_value(value)
                self.assertEquals(cursors[j].insert(),0)

        session_p2 = self.conn.open_session()
        session_p2.checkpoint()

        #Simulate a crash by copying to a new directory(RESTART).
        copy_wiredtiger_home(self, ".", "RESTART")

        # Open the new directory.
        self.conn = self.setUpConnectionOpen("RESTART")
        self.session = self.setUpSessionOpen(self.conn)

if __name__ == '__main__':
    wttest.run()
