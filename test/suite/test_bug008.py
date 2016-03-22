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

# Test search/search-near operations, including invisible values and keys
# past the end of the table.
class test_bug008(wttest.WiredTigerTestCase):
    uri = 'file:test_bug008'                # This is a btree layer test.
    scenarios = check_scenarios([
        ('fix', dict(fmt='key_format=r,value_format=8t', empty=1, colvar=0)),
        ('row', dict(fmt='key_format=S', empty=0, colvar=0)),
        ('var', dict(fmt='key_format=r', empty=0, colvar=1))
    ])

    # Verify cursor search and search-near operations in an empty table.
    def test_search_empty(self):
        # Create the object and open a cursor.
        self.session.create(self.uri, self.fmt)
        cursor = self.session.open_cursor(self.uri, None)

        # Search for a record past the end of the table, which should fail.
        cursor.set_key(key_populate(cursor, 100))
        self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)

        # Search-near for a record past the end of the table, which should fail.
        cursor.set_key(key_populate(cursor, 100))
        self.assertEqual(cursor.search_near(), wiredtiger.WT_NOTFOUND)

    # Verify cursor search and search-near operations at and past the end of
    # a file, with a set of on-page visible records.
    def test_search_eot(self):
        # Populate the tree and reopen the connection, forcing it to disk
        # and moving the records to an on-page format.
        simple_populate(self, self.uri, self.fmt, 100)
        self.reopen_conn()

        # Open a cursor.
        cursor = self.session.open_cursor(self.uri, None)

        # Search for a record at the end of the table, which should succeed.
        cursor.set_key(key_populate(cursor, 100))
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_key(), key_populate(cursor, 100))
        self.assertEqual(cursor.get_value(), value_populate(cursor, 100))

        # Search-near for a record at the end of the table, which should
        # succeed, returning the last record.
        cursor.set_key(key_populate(cursor, 100))
        self.assertEqual(cursor.search_near(), 0)
        self.assertEqual(cursor.get_key(), key_populate(cursor, 100))
        self.assertEqual(cursor.get_value(), value_populate(cursor, 100))

        # Search for a record past the end of the table, which should fail.
        cursor.set_key(key_populate(cursor, 200))
        self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)

        # Search-near for a record past the end of the table, which should
        # succeed, returning the last record.
        cursor.set_key(key_populate(cursor, 200))
        self.assertEqual(cursor.search_near(), -1)
        self.assertEqual(cursor.get_key(), key_populate(cursor, 100))
        self.assertEqual(cursor.get_value(), value_populate(cursor, 100))

    # Verify cursor search-near operations before and after a set of
    # column-store duplicates.
    def test_search_duplicate(self):
        if self.colvar == 0:
                return

        # Populate the tree.
        simple_populate(self, self.uri, self.fmt, 105)

        # Set up deleted records before and after a set of duplicate records,
        # and make sure search/search-near returns the correct record.
        cursor = self.session.open_cursor(self.uri, None)
        for i in range(20, 100):
            cursor[key_populate(cursor, i)] = '=== IDENTICAL VALUE ==='
        for i in range(15, 25):
            cursor.set_key(key_populate(cursor, i))
            self.assertEqual(cursor.remove(), 0)
        for i in range(95, 106):
            cursor.set_key(key_populate(cursor, i))
            self.assertEqual(cursor.remove(), 0)
        cursor.close()

        # Reopen the connection, forcing it to disk and moving the records to
        # an on-page format.
        self.reopen_conn()

        # Open a cursor.
        cursor = self.session.open_cursor(self.uri, None)

        # Search-near for a record in the deleted set before the duplicate set,
        # which should succeed, returning the first record in the duplicate set.
        cursor.set_key(key_populate(cursor, 18))
        self.assertEqual(cursor.search_near(), 1)
        self.assertEqual(cursor.get_key(), key_populate(cursor, 25))

        # Search-near for a record in the deleted set after the duplicate set,
        # which should succeed, returning the last record in the duplicate set.
        cursor.set_key(key_populate(cursor, 98))
        self.assertEqual(cursor.search_near(), -1)
        self.assertEqual(cursor.get_key(), key_populate(cursor, 94))

    # Verify cursor search and search-near operations on a file with a set of
    # on-page visible records, and a set of insert-list invisible records.
    def test_search_invisible_one(self):
        # Populate the tree.
        simple_populate(self, self.uri, self.fmt, 100)

        # Delete a range of records.
        for i in range(5, 10):
            cursor = self.session.open_cursor(self.uri, None)
            cursor.set_key(key_populate(cursor, i))
            self.assertEqual(cursor.remove(), 0)

        # Reopen the connection, forcing it to disk and moving the records to
        # an on-page format.
        self.reopen_conn()

        # Add updates to the existing records (in both the deleted an undeleted
        # range), as well as some new records after the end. Put the updates in
        # a separate transaction so they're invisible to another cursor.
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri, None)
        for i in range(5, 10):
            cursor[key_populate(cursor, i)] = value_populate(cursor, i + 1000)
        for i in range(30, 40):
            cursor[key_populate(cursor, i)] = value_populate(cursor, i + 1000)
        for i in range(100, 140):
            cursor[key_populate(cursor, i)] = value_populate(cursor, i + 1000)

        # Open a separate session and cursor.
        s = self.conn.open_session()
        cursor = s.open_cursor(self.uri, None)

        # Search for an existing record in the deleted range, should not find
        # it.
        for i in range(5, 10):
            cursor.set_key(key_populate(cursor, i))
            if self.empty:
                # Fixed-length column-store rows always exist.
                self.assertEqual(cursor.search(), 0)
                self.assertEqual(cursor.get_key(), i)
                self.assertEqual(cursor.get_value(), 0)
            else:
                self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)

        # Search for an existing record in the updated range, should see the
        # original value.
        for i in range(30, 40):
            cursor.set_key(key_populate(cursor, i))
            self.assertEqual(cursor.search(), 0)
            self.assertEqual(cursor.get_key(), key_populate(cursor, i))

        # Search for a added record, should not find it.
        for i in range(120, 130):
            cursor.set_key(key_populate(cursor, i))
            if self.empty:
                # Invisible updates to fixed-length column-store objects are
                # invisible to the reader, but the fact that they exist past
                # the end of the initial records causes the instantiation of
                # empty records: confirm successful return of an empty row.
                self.assertEqual(cursor.search(), 0)
                self.assertEqual(cursor.get_key(), i)
                self.assertEqual(cursor.get_value(), 0)
            else:
                # Otherwise, we should not find any matching records.
                self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)

        # Search-near for an existing record in the deleted range, should find
        # the next largest record. (This depends on the implementation behavior
        # which currently includes a bias to prefix search.)
        for i in range(5, 10):
            cursor.set_key(key_populate(cursor, i))
            if self.empty:
                # Fixed-length column-store rows always exist.
                self.assertEqual(cursor.search_near(), 0)
                self.assertEqual(cursor.get_key(), i)
                self.assertEqual(cursor.get_value(), 0)
            else:
                self.assertEqual(cursor.search_near(), 1)
                self.assertEqual(cursor.get_key(), key_populate(cursor, 10))

        # Search-near for an existing record in the updated range, should see
        # the original value.
        for i in range(30, 40):
            cursor.set_key(key_populate(cursor, i))
            self.assertEqual(cursor.search_near(), 0)
            self.assertEqual(cursor.get_key(), key_populate(cursor, i))

        # Search-near for an added record, should find the previous largest
        # record.
        for i in range(120, 130):
            cursor.set_key(key_populate(cursor, i))
            if self.empty:
                # Invisible updates to fixed-length column-store objects are
                # invisible to the reader, but the fact that they exist past
                # the end of the initial records causes the instantiation of
                # empty records: confirm successful return of an empty row.
                self.assertEqual(cursor.search_near(), 0)
                self.assertEqual(cursor.get_key(), i)
                self.assertEqual(cursor.get_value(), 0)
            else:
                self.assertEqual(cursor.search_near(), -1)
                self.assertEqual(cursor.get_key(), key_populate(cursor, 100))

    # Verify cursor search and search-near operations on a file with a set of
    # on-page visible records, a set of insert-list visible records, and a set
    # of insert-list invisible records.  (The reason I'm adding this slightly
    # different test is because I want to confirm that if search positions the
    # the cursor in the insert list with a set of invisible updates, the right
    # fallback happens, whether the correct position is in the page slots or
    # the insert list.)
    def test_search_invisible_two(self):
        # Populate the tree and reopen the connection, forcing it to disk
        # and moving the records to an on-page format.
        simple_populate(self, self.uri, self.fmt, 100)
        self.reopen_conn()

        # Add some additional visible records.
        cursor = self.session.open_cursor(self.uri, None)
        for i in range(100, 120):
            cursor[key_populate(cursor, i)] = value_populate(cursor, i)
        cursor.close()

        # Begin a transaction, and add some additional records.
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri, None)
        for i in range(120, 140):
            cursor[key_populate(cursor, i)] = value_populate(cursor, i)

        # Open a separate session and cursor.
        s = self.conn.open_session()
        cursor = s.open_cursor(self.uri, None)

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
