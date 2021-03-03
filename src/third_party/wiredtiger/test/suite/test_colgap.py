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
from wtdataset import SimpleDataSet, simple_key, simple_value
from wtscenario import make_scenarios

# test_colgap.py
#    Test variable-length column-store gap performance.
class test_column_store_gap(wttest.WiredTigerTestCase):
    nentries = 13

    # Cursor forward
    def forward(self, cursor, expected):
        cursor.reset()
        i = 0
        while True:
            if cursor.next() != 0:
                break
            self.assertEqual(cursor.get_key(), expected[i])
            i += 1
        self.assertEqual(i, self.nentries)

    # Cursor backward
    def backward(self, cursor, expected):
        cursor.reset()
        i = 0
        while True:
            if cursor.prev() != 0:
                break
            self.assertEqual(cursor.get_key(), expected[i])
            i += 1
        self.assertEqual(i, self.nentries)

    # Create a variable-length column-store table with really big gaps in the
    # namespace. If this runs in less-than-glacial time, it's working.
    def test_column_store_gap(self):
        uri = 'table:gap'
        # Initially just create tables.
        ds = SimpleDataSet(self, uri, 0, key_format='r')
        ds.populate()
        cursor = self.session.open_cursor(uri, None, None)
        self.nentries = 0

        # Create a column-store table with large gaps in the name-space.
        v = [ 1000, 2000000000000, 30000000000000 ]
        for i in v:
            cursor[ds.key(i)] = ds.value(i)
            self.nentries += 1

        # In-memory cursor forward, backward.
        self.forward(cursor, v)
        self.backward(cursor, list(reversed(v)))

        self.reopen_conn()
        cursor = self.session.open_cursor(uri, None, None)

        # Disk page cursor forward, backward.
        self.forward(cursor, v)
        self.backward(cursor, list(reversed(v)))

    def test_column_store_gap_traverse(self):
        uri = 'table:gap'
        # Initially just create tables.
        ds = SimpleDataSet(self, uri, 0, key_format='r')
        ds.populate()
        cursor = self.session.open_cursor(uri, None, None)
        self.nentries = 0

        # Create a column store with key gaps. The particular values aren't
        # important, we just want some gaps.
        v = [ 1000, 1001, 2000, 2001]
        for i in v:
            cursor[ds.key(i)] = ds.value(i)
            self.nentries += 1

        # In-memory cursor forward, backward.
        self.forward(cursor, v)
        self.backward(cursor, list(reversed(v)))

        self.reopen_conn()
        cursor = self.session.open_cursor(uri, None, None)

        # Disk page cursor forward, backward.
        self.forward(cursor, v)
        self.backward(cursor, list(reversed(v)))

        # Insert some new records, so there are in-memory updates and an
        # on disk image. Put them in the middle of the existing values
        # so the traversal walks to them.
        v2 = [ 1500, 1501 ]
        for i in v2:
            cursor[ds.key(i)] = ds.value(i)
            self.nentries += 1

        # Tell the validation what to expect.
        v = [ 1000, 1001, 1500, 1501, 2000, 2001 ]
        self.forward(cursor, v)
        self.backward(cursor, list(reversed(v)))

# Basic testing of variable-length column-store with big records.
class test_colmax(wttest.WiredTigerTestCase):
    name = 'test_colmax'

    types = [
        ('file', dict(type='file:')),
        ('table', dict(type='table:'))
    ]
    valfmt = [
        ('integer', dict(valfmt='i')),
        ('string', dict(valfmt='S')),
    ]
    record_number = [
        ('big', dict(recno=18446744073709551606)),
        ('max', dict(recno=18446744073709551615)),
    ]
    bulk = [
        ('bulk', dict(bulk=1)),
        ('not-bulk', dict(bulk=0)),
    ]
    reopen = [
        ('reopen', dict(reopen=1)),
        ('not-reopen', dict(reopen=0)),
    ]
    single = [
        ('single', dict(single=1)),
        ('not-single', dict(single=0)),
    ]

    scenarios = make_scenarios(\
        types, valfmt, record_number, bulk, reopen, single)

    # Test that variable-length column-store correctly/efficiently handles big
    # records (if it's not efficient, we'll just hang).
    def test_colmax_op(self):
        recno = self.recno

        uri = self.type + self.name
        self.session.create(uri, 'key_format=r' +',value_format=' + self.valfmt)

        # Insert a big record with/without a bulk cursor.
        bulk_config = ""
        if self.bulk:
            bulk_config = "bulk"
        cursor = self.session.open_cursor(uri, None, bulk_config)

        # Optionally make the big record the only record in the table.
        if not self.single:
            for i in range(1, 723):
                cursor[simple_key(cursor, i)] = simple_value(cursor, i)

        # Confirm searching past the end of the table works.
        if not self.bulk:
            cursor.set_key(simple_key(cursor, recno))
            self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)

        # Insert the big record.
        cursor[simple_key(cursor, recno)] = simple_value(cursor, recno)

        # Optionally flush to disk; re-open the cursor as necessary.
        if self.bulk or self.reopen:
            cursor.close()
        if self.reopen == 1:
            self.reopen_conn()
        if self.bulk or self.reopen:
            cursor = self.session.open_cursor(uri, None, None)

        # Search for the large record.
        cursor.set_key(simple_key(cursor, recno))
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_value(), simple_value(cursor, recno))

        # Update it.
        cursor[simple_key(cursor, recno)] = simple_value(cursor, 37)
        cursor.set_key(simple_key(cursor, recno))
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_value(), simple_value(cursor, 37))

        # Remove it.
        cursor.set_key(simple_key(cursor, recno))
        self.assertEqual(cursor.remove(), 0)
        cursor.set_key(simple_key(cursor, recno))
        self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)

if __name__ == '__main__':
    wttest.run()
