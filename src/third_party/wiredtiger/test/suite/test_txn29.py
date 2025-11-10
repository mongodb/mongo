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
from helper import simulate_crash_restart

# test_txn29.py
#   Test that transaction cannot be rolled back after being logged.
class test_txn29(wttest.WiredTigerTestCase):
    conn_config = "log=(enabled=true)"

    def test_transaction_logging(self):
        if wiredtiger.diagnostic_build():
            self.skipTest('requires a non-diagnostic build')

        # Create a logged table
        uri1 = "file:txn29-1"
        self.session.create(uri1, 'key_format=i,value_format=S')

        # Create a non-logged table
        uri2 = "file:txn29-2"
        self.session.create(uri2, 'key_format=i,value_format=S,log=(enabled=false)')

        # Do an update on the logged table and the non-logged table
        self.session.begin_transaction()
        cursor1 = self.session.open_cursor(uri1)
        cursor1[1] = "aaaa"
        cursor1.reset()
        cursor2 = self.session.open_cursor(uri2)
        cursor2[1] = "aaaa"
        cursor2.reset()
        self.session.commit_transaction(f'sync=on,commit_timestamp={self.timestamp_str(20)}')


        # Do an update on the logged table and the non-logged table
        self.session.begin_transaction()
        cursor1 = self.session.open_cursor(uri1)
        cursor1[1] = "bbbb"
        cursor1.reset()
        cursor2 = self.session.open_cursor(uri2)
        cursor2[1] = "bbbb"
        cursor2.reset()
        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: self.session.commit_transaction(f'sync=on,commit_timestamp={self.timestamp_str(10)}'))

        simulate_crash_restart(self, ".", "RESTART")

        # Should not see bbbb.
        self.session.begin_transaction()
        cursor2 = self.session.open_cursor(uri2)
        cursor2.set_key(1)
        self.assertEqual(cursor2.search(), wiredtiger.WT_NOTFOUND)
        cursor2.close()
        cursor1 = self.session.open_cursor(uri1)
        cursor1.set_key(1)
        cursor1.search()
        value = cursor1.get_value()
        self.assertEqual(value, 'aaaa')
        cursor1.close()
        self.session.rollback_transaction()

        self.ignoreStderrPatternIfExists('unexpected timestamp usage')
