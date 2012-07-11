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
from helper import confirmDoesNotExist, confirmEmpty, simplePopulate

# Test session.truncate.
class test_truncate(wttest.WiredTigerTestCase):
    name = 'test_truncate'
    nentries = 10000

    # Use a small page size because we want to create a number of pages.
    config = 'allocation_size=512,key_format=i,value_format=S'

    scenarios = [
        ('file', dict(uri='file:')),
        ('table', dict(uri='table:'))
        ]

    def initCursor(self, name, key):
        if key == None:
            return None
        cursor = self.session.open_cursor(name, None, None)
        cursor.set_key(key)
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_key(), key)
        return cursor

    # Truncate a range using cursors, and check the results.
    def truncateRangeAndCheck(self, name, begin, end):
        self.pr('truncateRangeAndCheck: ' + str(begin) + ',' + str(end))
        simplePopulate(self, name, self.config, self.nentries)
        cur1 = self.initCursor(name, begin)
        cur2 = self.initCursor(name, end)
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
        cursor = self.session.open_cursor(name, None, None)
        if begin == 0 and end == self.nentries - 1:
            confirmEmpty(self, name)
        else:
            self.assertEqual(cursor.next(), 0)
            key = cursor.get_key()
            if begin == 0:
                self.assertEqual(key, end + 1)
            else:
                self.assertEqual(key, 0)

            self.assertEqual(cursor.reset(), 0)
            self.assertEqual(cursor.prev(), 0)
            key = cursor.get_key()
            if end == self.nentries - 1:
                self.assertEqual(key, begin - 1)
            else:
                self.assertEqual(key, self.nentries - 1)

        cursor.close()
        self.session.drop(name, None)

    # Test truncate of an object.
    def test_truncate(self):
        name = self.uri + self.name
        simplePopulate(self, name, self.config, self.nentries)
        self.session.truncate(name, None, None, None)
        confirmEmpty(self, name)

    # Test using cursors for the begin and end of truncate, with 8 cases:
    #    beginning to end (begin and end set to None)
    #    beginning to end (first and last records)
    #    beginning to mid-point (begin cursor None)
    #    beginning to mid-point (begin cursor on first record)
    #    mid-point to end (end cursor None)
    #    mid-point to end (end cursor on last record)
    #    mid-point to mid-point
    #
    # For begin and end: None means pass None for the cursor arg to truncate.
    # An integer N, with 0 <= N < self.nentries, passes a cursor positioned
    # at that element, 0 positions a cursor before the first element, and
    # self.nentries positions a cursor after the last element.
    def test_truncate_cursor(self):
        name = self.uri + self.name
        self.truncateRangeAndCheck(name, 0, self.nentries - 1)
        self.truncateRangeAndCheck(name, None, self.nentries - 1)
        self.truncateRangeAndCheck(name, 0, None)
        self.truncateRangeAndCheck(name, None, self.nentries - 737)
        self.truncateRangeAndCheck(name, 0, self.nentries - 737)
        self.truncateRangeAndCheck(name, 737, None)
        self.truncateRangeAndCheck(name, 737, self.nentries - 1)
        self.truncateRangeAndCheck(name, 737, self.nentries - 737)

if __name__ == '__main__':
    wttest.run()
