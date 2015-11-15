#!/usr/bin/env python
#
# Public Domain 2014-2015 MongoDB, Inc.
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

import wiredtiger, wttest
from helper import key_populate, value_populate, simple_populate
from helper import complex_populate, complex_value_populate
from wtscenario import check_scenarios

# test_cursor09.py
#    JIRA WT-2217: insert resets key/value "set".
class test_cursor09(wttest.WiredTigerTestCase):

    scenarios = check_scenarios([
        ('file-r', dict(type='file:',keyfmt='r',complex=0)),
        ('file-S', dict(type='file:',keyfmt='S',complex=0)),
        ('lsm-S', dict(type='lsm:',keyfmt='S',complex=0)),
        ('table-r', dict(type='table:',keyfmt='r',complex=0)),
        ('table-S', dict(type='table:',keyfmt='S',complex=0)),
        ('table-r-complex', dict(type='table:',keyfmt='r',complex=1)),
        ('table-S-complex', dict(type='table:',keyfmt='S',complex=1)),
    ])

    def pop(self, uri):
        if self.complex == 1:
            complex_populate(self, uri, 'key_format=' + self.keyfmt, 100)
        else:
            simple_populate(self, uri, 'key_format=' + self.keyfmt, 100)

    def set_kv(self, cursor):
        cursor.set_key(key_populate(cursor, 10))
        if self.complex == 1:
            v = complex_value_populate(cursor, 10)
            cursor.set_value(v[0], v[1], v[2], v[3])
        else:
            cursor.set_value(value_populate(cursor, 10))

    # WT_CURSOR.insert doesn't leave the cursor positioned, verify any
    # subsequent cursor operation fails with a "key not set" message.
    def test_cursor09(self):
        uri = self.type + 'cursor09'
        self.pop(uri)
        cursor = self.session.open_cursor(uri, None, None)
        self.set_kv(cursor)
        cursor.insert()
        msg = '/requires key be set/'
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, cursor.search, msg)


if __name__ == '__main__':
    wttest.run()
