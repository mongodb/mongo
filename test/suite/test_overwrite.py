#!/usr/bin/env python
#
# Public Domain 2008-2013 WiredTiger, Inc.
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

import wiredtiger, wttest
from helper import key_populate, simple_populate

# test_overwrite.py
#    cursor overwrite configuration method
class test_overwrite(wttest.WiredTigerTestCase):
    scenarios = [
        ('file', dict(uri='file:overwrite',fmt='r')),
        ('file', dict(uri='file:overwrite',fmt='S')),
        ('table', dict(uri='table:overwrite',fmt='r')),
        ('table', dict(uri='table:overwrite',fmt='S'))
        ]

    # Test configuration of a cursor for overwrite.
    def test_overwrite(self):
        simple_populate(self, self.uri, 'key_format=' + self.fmt, 100)
        cursor = self.session.open_cursor(self.uri, None, None)
        cursor.set_key(key_populate(cursor, 10))
        cursor.set_value('XXXXXXXXXX')
        self.assertRaises(wiredtiger.WiredTigerError, lambda: cursor.insert())

        cursor = self.session.open_cursor(self.uri, None, "overwrite")
        cursor.set_key(key_populate(cursor, 10))
        cursor.set_value('XXXXXXXXXX')
        cursor.insert()

    # Test duplicating a cursor with overwrite.
    def test_overwrite_reconfig(self):
        simple_populate(self, self.uri, 'key_format=' + self.fmt, 100)
        cursor = self.session.open_cursor(self.uri, None)
        cursor.set_key(key_populate(cursor, 10))
        cursor.set_value('XXXXXXXXXX')
        self.assertRaises(wiredtiger.WiredTigerError, lambda: cursor.insert())

        cursor.set_key(key_populate(cursor, 10))
        dupc = self.session.open_cursor(None, cursor, "overwrite")
        dupc.set_value('XXXXXXXXXX')
        dupc.insert()


if __name__ == '__main__':
    wttest.run()
