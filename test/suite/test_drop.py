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
# test_drop.py
#	session level drop operation

import os, time
import wiredtiger, wttest
from helper import confirmDoesNotExist, complexPopulate, simplePopulate

# Test session.drop operations.
class test_drop(wttest.WiredTigerTestCase):
    name = 'test_drop'

    scenarios = [
        ('file', dict(uri='file:')),
        ('table', dict(uri='table:'))
        ]

    # Test drop of an object.
    def test_drop(self):
        # Simple file or table object.
        uri = self.uri + self.name
        simplePopulate(self, uri, 'key_format=S', 10)
        self.session.drop(uri, None)
        confirmDoesNotExist(self, uri)

        # A complex, multi-file table object.
        if self.uri == "table:":
            complexPopulate(self, uri, 'key_format=S', 10)
            self.session.drop(uri, None)
            confirmDoesNotExist(self, uri)

    # Test drop of a non-existent object.
    def test_drop_dne(self):
        uri = self.uri + self.name
        confirmDoesNotExist(self, uri)
        self.session.drop(uri, 'force')
        self.assertRaises(
            wiredtiger.WiredTigerError, lambda: self.session.drop(uri, None))

if __name__ == '__main__':
    wttest.run()
