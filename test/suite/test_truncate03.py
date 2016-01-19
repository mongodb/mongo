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
# test_truncate03.py
#       session level operations on tables

import wiredtiger, wttest
from helper import key_populate, simple_populate, value_populate

# A standalone test case that exercises address-deleted cells.
class test_truncate_address_deleted(wttest.WiredTigerTestCase):
    uri = 'file:test_truncate'

    # Use a small page size and lots of keys because we want to create lots
    # of individual pages in the file.
    nentries = 10000
    config = 'allocation_size=512,' +\
        'leaf_page_max=512,value_format=S,key_format=S'

    # address_deleted routine:
    #   Create an object that has a bunch of address-deleted cells on disk.
    #   Recover the object, and turn the address-deleted cells into free pages.
    def address_deleted(self):
        # Create the object, force it to disk, and verify the object.
        simple_populate(self, self.uri, self.config, self.nentries)
        self.reopen_conn()
        self.session.verify(self.uri)

        # Create a new session and start a transaction to force the upcoming
        # checkpoint operation to write address-deleted cells to disk.
        tmp_session = self.conn.open_session(None)
        tmp_session.begin_transaction("isolation=snapshot")

        # Truncate a big range of rows; the leaf pages aren't in memory, so
        # leaf page references will be deleted without being read.
        start = self.session.open_cursor(self.uri, None)
        start.set_key(key_populate(start, 10))
        end = self.session.open_cursor(self.uri, None)
        end.set_key(key_populate(end, self.nentries - 10))
        self.session.truncate(None, start, end, None)
        self.assertEqual(start.close(), 0)
        self.assertEqual(end.close(), 0)

        # Checkpoint, forcing address-deleted cells to be written.
        self.session.checkpoint()

        # Crash/reopen the connection and verify the object.
        self.reopen_conn()
        self.session.verify(self.uri)

        # Open a cursor and update a record (to dirty the tree, else we won't
        # mark pages with address-deleted cells dirty), then walk the tree so
        # we get a good look at all the internal pages and the address-deleted
        # cells.
        cursor = self.session.open_cursor(self.uri, None)
        cursor.set_key(key_populate(cursor, 5))
        cursor.set_value("changed value")
        self.assertEqual(cursor.update(), 0)
        cursor.reset()
        for key,val in cursor:
            continue
        self.assertEqual(cursor.close(), 0)

        # Checkpoint, freeing the pages.
        self.session.checkpoint()

    # Test object creation, recovery, and conversion of address-deleted cells
    # into free pages.
    def test_truncate_address_deleted_free(self):
        # Create the object on disk.
        self.address_deleted()

        # That's all just verify that worked.
        self.session.verify(self.uri)

    # Test object creation, recovery, and conversion of address-deleted cells
    # into free pages, but instead of verifying the final object, instantiate
    # empty pages by a reader after the underlying leaf pages are removed.
    def test_truncate_address_deleted_empty_page(self):
        # Create the object on disk.
        self.address_deleted()

        # Open a cursor and update pages in the middle of the range, forcing
        # creation of empty pages (once the underlying leaf page is freed, we
        # have to magic up a page if we need it).  Confirm we can read/write
        # the value as well as write the page and get it back.
        cursor = self.session.open_cursor(self.uri, None)
        for i in range(3000, 7000, 137):
            k = key_populate(cursor, i)
            v = 'changed value: ' + str(i)
            cursor[k] = v
        for i in range(3000, 7000, 137):
            k = key_populate(cursor, i)
            v = 'changed value: ' + str(i)
            cursor.set_key(k)
            self.assertEqual(cursor.search(), 0)
            self.assertEqual(cursor.get_value(), v)
        self.assertEqual(cursor.close(), 0)

        self.session.checkpoint()
        self.reopen_conn()
        self.session.verify(self.uri)

        cursor = self.session.open_cursor(self.uri, None)
        for i in range(3000, 7000, 137):
            k = key_populate(cursor, i)
            v = 'changed value: ' + str(i)
            cursor.set_key(k)
            self.assertEqual(cursor.search(), 0)
            self.assertEqual(cursor.get_value(), v)
        self.assertEqual(cursor.close(), 0)


if __name__ == '__main__':
    wttest.run()
