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
# test_rename.py
#	session level rename operation
#

import os, time
import wiredtiger, wttest
from helper import confirmDoesNotExist, simplePopulate, simplePopulateCheck

# Test session.rename operations.
class test_rename(wttest.WiredTigerTestCase):
    name1 = 'test_rename1'
    name2 = 'test_rename2'

    scenarios = [
        ('file', dict(uri='file:')),
        ('table', dict(uri='table:'))
        ]

    # Test rename of an object.
    def test_rename(self):
	name1 = self.uri + self.name1
	name2 = self.uri + self.name2
        simplePopulate(self, name1, 'key_format=S,value_format=S', 10)
        self.session.rename(name1, name2, None)
        confirmDoesNotExist(self, name1)
        simplePopulateCheck(self, name2)

        self.session.rename(name2, name1, None)
        confirmDoesNotExist(self, name2)
        simplePopulateCheck(self, name1)

    def test_rename_dne(self):
	name1 = self.uri + self.name1
	name2 = self.uri + self.name2
        confirmDoesNotExist(self, name1)
        self.assertRaises(wiredtiger.WiredTigerError,
	    lambda: self.session.rename(name1, name2, None))

if __name__ == '__main__':
    wttest.run()
