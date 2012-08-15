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
# test_truncate.py
#       session level operations on tables
#

import os, time
import wiredtiger, wttest
from helper import confirm_empty,\
    complex_populate, key_populate, simple_populate

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
        cursor = self.session.open_cursor(uri, None, None)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.truncate(uri, cursor, None, None), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.truncate(uri, None, cursor, None), msg)


# Test session.truncate.
#       Scenarios.
class test_truncate(wttest.WiredTigerTestCase):
    name = 'test_truncate'

    # Use a small page size because we want to create lots of pages in the file.
    config = 'leaf_page_max=1024,key_format='

    scenarios = [
        ('file-col-small', dict(type='file:',fmt='r',nentries=100,skip=5)),
        ('file-row-small', dict(type='file:',fmt='S',nentries=100,skip=5)),
        ('table-col-small', dict(type='table:',fmt='i',nentries=100,skip=5)),
        ('table-row-small', dict(type='table:',fmt='S',nentries=100,skip=5)),
        ('file-col-big', dict(type='file:',fmt='i',nentries=10000,skip=737)),
        ('file-row-big', dict(type='file:',fmt='S',nentries=10000,skip=737)),
        ('table-col-big', dict(type='table:',fmt='i',nentries=10000,skip=737)),
        ('table-row-big', dict(type='table:',fmt='S',nentries=10000,skip=737))
        ]

    # Test truncation of an object using its URI.
    def test_truncate(self):
        uri = self.type + self.name
        simple_populate(self, uri, self.config + self.fmt, self.nentries)
        self.session.truncate(uri, None, None, None)
        confirm_empty(self, uri)
        self.session.drop(uri, None)

        if self.type == "table:":
            complex_populate(self, uri, self.config + self.fmt, self.nentries)
            self.session.truncate(uri, None, None, None)
            confirm_empty(self, uri)
            self.session.drop(uri, None)

    def initCursor(self, uri, key):
        if key == -1:
            return None
        cursor = self.session.open_cursor(uri, None, None)
        cursor.set_key(key_populate(self.fmt, key))

        # Test scenarios where we fully instantiate a cursor as well as where we
        # only set the key.  If it's a small run, do a search which builds out
        # the cursor, otherwise, do nothing.
        if self.skip == 5:
            self.assertEqual(cursor.search(), 0)
            self.assertEqual(cursor.get_key(), key_populate(self.fmt, key))

        return cursor

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
        cursor = self.session.open_cursor(uri, None, None)
        if begin == 1 and end == self.nentries - 1:
            confirm_empty(self, uri)
        else:
            self.assertEqual(cursor.next(), 0)
            key = cursor.get_key()
            if begin == 1:
                self.assertEqual(key, key_populate(self.fmt, end + 1))
            else:
                self.assertEqual(key, key_populate(self.fmt, 1))
            self.assertEqual(cursor.reset(), 0)
            self.assertEqual(cursor.prev(), 0)
            key = cursor.get_key()
            if end == self.nentries - 1:
                self.assertEqual(key, key_populate(self.fmt, begin - 1))
            else:
                self.assertEqual(key, key_populate(self.fmt, self.nentries - 1))

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
            simple_populate(self, uri, self.config + self.fmt, self.nentries)
            self.truncateRangeAndCheck(uri, begin, end)
            self.session.drop(uri, None)

        # A complex, multi-file table object.
        if self.type == "table:":
            for begin,end in list:
                complex_populate(
                    self, uri, self.config + self.fmt, self.nentries)
                self.truncateRangeAndCheck(uri, begin, end)
                self.session.drop(uri, None)

if __name__ == '__main__':
    wttest.run()
