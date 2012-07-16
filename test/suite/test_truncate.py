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

    # Use a small page size because we want to create lots of pages in the file.
    config = 'leaf_page_max=1024,key_format=i,value_format=S'

    scenarios = [
        ('file', dict(uri='file:')),
        ('table', dict(uri='table:'))
        ]

    # Test simple truncate of an object using its URI.
    def test_truncate(self):
        name = self.uri + self.name
        simplePopulate(self, name, self.config, self.nentries)
        self.session.truncate(name, None, None, None)
        confirmEmpty(self, name)

    def initCursor(self, name, key):
        if key == -1:
            return None
        cursor = self.session.open_cursor(name, None, None)
        cursor.set_key(key)
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_key(), key)
        return cursor

    # Truncate a range using cursors, and check the results.
    def truncateRangeAndCheck(self, name, begin, end):
        self.pr('truncateRangeAndCheck: ' + str(begin) + ',' + str(end))
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

    # Build a complex table with multiple files.
    def complexPopulate(self):
        name = self.uri + self.name
        self.session.create(name,
            'leaf_page_max=1024,key_format=i,value_format=SiSS,' +
            'columns=(record,column2,column3,column4,column5),' +
            'colgroups=(cgroup1,cgroup2,cgroup3,cgroup4,cgroup5,cgroup6)')
        cgname = 'colgroup:' + self.name
        self.session.create(cgname + ':cgroup1', 'columns=(column2)')
        self.session.create(cgname + ':cgroup2', 'columns=(column3)')
        self.session.create(cgname + ':cgroup3', 'columns=(column4)')
        self.session.create(cgname + ':cgroup4', 'columns=(column2,column3)')
        self.session.create(cgname + ':cgroup5', 'columns=(column3,column4)')
        self.session.create(cgname + ':cgroup6', 'columns=(column4,column5)')
        cursor = self.session.open_cursor(name, None, None)
        for i in range(0, self.nentries):
                cursor.set_key(i)
                cursor.set_value(
                    str(i) + ': abcdefghijklmnopqrstuvwxyz'[0:i%26],
                    i,
                    str(i) + ': abcdefghijklmnopqrstuvwxyz'[0:i%23],
                    str(i) + ': abcdefghijklmnopqrstuvwxyz'[0:i%18])
                cursor.insert()
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
        name = self.uri + self.name
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
            simplePopulate(self, name, self.config, self.nentries)
            self.truncateRangeAndCheck(name, begin, end)
            self.session.drop(name, None)

        # A complex, multi-file table object.
        if self.uri == "table:":
            for begin,end in list:
                self.complexPopulate()
                self.truncateRangeAndCheck(name, begin, end)
                self.session.drop(name, None)

if __name__ == '__main__':
    wttest.run()
