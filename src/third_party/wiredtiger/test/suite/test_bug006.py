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
# test_bug006.py
#       Regression tests.

import wiredtiger, wttest
from wtdataset import simple_key, simple_value
from wtscenario import make_scenarios

# Check that verify and salvage both raise exceptions if there is an open
# cursor.
class test_bug006(wttest.WiredTigerTestCase):
    name = 'test_bug006'
    scenarios = make_scenarios([
        ('file', dict(uri='file:')),
        ('table', dict(uri='table:')),
    ])

    def test_bug006(self):
        if 'tiered' in self.hook_names:
            self.skipTest("negative tests for session APIs like drop do not work in tiered storage")
        uri = self.uri + self.name
        self.session.create(uri, 'value_format=S,key_format=S')
        cursor = self.session.open_cursor(uri, None)
        for i in range(1, 1000):
            cursor[simple_key(cursor, i)] = simple_value(cursor, i)

        # Table operations should fail, the cursor is open.
        self.assertRaises(
            wiredtiger.WiredTigerError, lambda: self.session.drop(uri, None))
        self.assertRaises(
            wiredtiger.WiredTigerError,
            lambda: self.session.rename(uri, self.uri + "new", None))
        self.assertRaises(
            wiredtiger.WiredTigerError, lambda: self.session.salvage(uri, None))
        self.assertRaises(
            wiredtiger.WiredTigerError, lambda: self.session.upgrade(uri, None))
        self.assertRaises(
            wiredtiger.WiredTigerError, lambda: self.session.verify(uri, None))

        cursor.close()

        # Table operations should succeed, the cursor is closed.
        self.renameUntilSuccess(self.session, uri, self.uri + "new")
        self.renameUntilSuccess(self.session, self.uri + "new", uri)
        self.session.salvage(uri, None)
        self.session.truncate(uri, None, None, None)
        self.upgradeUntilSuccess(self.session, uri)
        self.verifyUntilSuccess(self.session, uri)

        self.dropUntilSuccess(self.session, uri)

if __name__ == '__main__':
    wttest.run()
