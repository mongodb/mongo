#!/usr/bin/env python
#
# Public Domain 2014-2016 MongoDB, Inc.
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
# test_bug008.py
#       Regression tests.

import wiredtiger, wttest
from helper import simple_populate, key_populate, value_populate
from wtscenario import check_scenarios

# Tests for invisible updates.
class test_bug008(wttest.WiredTigerTestCase):
    scenarios = check_scenarios([
        ('fix', dict(fmt='key_format=r,value_format=8t', empty=1)),
        ('row', dict(fmt='key_format=S', empty=0)),
        ('var', dict(fmt='key_format=r', empty=0))
    ])

    # Verify cursor search and search-near operations on a file with a set of
    # on-page visible records, and a set of insert-list invisible records.
    def test_search_invisible_one(self):
        uri = 'file:test_bug008'                # This is a btree layer test.

        # Populate the tree and reopen the connection, forcing it to disk
        # and moving the records to an on-page format.
        simple_populate(self, uri, self.fmt, 100)
        self.reopen_conn()

        # Begin a transaction, and add some additional records.
        self.session.begin_transaction()
        cursor = self.session.open_cursor(uri, None)
        for i in range(100, 140):
            cursor[key_populate(cursor, i)] = value_populate(cursor, i)

        # Open a separate session and cursor.
        s = self.conn.open_session()
        cursor = s.open_cursor(uri, None)

        # Search for an invisible record.
        cursor.set_key(key_populate(cursor, 130))
        if self.empty:
            # Invisible updates to fixed-length column-store objects are
            # invisible to the reader, but the fact that they exist past
            # the end of the initial records causes the instantiation of
            # empty records: confirm successful return of an empty row.
            cursor.search()
            self.assertEqual(cursor.get_key(), 130)
            self.assertEqual(cursor.get_value(), 0)
        else:
            # Otherwise, we should not find any matching records.
            self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)

        # Search-near for an invisible record, which should succeed, returning
        # the last visible record.
        cursor.set_key(key_populate(cursor, 130))
        cursor.search_near()
        if self.empty:
            # Invisible updates to fixed-length column-store objects are
            # invisible to the reader, but the fact that they exist past
            # the end of the initial records causes the instantiation of
            # empty records: confirm successful return of an empty row.
            cursor.search()
            self.assertEqual(cursor.get_key(), 130)
            self.assertEqual(cursor.get_value(), 0)
        else:
            # Otherwise, we should find the closest record for which we can see
            # the value.
            self.assertEqual(cursor.get_key(), key_populate(cursor, 100))
            self.assertEqual(cursor.get_value(), value_populate(cursor, 100))

    # Verify cursor search and search-near operations on a file with a set of
    # on-page visible records, a set of insert-list visible records, and a set
    # of insert-list invisible records.  (The reason I'm adding this slightly
    # different test is because I want to confirm that if search positions the
    # the cursor in the insert list with a set of invisible updates, the right
    # fallback happens, whether the correct position is in the page slots or
    # the insert list.)
    def test_search_invisible_two(self):
        uri = 'file:test_bug008'                # This is a btree layer test.

        # Populate the tree and reopen the connection, forcing it to disk
        # and moving the records to an on-page format.
        simple_populate(self, uri, self.fmt, 100)
        self.reopen_conn()

        # Add some additional visible records.
        cursor = self.session.open_cursor(uri, None)
        for i in range(100, 120):
            cursor[key_populate(cursor, i)] = value_populate(cursor, i)
        cursor.close()

        # Begin a transaction, and add some additional records.
        self.session.begin_transaction()
        cursor = self.session.open_cursor(uri, None)
        for i in range(120, 140):
            cursor[key_populate(cursor, i)] = value_populate(cursor, i)

        # Open a separate session and cursor.
        s = self.conn.open_session()
        cursor = s.open_cursor(uri, None)

        # Search for an invisible record.
        cursor.set_key(key_populate(cursor, 130))
        if self.empty:
            # Invisible updates to fixed-length column-store objects are
            # invisible to the reader, but the fact that they exist past
            # the end of the initial records causes the instantiation of
            # empty records: confirm successful return of an empty row.
            cursor.search()
            self.assertEqual(cursor.get_key(), 130)
            self.assertEqual(cursor.get_value(), 0)
        else:
            # Otherwise, we should not find any matching records.
            self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)

        # Search-near for an invisible record, which should succeed, returning
        # the last visible record.
        cursor.set_key(key_populate(cursor, 130))
        cursor.search_near()
        if self.empty:
            # Invisible updates to fixed-length column-store objects are
            # invisible to the reader, but the fact that they exist past
            # the end of the initial records causes the instantiation of
            # empty records: confirm successful return of an empty row.
            cursor.search()
            self.assertEqual(cursor.get_key(), 130)
            self.assertEqual(cursor.get_value(), 0)
        else:
            # Otherwise, we should find the closest record for which we can see
            # the value.
            self.assertEqual(cursor.get_key(), key_populate(cursor, 119))
            self.assertEqual(cursor.get_value(), value_populate(cursor, 119))


if __name__ == '__main__':
    wttest.run()
