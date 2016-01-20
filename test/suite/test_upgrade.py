#!/usr/bin/env python
#
# Public Domain 2014-2016 MongoDB, Inc.
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

import os, time
import wiredtiger, wttest
from helper import complex_populate, simple_populate
from wtscenario import check_scenarios

# test_upgrade.py
#    session level upgrade operation
class test_upgrade(wttest.WiredTigerTestCase):
    name = 'test_upgrade'

    scenarios = check_scenarios([
        ('file', dict(uri='file:')),
        ('table', dict(uri='table:'))
    ])

    # Populate an object, then upgrade it.
    def upgrade(self, populate, with_cursor):
        uri = self.uri + self.name
        populate(self, uri, 'key_format=S', 10)

        # Open cursors should cause failure.
        if with_cursor:
            cursor = self.session.open_cursor(uri, None, None)
            self.assertRaises(wiredtiger.WiredTigerError,
                lambda: self.session.drop(uri, None))
            cursor.close()

        self.session.upgrade(uri, None)
        self.session.drop(uri)

    # Test upgrade of an object.
    def test_upgrade(self):
        # Simple file or table object.
        self.upgrade(simple_populate, False)
        self.upgrade(simple_populate, True)

        # A complex, multi-file table object.
        if self.uri == "table:":
            self.upgrade(complex_populate, False)
            self.upgrade(complex_populate, True)


if __name__ == '__main__':
    wttest.run()
