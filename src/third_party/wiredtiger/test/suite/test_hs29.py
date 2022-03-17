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

# test_hs29.py
# It is possible to end up with 3 opened history store cursors at the same time when the following
# occurs:
# - The reconciliation process opens one history store cursor.
# - The function hs_delete_reinsert_from_pos creates a history store cursor too. This means we need
# an update with an OOO timestamp to trigger that function.
# - The function wt_rec_hs_clear_on_tombstone creates a history store cursor as well. This means we
# need a tombstone to trigger the function, i.e a deleted key.
class test_hs29(wttest.WiredTigerTestCase):

    def test_3_hs_cursors(self):

        # Create a table.
        uri = "table:test_hs_cursor"
        self.session.create(uri, 'key_format=S,value_format=S')

        # Open one cursor to operate on the table and another one to perform eviction.
        cursor = self.session.open_cursor(uri)
        cursor2 = self.session.open_cursor(uri, None, "debug=(release_evict=true)")

        # Create two keys and perform an update on each.
        self.session.begin_transaction()
        cursor['1'] = '1'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))

        self.session.begin_transaction()
        cursor['1'] = '11'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(3))

        self.session.begin_transaction()
        cursor['2'] = '2'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))

        self.session.begin_transaction()
        cursor['2'] = '22'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))

        # Perform eviction.
        cursor2.set_key('1')
        self.assertEqual(cursor2.search(), 0)
        self.assertEqual(cursor2.get_value(), '11')
        self.assertEqual(cursor2.reset(), 0)

        cursor2.set_key('2')
        self.assertEqual(cursor2.search(), 0)
        self.assertEqual(cursor2.get_value(), '22')
        self.assertEqual(cursor2.reset(), 0)

        # Remove the first key without giving a ts.
        self.session.begin_transaction()
        cursor.set_key('1')
        cursor.remove()
        self.session.commit_transaction()

        # Update the second key with out of order timestamp.
        self.session.begin_transaction()
        cursor['2'] = '222'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(5))

        # Close the connection to trigger a final checkpoint and reconciliation.
        self.conn.close()

if __name__ == '__main__':
    wttest.run()
