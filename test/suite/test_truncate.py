#!/usr/bin/env python
#
# Copyright (c) 2008-2012 WiredTiger, Inc.
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
#	session level operations on tables
#

import os, time
import wiredtiger, wttest
from helper import confirmDoesNotExist, \
    confirmEmpty, complexPopulate, keyPopulate, simplePopulate

# Test session.truncate.
class test_truncate(wttest.WiredTigerTestCase):
    name = 'test_truncate'
    nentries = 10000

    # Use a small page size because we want to create lots of pages in the file.
    config = 'leaf_page_max=1024,key_format='

    scenarios = [
        ( 'file-row',  dict(type='file:',fmt='S')),
        ( 'file-col',  dict(type='file:',fmt='i')),
        ('table-row', dict(type='table:',fmt='S')),
        ('table-col', dict(type='table:',fmt='i'))
        ]

    # Test truncation of an object using its URI.
    def test_truncate(self):
        uri = self.type + self.name
        simplePopulate(self, uri, self.config + self.fmt, self.nentries)
        self.session.truncate(uri, None, None, None)
        confirmEmpty(self, uri)
        self.session.drop(uri, None)

        if self.type == "table:":
            complexPopulate(self, uri, self.config + self.fmt, self.nentries)
            self.session.truncate(uri, None, None, None)
            confirmEmpty(self, uri)
            self.session.drop(uri, None)

    def initCursor(self, uri, key):
        if key == -1:
            return None
        cursor = self.session.open_cursor(uri, None, None)
        cursor.set_key(keyPopulate(self.fmt, key))
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_key(), keyPopulate(self.fmt, key))
        return cursor

    # Truncate a range using cursors, and check the results.
    def truncateRangeAndCheck(self, uri, begin, end):
        self.pr('truncateRangeAndCheck: ' + str(begin) + ',' + str(end))
        cur1 = self.initCursor(uri, begin)
        cur2 = self.initCursor(uri, end)
        self.session.truncate(None, cur1, cur2, None)
        if not cur1:
            begin = 0
        else:
            cur1.close()
        if not cur2:
            end = self.nentries - 1
        else:
            cur2.close()

        # If the object is empty, confirm that, otherwise test the first and
        # last keys are the ones before/after the truncated range.
        cursor = self.session.open_cursor(uri, None, None)
        if begin == 0 and end == self.nentries - 1:
            confirmEmpty(self, uri)
        else:
            self.assertEqual(cursor.next(), 0)
            key = cursor.get_key()
            if begin == 0:
                self.assertEqual(key, keyPopulate(self.fmt, end + 1))
            else:
                self.assertEqual(key, keyPopulate(self.fmt, 0))
            self.assertEqual(cursor.reset(), 0)
            self.assertEqual(cursor.prev(), 0)
            key = cursor.get_key()
            if end == self.nentries - 1:
                self.assertEqual(key, keyPopulate(self.fmt, begin - 1))
            else:
                self.assertEqual(key, keyPopulate(self.fmt, self.nentries - 1))

        cursor.close()

    # Test truncation using cursors, with 8 cases:
    #    beginning to end (begin and end set to None)
    #    beginning to end (begin/end set to first/last records)
    #    beginning to mid-point (begin cursor set to None)
    #    beginning to mid-point (begin cursor set to first record)
    #    mid-point to end (end cursor set to None)
    #    mid-point to end (end cursor set to last record)
    #    mid-point to mid-point
    #
    # For begin and end: -1 means pass None for the cursor arg to truncate.
    # An integer N, with 0 <= N < self.nentries, passes a cursor positioned
    # at that element.
    def test_truncate_cursor(self):
        uri = self.type + self.name
        list = [
            (0, self.nentries - 1),
            (-1, self.nentries - 1),
            (0, -1),
            (-1, self.nentries - 737),
            (0, self.nentries - 737),
            (737, -1),
            (737, self.nentries - 1),
            (737, self.nentries - 737)
            ]

        # A simple, one-file file or table object.
        for begin,end in list:
            simplePopulate(self, uri, self.config + self.fmt, self.nentries)
            self.truncateRangeAndCheck(uri, begin, end)
            self.session.drop(uri, None)

        # A complex, multi-file table object.
        if self.type == "table:":
            for begin,end in list:
                complexPopulate(
                    self, uri, self.config + self.fmt, self.nentries)
                self.truncateRangeAndCheck(uri, begin, end)
                self.session.drop(uri, None)

if __name__ == '__main__':
    wttest.run()
