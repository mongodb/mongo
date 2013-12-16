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
#
# test_huge.py
#       Test huge key/value items.

import wiredtiger, wttest
from helper import key_populate

class test_huge(wttest.WiredTigerTestCase):
    name = 'huge'

    scenarios = [
        ('file', dict(type='file:', keyfmt='r')),
        ('file', dict(type='file:', keyfmt='S')),
        ('lsm', dict(type='lsm:', keyfmt='S')),
        ('table', dict(type='table:', keyfmt='r')),
        ('table', dict(type='table:', keyfmt='S'))
    ]

    # Override WiredTigerTestCase, we want a large cache so that eviction
    # doesn't happen, there's nothing to evict.
    def setUpConnectionOpen(self, dir):
        conn = wiredtiger.wiredtiger_open(dir,
            'create,cache_size=10GB,error_prefix="%s: "' % self.shortid())
        return conn

    def huge_key_ok(self, key):
        uri = self.type + self.name
        config = 'key_format=' + self.keyfmt + ',value_format=S'
        self.session.create(uri, config)
        cursor = self.session.open_cursor(uri, None, None)
        cursor.set_key(key)
        cursor.set_value("value001")
        cursor.insert()
        cursor.set_key(key)
        cursor.remove()
        cursor.close()

    def huge_key_fail(self, key):
        uri = self.type + self.name
        config = 'key_format=' + self.keyfmt + ',value_format=S'
        self.session.create(uri, config)
        cursor = self.session.open_cursor(uri, None, None)
        cursor.set_key(key)
        cursor.set_value("value001")
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: cursor.insert(), '/exceeds the maximum supported size/')
        cursor.close()

    def huge_value_ok(self, value):
        uri = self.type + self.name
        config = 'key_format=' + self.keyfmt + ',value_format=S'
        self.session.create(uri, config)
        cursor = self.session.open_cursor(uri, None, None)
        cursor.set_key(key_populate(cursor, 1))
        cursor.set_value(value)
        cursor.insert()
        cursor.set_key(key_populate(cursor, 1))
        cursor.remove()
        cursor.close()

    def huge_value_fail(self, value):
        uri = self.type + self.name
        config = 'key_format=' + self.keyfmt + ',value_format=S'
        self.session.create(uri, config)
        cursor = self.session.open_cursor(uri, None, None)
        cursor.set_key(key_populate(cursor, 1))
        cursor.set_value(value)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: cursor.insert(), '/exceeds the maximum supported size/')
        cursor.close()

    # Huge key test.
    def test_huge(self):
        v = "a" * 1073741824                    # 1GB
        if self.keyfmt == 'S':                  # Huge keys for row-store only
                self.huge_key_ok(v)
        self.huge_value_ok(v)

        v = "a" * 4294967200                    # Just under 4GB
        if self.keyfmt == 'S':
                self.huge_key_fail(v)           # Huge keys for row-store only
        self.huge_value_fail(v)


if __name__ == '__main__':
    wttest.run()
