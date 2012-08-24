#!/usr/bin/env python
#
# Public Domain 2008-2012 WiredTiger, Inc.
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
# test_truncate01.py
#       session level operations on tables
#

import wiredtiger, wttest
from helper import confirm_empty,\
    complex_populate, key_populate, simple_populate, value_populate
from wtscenario import multiply_scenarios, number_scenarios

# Test session.truncate
#       Simple, one-off tests.
class test_truncate_standalone(wttest.WiredTigerTestCase):

    # Test truncation without URI or cursors specified, or with a URI and
    # either cursor specified, expect errors.
    def test_truncate_bad_args(self):
        uri = 'file:xxx'
        simple_populate(self, uri, 'key_format=S', 10)
        msg = '/either a URI or start/stop cursors/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.truncate(None, None, None, None), msg)
        cursor = self.session.open_cursor(uri, None)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.truncate(uri, cursor, None, None), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.truncate(uri, None, cursor, None), msg)


# Test truncation of an object using its URI.
class test_truncate_uri(wttest.WiredTigerTestCase):
    name = 'test_truncate'
    scenarios = [
        ('file', dict(type='file:')),
        ('table', dict(type='table:'))
    ]

    # Populate an object, truncate it by URI, and confirm it's empty.
    def test_truncate_uri(self):
        uri = self.type + self.name

        # A simple, one-file file or table object.
        simple_populate(self, uri, 'key_format=S', 100)
        self.session.truncate(uri, None, None, None)
        confirm_empty(self, uri)
        self.session.drop(uri, None)

        if self.type == "table:":
            complex_populate(self, uri, 'key_format=S', 100)
            self.session.truncate(uri, None, None, None)
            confirm_empty(self, uri)
            self.session.drop(uri, None)


# XXX
#	-- test with appends
#       -- test with re-open and subsequent appends (half-and-half)
# Test session.truncate.
class test_truncate(wttest.WiredTigerTestCase):
    name = 'test_truncate'

    # Use a small page size because we want to create lots of pages.
    # The underlying table routines don't easily support 8t value types, limit
    # those tests to file objects.
    types = [
        ('file', dict(config='leaf_page_max=512,key_format=',type='file:')),
        ('file', dict(config=\
            'leaf_page_max=512,value_format=8t,key_format=',type='file:')),
        ('table', dict(config='leaf_page_max=512,key_format=',type='table:')),
    ]
    keyfmt = [
        ('integer', dict(keyfmt='i')),
        ('recno', dict(keyfmt='r')),
        ('string', dict(keyfmt='S')),
    ]
    image = [
        ('in-memory', dict(reopen=False)),
        ('on-disk', dict(reopen=True)),
    ]
    size = [
        ('small', dict(nentries=100,skip=7)),
        ('big', dict(nentries=1000,skip=37)),
    ]
    search = [
        ('searchtrue', dict(search=True)),
        ('searchfalse', dict(search=False)),
    ]

    scenarios = number_scenarios(
        multiply_scenarios('.', types, keyfmt, image, size, search))

    # Set a cursor and optionally search for the item.
    def initCursor(self, uri, key):
        if key == -1:
            return None
        cursor = self.session.open_cursor(uri, None)
        cursor.set_key(key_populate(cursor, key))

        # Test scenarios where we fully instantiate a cursor as well as where we
        # only set the key.
        if self.search:
            self.assertEqual(cursor.search(), 0)
            self.assertEqual(cursor.get_key(), key_populate(cursor, key))

        return cursor

    # Return the first/last valid item in the object (valid means skipping
    # over deleted fixed-length rows).
    def getKey(self, cursor, nextprev):
        if cursor.value_format == '8t':
            while (True):
                self.assertEqual(nextprev(), 0)
                if cursor.get_value() != 0:
                        break;
        else:
            self.assertEqual(nextprev(), 0)
        return cursor.get_key()
    def getFirstKey(self, cursor):
        return self.getKey(cursor, cursor.next)
    def getLastKey(self, cursor):
        return self.getKey(cursor, cursor.prev)

    # Truncate a range using cursors, and check the results.
    def truncateRangeAndCheck(self, uri, begin, end):
        self.pr('truncateRangeAndCheck: ' + str(begin) + ',' + str(end))
        cur1 = self.initCursor(uri, begin)
        cur2 = self.initCursor(uri, end)
        self.session.truncate(None, cur1, cur2, None)
        if not cur1:
            begin = 1
        else:
            cur1.close()
        if not cur2:
            end = self.nentries - 1
        else:
            cur2.close()

        # If the object is empty, confirm that, otherwise test the first and
        # last keys are the ones before/after the truncated range.
        cursor = self.session.open_cursor(uri, None)
        if begin == 1 and end == self.nentries - 1:
            confirm_empty(self, uri)
        else:
            key = self.getFirstKey(cursor)
            if begin == 1:
                self.assertEqual(key, key_populate(cursor, end + 1))
            else:
                self.assertEqual(key, key_populate(cursor, 1))

            self.assertEqual(cursor.reset(), 0)

            key = self.getLastKey(cursor)
            if end == self.nentries - 1:
                self.assertEqual(key, key_populate(cursor, begin - 1))
            else:
                self.assertEqual(key, key_populate(cursor, self.nentries - 1))
        cursor.close()

    # Test truncation using cursors.
    #
    # For begin and end: -1 means pass None for the cursor arg to truncate.
    # An integer N, with 1 <= N < self.nentries, passes a cursor positioned
    # at that element.
    def test_truncate_cursor(self):
        uri = self.type + self.name
        list = [
            (-1, self.nentries - 1),            # begin to end, begin = None
            (1, -1),                            # begin to end, end = None
            (1, self.nentries - 1),             # begin to end
            (-1, self.nentries - self.skip),    # begin to middle, begin = None
            (1, self.nentries - self.skip),     # begin to middle
            (self.skip, -1),                    # middle to end, end = None
            (self.skip, self.nentries - 1),     # middle to end
            (self.skip,                         # middle to different middle
                self.nentries - self.skip),
            (1, 1),                             # begin to begin
            (self.nentries - 1,                 # end to end
                self.nentries -1),
            (self.skip, self.skip)              # middle to same middle
            ]

        # A simple, one-file file or table object.
        for begin,end in list:
            # Populate the object.
            simple_populate(self, uri, self.config + self.keyfmt, self.nentries)

            # Optionally close and re-open the object to get a disk image.
            if self.reopen:
                self.reopen_conn()

            self.truncateRangeAndCheck(uri, begin, end)
            self.session.drop(uri, None)

        # A complex, multi-file table object.
        if self.type == "table:":
            for begin,end in list:
                complex_populate(
                    self, uri, self.config + self.keyfmt, self.nentries)
                if self.reopen:
                    self.reopen_conn()
                self.truncateRangeAndCheck(uri, begin, end)
                self.session.drop(uri, None)


if __name__ == '__main__':
    wttest.run()
