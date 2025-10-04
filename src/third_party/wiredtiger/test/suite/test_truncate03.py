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
# test_truncate03.py
#       session level operations on tables

import wttest
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# A standalone test case that exercises address-deleted cells.
class test_truncate_address_deleted(wttest.WiredTigerTestCase):
    uri = 'file:test_truncate'

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('row_integer', dict(key_format='i', value_format='S')),
    ]

    scenarios = make_scenarios(format_values)

    # Use a small page size and lots of keys because we want to create lots
    # of individual pages in the file.
    nentries = 10000
    config = 'allocation_size=512,leaf_page_max=512'

    # address_deleted routine:
    #   Create an object that has a bunch of address-deleted cells on disk.
    #   Recover the object, and turn the address-deleted cells into free pages.
    def address_deleted(self):
        # Create the object, force it to disk, and verify the object.
        ds = SimpleDataSet(self, self.uri, self.nentries,
            key_format=self.key_format, value_format = self.value_format, config=self.config)
        ds.populate()
        self.reopen_conn()
        self.session.verify(self.uri)

        if self.value_format == '8t':
            changed_value = 0xfe
        else:
            changed_value = "changed value"

        # Create a new session and start a transaction to force the upcoming
        # checkpoint operation to write address-deleted cells to disk.
        tmp_session = self.conn.open_session(None)
        tmp_session.begin_transaction()

        # Truncate a big range of rows; the leaf pages aren't in memory, so
        # leaf page references will be deleted without being read.
        start = ds.open_cursor(self.uri, None)
        start.set_key(ds.key(10))
        end = ds.open_cursor(self.uri, None)
        end.set_key(ds.key(self.nentries - 10))
        ds.truncate(None, start, end, None)
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
        cursor = ds.open_cursor(self.uri, None)
        cursor.set_key(ds.key(5))
        cursor.set_value(changed_value)
        self.assertEqual(cursor.update(), 0)
        cursor.reset()
        for key,val in cursor:
            continue
        self.assertEqual(cursor.close(), 0)

        # Checkpoint, freeing the pages.
        self.session.checkpoint()
        return ds

    # Test object creation, recovery, and conversion of address-deleted cells
    # into free pages.
    def test_truncate_address_deleted_free(self):
        # Create the object on disk.
        ds = self.address_deleted()

        # That's all just verify that worked; eviction can re-dirty the cache and cause verify to
        # fail, checkpoint until verify succeeds.
        while True:
            if not self.raisesBusy(lambda: self.session.verify(self.uri)):
                break
            self.session.checkpoint()

    # Test object creation, recovery, and conversion of address-deleted cells
    # into free pages, but instead of verifying the final object, instantiate
    # empty pages by a reader after the underlying leaf pages are removed.
    def test_truncate_address_deleted_empty_page(self):
        # Create the object on disk.
        ds = self.address_deleted()

        # Open a cursor and update pages in the middle of the range, forcing
        # creation of empty pages (once the underlying leaf page is freed, we
        # have to magic up a page if we need it).  Confirm we can read/write
        # the value as well as write the page and get it back.
        cursor = ds.open_cursor(self.uri, None)
        for i in range(3000, 7000, 137):
            k = ds.key(i)
            if self.value_format == '8t':
                v = ds.value(i) + 37
            else:
                v = 'changed value: ' + str(i)
            cursor[k] = v
        for i in range(3000, 7000, 137):
            k = ds.key(i)
            if self.value_format == '8t':
                v = ds.value(i) + 37
            else:
                v = 'changed value: ' + str(i)
            cursor.set_key(k)
            self.assertEqual(cursor.search(), 0)
            self.assertEqual(cursor.get_value(), v)
        self.assertEqual(cursor.close(), 0)

        self.session.checkpoint()
        self.reopen_conn()
        self.session.verify(self.uri)

        cursor = ds.open_cursor(self.uri, None)
        for i in range(3000, 7000, 137):
            k = ds.key(i)
            if self.value_format == '8t':
                v = ds.value(i) + 37
            else:
                v = 'changed value: ' + str(i)
            cursor.set_key(k)
            self.assertEqual(cursor.search(), 0)
            self.assertEqual(cursor.get_value(), v)
        self.assertEqual(cursor.close(), 0)
