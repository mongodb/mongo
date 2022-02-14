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
# test_bulk.py
#       bulk-cursor test.
#

import wiredtiger, wttest
from wtdataset import simple_key, simple_value
from wtscenario import make_scenarios

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
    scenarios = make_scenarios(types, keyfmt, valfmt)

    # Test a simple bulk-load
    def test_bulk_load(self):
        uri = self.type + self.name
        self.session.create(uri,
            'key_format=' + self.keyfmt + ',value_format=' + self.valfmt)
        cursor = self.session.open_cursor(uri, None, "bulk")
        for i in range(1, 1000):
            cursor[simple_key(cursor, i)] = simple_value(cursor, i)

    # Test a bulk-load triggers variable-length column-store RLE correctly.
    def test_bulk_load_var_rle(self):
        if self.keyfmt != 'r' or self.valfmt == '8t':
                return

        # We can't directly test RLE, it's internal to WiredTiger. However,
        # diagnostic builds catch records that should have been RLE compressed,
        # but weren't, so setting matching values should be sufficient.
        uri = self.type + self.name
        self.session.create(uri,
            'key_format=' + self.keyfmt + ',value_format=' + self.valfmt)
        cursor = self.session.open_cursor(uri, None, "bulk")
        for i in range(1, 1000):
            cursor[simple_key(cursor, i)] = simple_value(cursor, i//7)

    # Test a bulk-load variable-length column-store append ignores any key.
    def test_bulk_load_var_append(self):
        if self.keyfmt != 'r':
                return

        uri = self.type + self.name
        self.session.create(uri,
            'key_format=' + self.keyfmt + ',value_format=' + self.valfmt)
        cursor = self.session.open_cursor(uri, None, "bulk,append")
        for i in range(1, 1000):
            cursor[simple_key(cursor, 37)] = simple_value(cursor, i)
        cursor.close()
        cursor = self.session.open_cursor(uri, None, None)
        for i in range(1, 1000):
            cursor.set_key(simple_key(cursor, i))
            cursor.search()
            self.assertEqual(cursor.get_value(), simple_value(cursor, i))

    # Test that column-store bulk-load handles skipped records correctly.
    def test_bulk_load_col_delete(self):
        if self.keyfmt != 'r':
                return

        uri = self.type + self.name
        self.session.create(uri,
            'key_format=' + self.keyfmt + ',value_format=' + self.valfmt)
        cursor = self.session.open_cursor(uri, None, "bulk")
        for i in range(1, 1000):
            if i % 7 == 0:
                cursor[simple_key(cursor, i)] = simple_value(cursor, i)

        # Ensure we create all the missing records.
        i = i + 1
        cursor[simple_key(cursor, i)] = simple_value(cursor, i)

        cursor.close()
        cursor = self.session.open_cursor(uri, None, None)

        # Verify all the records are there, in their proper state.
        for i in range(1, 1000):
            cursor.set_key(simple_key(cursor, i))
            if i % 7 == 0:
                cursor.search()
                self.assertEqual(cursor.get_value(), simple_value(cursor, i))
            elif cursor.value_format == '8t':
                cursor.search()
                self.assertEqual(cursor.get_value(), 0)
            else:
                self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)

    # Test that variable-length column-store bulk-load efficiently creates big
    # records.
    def test_bulk_load_col_big(self):
        if self.keyfmt != 'r' or self.valfmt == '8t':
                return

        uri = self.type + self.name
        self.session.create(uri,
            'key_format=' + self.keyfmt + ',value_format=' + self.valfmt)
        cursor = self.session.open_cursor(uri, None, "bulk")
        for i in range(1, 10):
            cursor[simple_key(cursor, i)] = simple_value(cursor, i)

        # A big record -- if it's not efficient, we'll just hang.
        big = 18446744073709551606
        cursor[simple_key(cursor, big)] = simple_value(cursor, big)

        cursor.close()
        cursor = self.session.open_cursor(uri, None, None)
        cursor.set_key(simple_key(cursor, big))
        cursor.search()
        self.assertEqual(cursor.get_value(), simple_value(cursor, big))

    # Test that bulk-load out-of-order fails.
    def test_bulk_load_order_check(self):
        uri = self.type + self.name
        self.session.create(uri,
            'key_format=' + self.keyfmt + ',value_format=' + self.valfmt)
        cursor = self.session.open_cursor(uri, None, "bulk")
        cursor[simple_key(cursor, 10)] = simple_value(cursor, 10)

        for i in [1, 9, 10]:
            cursor.set_key(simple_key(cursor, 1))
            cursor.set_value(simple_value(cursor, 1))
            msg = '/than previously inserted key/'
            self.assertRaisesWithMessage(
                wiredtiger.WiredTigerError, lambda: cursor.insert(), msg)

        cursor[simple_key(cursor, 11)] = simple_value(cursor, 11)

    # Test that row-store bulk-load out-of-order can succeed.
    def test_bulk_load_row_order_nocheck(self):
        # Row-store offers an optional fast-past that skips the relatively
        # expensive key-order checks, used when the input is known to be
        # correct. Column-store comparisons are cheap, so it doesn't have
        # that fast-path support.
        if self.keyfmt != 'S':
                return

        uri = self.type + self.name
        self.session.create(uri,
            'key_format=' + self.keyfmt + ',value_format=' + self.valfmt)
        cursor = self.session.open_cursor(uri, None, "bulk,skip_sort_check")
        cursor[simple_key(cursor, 10)] = simple_value(cursor, 10)
        cursor[simple_key(cursor, 1)] = simple_value(cursor, 1)

        if not wiredtiger.diagnostic_build():
            self.skipTest('requires a diagnostic build')

        # Close explicitly, there's going to be a failure.
        msg = '/are incorrectly sorted/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.conn.close(), msg)

    # Test bulk-load only permitted on newly created objects.
    def test_bulk_load_not_empty(self):
        uri = self.type + self.name
        self.session.create(uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(uri, None)
        cursor[simple_key(cursor, 1)] = simple_value(cursor, 1)
        # Close the insert cursor, else we'll get EBUSY.
        cursor.close()
        msg = '/bulk-load is only supported on newly created objects/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor(uri, None, "bulk"), msg)

    # Test that bulk-load objects cannot be opened by other cursors.
    def test_bulk_load_busy(self):
        uri = self.type + self.name
        self.session.create(uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(uri, None)
        cursor[simple_key(cursor, 1)] = simple_value(cursor, 1)
        # Don't close the insert cursor, we want EBUSY.
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor(uri, None, "bulk"))

if __name__ == '__main__':
    wttest.run()
