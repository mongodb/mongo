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

import wiredtiger, wttest
from helper import complex_populate, simple_populate
from helper import key_populate, value_populate
from wtscenario import check_scenarios, multiply_scenarios, number_scenarios

# test_cursor_random.py
#    Cursor next_random operations
class test_cursor_random(wttest.WiredTigerTestCase):
    types = [
        ('file', dict(type='file:random')),
        ('table', dict(type='table:random'))
    ]
    config = [
        ('sample', dict(config='next_random=true,next_random_sample_size=35')),
        ('not-sample', dict(config='next_random=true'))
    ]
    scenarios =number_scenarios(multiply_scenarios('.', types, config))

    # Check that opening a random cursor on a row-store returns not-supported
    # for methods other than next, reconfigure and reset, and next returns
    # not-found.
    def test_cursor_random(self):
        uri = self.type
        self.session.create(uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(uri, None, self.config)
        msg = "/Unsupported cursor/"
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, lambda: cursor.compare(cursor), msg)
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, lambda: cursor.insert(), msg)
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, lambda: cursor.prev(), msg)
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, lambda: cursor.remove(), msg)
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, lambda: cursor.search(), msg)
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, lambda: cursor.search_near(), msg)
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, lambda: cursor.update(), msg)

        self.assertTrue(cursor.next(), wiredtiger.WT_NOTFOUND)
        self.assertEquals(cursor.reconfigure(), 0)
        self.assertEquals(cursor.reset(), 0)
        cursor.close()

    # Check that next_random works with a single value, repeatedly.
    def test_cursor_random_single_record(self):
        uri = self.type
        self.session.create(uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(uri, None)
        cursor['AAA'] = 'BBB'
        cursor.close()
        cursor = self.session.open_cursor(uri, None, self.config)
        for i in range(1,5):
            self.assertEquals(cursor.next(), 0)
            self.assertEquals(cursor.get_key(), 'AAA')
        cursor.close

    # Check that next_random works in the presence of a larger set of values,
    # where the values are in an insert list.
    def test_cursor_random_multiple_insert_records(self):
        uri = self.type
        if uri.startswith('file:'):
            simple_populate(self, uri,
                'allocation_size=512,leaf_page_max=512,key_format=S', 100)
        else:
            complex_populate(self, uri,
                'allocation_size=512,leaf_page_max=512,key_format=S', 100)

        # In a insert list, next_random always selects the middle key/value
        # pair, all we can do is confirm cursor.next works.
        cursor = self.session.open_cursor(uri, None, self.config)
        self.assertEqual(cursor.next(), 0)

    # Check that next_random works in the presence of a larger set of values,
    # where the values are in a disk format page.
    def cursor_random_multiple_page_records(self, reopen):
        uri = self.type
        if uri.startswith('file:'):
            simple_populate(self, uri,
                'allocation_size=512,leaf_page_max=512,key_format=S', 10000)
        else:
            complex_populate(self, uri,
                'allocation_size=512,leaf_page_max=512,key_format=S', 10000)

        # Optionally close the connection so everything is forced to disk,
        # insert lists are an entirely different path in the code.
        if reopen:
            self.reopen_conn()

        cursor = self.session.open_cursor(uri, None, self.config)
        last = ''
        match = 0
        for i in range(1,10):
            self.assertEqual(cursor.next(), 0)
            current = cursor.get_key()
            if current == last:
                match += 1
            last = current
        self.assertLess(match, 5,
            'next_random did not return random records, too many matches found')

    def test_cursor_random_multiple_page_records_reopen(self):
        self.cursor_random_multiple_page_records(1)
    def test_cursor_random_multiple_page_records(self):
        self.cursor_random_multiple_page_records(0)

# Check that opening a random cursor on column-store returns not-supported.
class test_cursor_random_column(wttest.WiredTigerTestCase):
    scenarios = check_scenarios([
        ('file', dict(uri='file:random')),
        ('table', dict(uri='table:random'))
    ])

    def test_cursor_random_column(self):
        self.session.create(self.uri, 'key_format=r,value_format=S')
        msg = '/next_random .* not supported/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
            self.session.open_cursor(self.uri, None, "next_random=true"), msg)


# Check next_random works in the presence a set of updates, some or all of
# which are invisible to the cursor.
class test_cursor_random_invisible(wttest.WiredTigerTestCase):
    types = [
        ('file', dict(type='file:random')),
        ('table', dict(type='table:random'))
    ]
    config = [
        ('sample', dict(config='next_random=true,next_random_sample_size=35')),
        ('not-sample', dict(config='next_random=true'))
    ]
    scenarios =number_scenarios(multiply_scenarios('.', types, config))

    def test_cursor_random_invisible_all(self):
        uri = self.type
        self.session.create(uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(uri, None)

        # Start a transaction.
        self.session.begin_transaction()
        for i in range(1, 100):
            cursor[key_populate(cursor, i)] = value_populate(cursor, i)

        # Open another session, the updates won't yet be visible, we shouldn't
        # find anything at all.
        s = self.conn.open_session()
        cursor = s.open_cursor(uri, None, self.config)
        self.assertEqual(cursor.next(), wiredtiger.WT_NOTFOUND)

    def test_cursor_random_invisible_after(self):
        uri = self.type
        self.session.create(uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(uri, None)

        # Insert a single leading record.
        cursor[key_populate(cursor, 1)] = value_populate(cursor, 1)

        # Start a transaction.
        self.session.begin_transaction()
        for i in range(2, 100):
            cursor[key_populate(cursor, i)] = value_populate(cursor, i)

        # Open another session, the updates won't yet be visible, we should
        # return the only possible record.
        s = self.conn.open_session()
        cursor = s.open_cursor(uri, None, self.config)
        self.assertEquals(cursor.next(), 0)
        self.assertEqual(cursor.get_key(), key_populate(cursor, 1))

    def test_cursor_random_invisible_before(self):
        uri = self.type
        self.session.create(uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(uri, None)

        # Insert a single leading record.
        cursor[key_populate(cursor, 99)] = value_populate(cursor, 99)

        # Start a transaction.
        self.session.begin_transaction()
        for i in range(2, 100):
            cursor[key_populate(cursor, i)] = value_populate(cursor, i)

        # Open another session, the updates won't yet be visible, we should
        # return the only possible record.
        s = self.conn.open_session()
        cursor = s.open_cursor(uri, None, self.config)
        self.assertEquals(cursor.next(), 0)
        self.assertEqual(cursor.get_key(), key_populate(cursor, 99))


if __name__ == '__main__':
    wttest.run()
