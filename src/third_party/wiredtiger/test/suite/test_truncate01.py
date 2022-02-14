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
# truncate
# [END_TAGS]
#
# test_truncate01.py
#       session level operations on tables
#

import wiredtiger, wttest
from helper import confirm_empty
from wtdataset import SimpleDataSet, ComplexDataSet, simple_key
from wtscenario import make_scenarios

# Test truncation arguments.
class test_truncate_arguments(wttest.WiredTigerTestCase):
    name = 'test_truncate'

    scenarios = make_scenarios([
        ('file', dict(type='file:')),
        ('table', dict(type='table:'))
    ])

    # Test truncation without URI or cursors specified, or with a URI and
    # either cursor specified, expect errors.
    def test_truncate_bad_args(self):
        uri = self.type + self.name
        SimpleDataSet(self, uri, 100).populate()
        msg = '/either a URI or start/stop cursors/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.truncate(None, None, None, None), msg)
        cursor = self.session.open_cursor(uri, None)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.truncate(uri, cursor, None, None), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.truncate(uri, None, cursor, None), msg)

    # Test truncation of cursors where no key is set, expect errors.
    def test_truncate_cursor_notset(self):
        uri = self.type + self.name
        msg = '/requires key be set/'

        ds = SimpleDataSet(self, uri, 100)
        ds.populate()

        c1 = self.session.open_cursor(uri, None)
        c2 = self.session.open_cursor(uri, None)
        c2.set_key(ds.key(10))
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.truncate(None, c1, c2, None), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.truncate(None, c2, c1, None), msg)
        c1.close()
        c2.close()

# Test truncation of an object using its URI.
class test_truncate_uri(wttest.WiredTigerTestCase):
    name = 'test_truncate'
    scenarios = make_scenarios([
        ('file', dict(type='file:')),
        ('table', dict(type='table:'))
    ])

    # Populate an object, truncate it by URI, and confirm it's empty.
    def test_truncate_uri(self):
        uri = self.type + self.name

        # A simple, one-file file or table object.
        SimpleDataSet(self, uri, 100).populate()
        self.session.truncate(uri, None, None, None)
        confirm_empty(self, uri)
        self.dropUntilSuccess(self.session, uri)

        if self.type == "table:":
            ComplexDataSet(self, uri, 100).populate()
            self.session.truncate(uri, None, None, None)
            confirm_empty(self, uri)
            self.dropUntilSuccess(self.session, uri)

# Test truncation of cursors in an illegal order.
class test_truncate_cursor_order(wttest.WiredTigerTestCase):
    name = 'test_truncate'

    types = [
        ('file', dict(type='file:')),
        ('table', dict(type='table:'))
    ]
    keyfmt = [
        ('integer', dict(keyfmt='i')),
        ('recno', dict(keyfmt='r')),
        ('string', dict(keyfmt='S')),
    ]
    scenarios = make_scenarios(types, keyfmt)

    # Test an illegal order, then confirm that equal cursors works.
    def test_truncate_cursor_order(self):
        uri = self.type + self.name
        ds = SimpleDataSet(self, uri, 100, key_format=self.keyfmt)
        ds.populate()
        c1 = self.session.open_cursor(uri, None)
        c2 = self.session.open_cursor(uri, None)

        c1.set_key(ds.key(20))
        c2.set_key(ds.key(10))
        msg = '/the start cursor position is after the stop cursor position/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.truncate(None, c1, c2, None), msg)
        c1.set_key(ds.key(10))
        c2.set_key(ds.key(20))
        self.session.truncate(None, c1, c2, None)

