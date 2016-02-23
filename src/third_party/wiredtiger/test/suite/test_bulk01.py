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
#
# test_bulk.py
#       bulk-cursor test.
#

import wiredtiger, wttest
from helper import key_populate, value_populate
from wtscenario import check_scenarios, multiply_scenarios, number_scenarios

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
            cursor[key_populate(cursor, i)] = value_populate(cursor, i)
        cursor.close()


# Test that out-of-order insert in a row-store fails by default, but
# works if key order validation is turned off.
class test_bulk_load_row_order(wttest.WiredTigerTestCase):
    name = 'test_bulk'

    scenarios = check_scenarios([
        ('file', dict(type='file:')),
        ('table', dict(type='table:'))
    ])

    def test_bulk_load_row_order_check(self):
        uri = self.type + self.name
        self.session.create(uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(uri, None, "bulk")
        cursor[key_populate(cursor, 10)] = value_populate(cursor, 10)

        cursor.set_key(key_populate(cursor, 1))
        cursor.set_value(value_populate(cursor, 1))
        msg = '/compares smaller than previously inserted key/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: cursor.insert(), msg)

    def test_bulk_load_row_order_nocheck(self):
        uri = self.type + self.name
        self.session.create(uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(uri, None, "bulk,skip_sort_check")
        cursor[key_populate(cursor, 10)] = value_populate(cursor, 10)
        cursor[key_populate(cursor, 1)] = value_populate(cursor, 1)

        if not wiredtiger.diagnostic_build():
            self.skipTest('requires a diagnostic build')

        # Close explicitly, there's going to be a fallure.
        msg = '/are incorrectly sorted/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.conn.close(), msg)


# Test that inserting into the file blocks a subsequent bulk-load.
class test_bulk_load_not_empty(wttest.WiredTigerTestCase):
    name = 'test_bulk'

    scenarios = check_scenarios([
        ('file', dict(type='file:')),
        ('table', dict(type='table:'))
    ])

    def test_bulk_load_not_empty(self):
        uri = self.type + self.name
        self.session.create(uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(uri, None)
        cursor[key_populate(cursor, 1)] = value_populate(cursor, 1)
        # Close the insert cursor, else we'll get EBUSY.
        cursor.close()
        msg = '/bulk-load is only supported on newly created objects/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor(uri, None, "bulk"), msg)

    def test_bulk_load_busy(self):
        uri = self.type + self.name
        self.session.create(uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(uri, None)
        cursor[key_populate(cursor, 1)] = value_populate(cursor, 1)
        # Don't close the insert cursor, we want EBUSY.
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor(uri, None, "bulk"))


if __name__ == '__main__':
    wttest.run()
