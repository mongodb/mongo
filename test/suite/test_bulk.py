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
# test_bulk.py
#       bulk-cursor test.
#

import wiredtiger, wttest
from helper import key_populate, value_populate
from wtscenario import multiply_scenarios, number_scenarios

# Test that inserting into the file blocks a subsequent bulk-load.
class test_bulk_load_not_empty(wttest.WiredTigerTestCase):
    name = 'test_bulk'

    scenarios = [
        ('file', dict(type='file:')),
        ('table', dict(type='table:'))
    ]

    def test_bulk_load_not_empty(self):
        uri = self.type + self.name
        self.session.create(uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(uri, None)
        cursor.set_key(key_populate(cursor, 1))
        cursor.set_value(value_populate(cursor, 1))
        cursor.insert()
        # Close the insert cursor, else we'll get EBUSY.
        cursor.close()
        msg = '/bulk-load is only supported on newly created objects/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor(uri, None, "bulk"), msg)

    def test_bulk_load_busy(self):
        uri = self.type + self.name
        self.session.create(uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(uri, None)
        cursor.set_key(key_populate(cursor, 1))
        cursor.set_value(value_populate(cursor, 1))
        cursor.insert()
        # Don't close the insert cursor, we want EBUSY.
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor(uri, None, "bulk"))


# Smoke test bulk-load.
class test_bulk_load(wttest.WiredTigerTestCase):
    name = 'test_bulk'

    types = [
        ('file', dict(type='file:')),
        ('table', dict(type='table:'))
    ]
    keyfmt = [
        ('integer', dict(keyfmt='i')),
        ('recno', dict(keyfmt='r')),
        ('string', dict(keyfmt='S')),
    ]
    valfmt = [
        ('fixed', dict(valfmt='8t')),
        ('integer', dict(valfmt='i')),
        ('string', dict(valfmt='S')),
    ]
    scenarios = number_scenarios(multiply_scenarios('.', types, keyfmt, valfmt))

    # Test a simple bulk-load
    def test_bulk_load(self):
        uri = self.type + self.name
        self.session.create(uri,
            'key_format=' + self.keyfmt + ',value_format=' + self.valfmt)
        cursor = self.session.open_cursor(uri, None, "bulk")
        for i in range(1, 100):
            cursor.set_key(key_populate(cursor, i))
            cursor.set_value(value_populate(cursor, i))
            cursor.insert()
        cursor.close()


if __name__ == '__main__':
    wttest.run()