# Test truncation of cursors past the end of the object.
class test_truncate_cursor_end(wttest.WiredTigerTestCase):
    name = 'test_truncate'

    types = [
        ('file', dict(type='file:')),
        ('table', dict(type='table:'))
    ]
    keyfmt = [
        ('integer', dict(keyfmt='i')),
        ('recno', dict(keyfmt='r')),
        ('string', dict(keyfmt='S')),
    ]
    scenarios = make_scenarios(types, keyfmt)

    # Test truncation of cursors past the end of the object.
    def test_truncate_cursor_order(self):
        uri = self.type + self.name

        # A simple, one-file file or table object.
        ds = SimpleDataSet(self, uri, 100, key_format=self.keyfmt)
        ds.populate()
        c1 = self.session.open_cursor(uri, None)
        c1.set_key(ds.key(1000))
        c2 = self.session.open_cursor(uri, None)
        c2.set_key(ds.key(2000))
        self.session.truncate(None, c1, c2, None)
        self.assertEqual(c1.close(), 0)
        self.assertEqual(c2.close(), 0)
        self.dropUntilSuccess(self.session, uri)

        if self.type == "table:":
            ds = ComplexDataSet(self, uri, 100, key_format=self.keyfmt)
            ds.populate()
            c1 = self.session.open_cursor(uri, None)
            c1.set_key(ds.key(1000))
            c2 = self.session.open_cursor(uri, None)
            c2.set_key(ds.key(2000))
            self.session.truncate(None, c1, c2, None)
            self.assertEqual(c1.close(), 0)
            self.assertEqual(c2.close(), 0)
            self.dropUntilSuccess(self.session, uri)

# Test truncation of empty objects.
class test_truncate_empty(wttest.WiredTigerTestCase):
    name = 'test_truncate_empty'

    types = [
        ('file', dict(type='file:')),
        ('table', dict(type='table:'))
    ]
    keyfmt = [
        ('integer', dict(keyfmt='i')),
        ('recno', dict(keyfmt='r')),
        ('string', dict(keyfmt='S')),
    ]
    scenarios = make_scenarios(types, keyfmt)

    # Test truncation of empty objects using a cursor
    def test_truncate_empty_cursor(self):
        uri = self.type + self.name
        self.session.create(uri,
            ',key_format=' + self.keyfmt + ',value_format=S')
        c1 = self.session.open_cursor(uri, None)
        c1.set_key(simple_key(c1, 1000))
        c2 = self.session.open_cursor(uri, None)
        c2.set_key(simple_key(c2, 2000))
        self.assertEqual(self.session.truncate(None, c1, c2, None), 0)

    # Test truncation of empty objects using a URI
    def test_truncate_empty_uri(self):
        uri = self.type + self.name
        self.session.create(uri,
            ',key_format=' + self.keyfmt + ',value_format=S')
        self.assertEqual(self.session.truncate(uri, None, None, None), 0)

