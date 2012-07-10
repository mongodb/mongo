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
# test_base06.py
#	session level operations on tables
#

import os, time
import wiredtiger, wttest
from helper import confirmDoesNotExist, confirmEmpty

# Test session.drop and session.rename operations.
class test_base06(wttest.WiredTigerTestCase):
    name1 = 'test_base06a'
    name2 = 'test_base06b'
    nentries = 30

    scenarios = [
        ('file', dict(uri='file:')),
        ('table', dict(uri='table:'))
        ]

    def populate(self, name):
        create_args = 'key_format=i,value_format=S'
        self.session.create(self.uri + name, create_args)
        cursor = self.session.open_cursor(self.uri + name, None, None)
        for i in range(0, self.nentries):
            cursor.set_key(i)
            cursor.set_value(str(i))
            cursor.insert()
        self.pr('populate: ' + name + ': added ' + str(self.nentries))
        cursor.close()

    def checkContents(self, name):
        cursor = self.session.open_cursor(self.uri + name, None, None)
        want = 0
        for key,val in cursor:
            self.assertEqual(key, want)
            self.assertEqual(val, str(want))
            want += 1
        self.assertEqual(want, self.nentries)
        cursor.close()

    def test_nop(self):
        """ Make sure our test functions work """
        self.populate(self.name1)
        self.checkContents(self.name1)
        confirmDoesNotExist(self, self.uri + self.name2)

    def test_drop(self):
        self.populate(self.name1)
        self.session.drop(self.uri + self.name1, None)
        confirmDoesNotExist(self, self.uri + self.name1)
        self.session.drop(self.uri + self.name1, 'force')
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: self.session.drop(self.uri + self.name1, None))

    def test_rename(self):
        self.populate(self.name1)
        self.session.rename(
            self.uri + self.name1, self.uri + self.name2, None)
        self.checkContents(self.name2)
        confirmDoesNotExist(self, self.uri + self.name1)
        self.session.rename(
            self.uri + self.name2, self.uri + self.name1, None)
        self.checkContents(self.name1)
        confirmDoesNotExist(self, self.uri + self.name2)
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: self.session.rename(
            self.uri + self.name2, self.uri + self.name1, None))

if __name__ == '__main__':
    wttest.run()