# Test session.truncate.
class test_truncate_cursor(wttest.WiredTigerTestCase):
    name = 'test_truncate'

    # Use a small page size because we want to create lots of pages.
    # The underlying table routines don't easily support 8t value types, limit
    # those tests to file objects.
    types = [
        ('file', dict(type='file:', valuefmt='S',
            config='allocation_size=512,leaf_page_max=512', P=0.25)),
        ('file8t', dict(type='file:', valuefmt='8t',
            config='allocation_size=512,leaf_page_max=512', P=0.25)),
        ('table', dict(type='table:', valuefmt='S',
            config='allocation_size=512,leaf_page_max=512', P=0.5)),
    ]
    keyfmt = [
        ('integer', dict(keyfmt='i')),
        ('recno', dict(keyfmt='r')),
        ('string', dict(keyfmt='S')),
    ]
    reopen = [
        ('in-memory', dict(reopen=False)),
        ('on-disk', dict(reopen=True)),
    ]
    size = [
        ('small', dict(nentries=100,skip=7)),
        ('big', dict(nentries=1000,skip=37)),
    ]

    scenarios = make_scenarios(types, keyfmt, size, reopen,
        prune=10, prunelong=1000)

    # Set a cursor key.
    def cursorKey(self, ds, uri, key):
        if key == -1:
            return None
        cursor = self.session.open_cursor(uri, None)
        cursor.set_key(ds.key(key))
        return cursor

    # Truncate a range using cursors, and check the results.
    def truncateRangeAndCheck(self, ds, uri, begin, end, expected):
        self.pr('truncateRangeAndCheck: ' + str(begin) + ',' + str(end))
        cur1 = self.cursorKey(ds, uri, begin)
        cur2 = self.cursorKey(ds, uri, end)
        self.session.truncate(None, cur1, cur2, None)
        if not cur1:
            begin = 1
        else:
            cur1.close()
        if not cur2:
            end = self.nentries
        else:
            cur2.close()

        # If the object should be empty, confirm that.
        if begin == 1 and end == self.nentries:
            confirm_empty(self, uri)
            return

        # Check the expected values against the object.
        cursor = self.session.open_cursor(uri, None)
        for i in range(begin, end + 1):
            expected[ds.key(i)] = [0]
        for k, v in expected.items():
            cursor.set_key(k)
            if v == [0] and \
              cursor.key_format == 'r' and cursor.value_format == '8t':
                cursor.search()
                self.assertEqual(cursor.get_values(), [0])
            elif v == [0]:
                self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
            else:
                cursor.search()
                self.assertEqual(cursor.get_values(), v)
        cursor.close()

    # Test truncation of files and simple tables using cursors.
    def test_truncate_simple(self):
        uri = self.type + self.name

        # layout:
        #    the number of initial skipped records
        #    the number of initial inserted records
        #    the number of trailing skipped records
        #    the number of trailing inserted records
        layout = [
            # simple set of rows
            (0, 0, 0, 0),

            # trailing append list, no delete point overlap
            (0, 0, 0, self.skip - 3),

            # trailing append list, delete point overlap
            (0, 0, 0, self.skip + 3),

            # trailing skipped list, no delete point overlap
            (0, 0, self.skip - 3, 1),

            # trailing skipped list, delete point overlap
            (0, 0, self.skip + 3, 1),

            # leading insert list, no delete point overlap
            (0, self.skip - 3, 0, 0),

            # leading insert list, delete point overlap
            (0, self.skip + 3, 0, 0),

            # leading skipped list, no delete point overlap
            (self.skip - 3, 1, 0, 0),

            # leading skipped list, delete point overlap
            (self.skip + 3, 1, 0, 0),
        ]

        # list: truncation patterns applied on top of the layout.
        #
        # begin and end: -1 means pass None for the cursor arg to truncate.  An
        # integer N, with 1 <= N < self.nentries, truncates from/to a cursor
        # positioned at that row.
        list = [
            (-1, self.nentries),                # begin to end, begin = None
            (1, -1),                            # begin to end, end = None
            (1, self.nentries),                 # begin to end
            (-1, self.nentries - self.skip),    # begin to middle, begin = None
            (1, self.nentries - self.skip),     # begin to middle
            (self.skip, -1),                    # middle to end, end = None
            (self.skip, self.nentries),         # middle to end
            (self.skip,                         # middle to different middle
                self.nentries - self.skip),
            (1, 1),                             # begin to begin
            (self.nentries, self.nentries),     # end to end
            (self.skip, self.skip)              # middle to same middle
            ]

        # Using this data set to compare only, it doesn't create or populate.
        ds = SimpleDataSet(self, uri, 0, key_format=self.keyfmt,
            value_format=self.valuefmt, config=self.config)

        # Build the layout we're going to test
        total = self.nentries
        for begin_skipped,begin_insert,end_skipped,end_insert in layout:

            # skipped records require insert/append records
            if begin_skipped and not begin_insert or \
                end_skipped and not end_insert:
                raise AssertionError('test error: skipped set without insert')

            for begin,end in list:
                '''
                print '===== run:'
                print 'key:', self.keyfmt, 'begin:', begin, 'end:', end
                print 'total: ', total, \
                    'begin_skipped:', begin_skipped, \
                    'begin_insert:', begin_insert, \
                    'end_skipped:', end_skipped, \
                    'end_insert:', end_insert
                '''

                # Build a dictionary of what the object should look like for
                # later comparison
                expected = {}

                # Create the object.
                self.session.create(
                    uri, self.config + ',key_format=' + self.keyfmt +
                    ',value_format=' + self.valuefmt)

                # Insert the records that aren't skipped or inserted.
                start = begin_skipped + begin_insert
                stop = self.nentries - (end_skipped + end_insert)
                cursor = self.session.open_cursor(uri, None)
                for i in range(start + 1, stop + 1):
                    k = ds.key(i)
                    v = ds.value(i)
                    cursor[k] = v
                    expected[k] = [v]
                cursor.close()

                # Optionally close and re-open the object to get a disk image
                # instead of a big insert list.
                if self.reopen:
                    self.reopen_conn()

                # Optionally insert initial skipped records.
                cursor = self.session.open_cursor(uri, None, "overwrite")
                start = 0
                for i in range(0, begin_skipped):
                    start += 1
                    k = ds.key(start)
                    expected[k] = [0]

                # Optionally insert leading records.
                for i in range(0, begin_insert):
                    start += 1
                    k = ds.key(start)
                    v = ds.value(start)
                    cursor[k] = v
                    expected[k] = [v]

                # Optionally insert trailing skipped records.
                for i in range(0, end_skipped):
                    stop += 1
                    k = ds.key(stop)
                    expected[k] = [0]

                # Optionally insert trailing records.
                for i in range(0, end_insert):
                    stop += 1
                    k = ds.key(stop)
                    v = ds.value(stop)
                    cursor[k] = v
                    expected[k] = [v]
                cursor.close()

                self.truncateRangeAndCheck(ds, uri, begin, end, expected)
                self.dropUntilSuccess(self.session, uri)

    # Test truncation of complex tables using cursors.  We can't do the kind of
    # layout and detailed testing as we can with files, but this will at least
    # smoke-test the handling of indexes and column-groups.
    def test_truncate_complex(self):

        # We only care about tables.
        if self.type == 'file:':
                return

        uri = self.type + self.name

        # list: truncation patterns
        #
        # begin and end: -1 means pass None for the cursor arg to truncate.  An
        # integer N, with 1 <= N < self.nentries, truncates from/to a cursor
        # positioned at that row.
        list = [
            (-1, self.nentries),                # begin to end, begin = None
            (1, -1),                            # begin to end, end = None
            (1, self.nentries),                 # begin to end
            (-1, self.nentries - self.skip),    # begin to middle, begin = None
            (1, self.nentries - self.skip),     # begin to middle
            (self.skip, -1),                    # middle to end, end = None
            (self.skip, self.nentries),         # middle to end
            (self.skip,                         # middle to different middle
                self.nentries - self.skip),
            (1, 1),                             # begin to begin
            (self.nentries, self.nentries),     # end to end
            (self.skip, self.skip)              # middle to same middle
            ]

        # Build the layout we're going to test
        for begin,end in list:
            '''
            print '===== run:', uri
            print 'key:', self.keyfmt, 'begin:', begin, 'end:', end
            '''

            # Create the object.
            ds = ComplexDataSet(self, uri, self.nentries,
                config=self.config, key_format=self.keyfmt)
            ds.populate()

            # Build a dictionary of what the object should look like for
            # later comparison
            cursor = self.session.open_cursor(uri, None)
            expected = {}
            for i in range(1, self.nentries + 1):
                expected[ds.key(i)] = ds.comparable_value(i)
            cursor.close()

            # Optionally close and re-open the object to get a disk image
            # instead of a big insert list.
            if self.reopen:
                self.reopen_conn()

            self.truncateRangeAndCheck(ds, uri, begin, end, expected)
            self.dropUntilSuccess(self.session, uri)

if __name__ == '__main__':
    wttest.run()
