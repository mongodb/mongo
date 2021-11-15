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

import wiredtiger, wttest
from wtdataset import SimpleDataSet, ComplexDataSet, simple_key, simple_value
from wtscenario import make_scenarios

# test_cursor_random.py
#    Cursor next_random operations
class test_cursor_random(wttest.WiredTigerTestCase):
    types = [
        ('file', dict(type='file:random', dataset=SimpleDataSet)),
        ('table', dict(type='table:random', dataset=ComplexDataSet))
    ]
    config = [
        ('sample', dict(config='next_random=true,next_random_sample_size=35')),
        ('not-sample', dict(config='next_random=true'))
    ]
    scenarios = make_scenarios(types, config)

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

    # Check that next_random fails with an empty tree, repeatedly.
    def test_cursor_random_empty(self):
        uri = self.type
        self.session.create(uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(uri, None, self.config)
        for i in range(1,5):
            self.assertTrue(cursor.next(), wiredtiger.WT_NOTFOUND)
        cursor.close

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
    def cursor_random_multiple_insert_records(self, n):
        uri = self.type
        ds = self.dataset(self, uri, n,
            config='allocation_size=512,leaf_page_max=512')
        ds.populate()

        # Assert we only see 20% matches. We expect to see less than that, but we don't want
        # to chase random test failures, either.
        cursor = self.session.open_cursor(uri, None, self.config)
        list=[]
        for i in range(1,100):
            self.assertEqual(cursor.next(), 0)
            list.append(cursor.get_key())
        self.assertGreater(len(set(list)), 80)

    def test_cursor_random_multiple_insert_records_small(self):
        self.cursor_random_multiple_insert_records(2000)
    def test_cursor_random_multiple_insert_records_large(self):
        self.cursor_random_multiple_insert_records(10000)

    # Check that next_random works in the presence of a larger set of values,
    # where the values are in a disk format page.
    def cursor_random_multiple_page_records(self, n, reopen):
        uri = self.type
        ds = self.dataset(self, uri, n,
            config='allocation_size=512,leaf_page_max=512')
        ds.populate()

        # Optionally close the connection so everything is forced to disk, insert lists are an
        # entirely different page format.
        if reopen:
            self.reopen_conn()

        # Assert we only see 20% matches. We expect to see less than that, but we don't want
        # to chase random test failures, either.
        cursor = self.session.open_cursor(uri, None, self.config)
        list=[]
        for i in range(1, 100):
            self.assertEqual(cursor.next(), 0)
            list.append(cursor.get_key())
        self.assertGreater(len(set(list)), 80)

    def test_cursor_random_multiple_page_records_reopen_small(self):
        self.cursor_random_multiple_page_records(2000, True)
    def test_cursor_random_multiple_page_records_reopen_large(self):
        self.cursor_random_multiple_page_records(10000, True)
    def test_cursor_random_multiple_page_records_small(self):
        self.cursor_random_multiple_page_records(2000, False)
    def test_cursor_random_multiple_page_records_large(self):
        self.cursor_random_multiple_page_records(10000, False)

    # Check that next_random succeeds in the presence of a set of values, some of
    # which are deleted.
    def test_cursor_random_deleted_partial(self):
        uri = self.type
        ds = self.dataset(self, uri, 10000,
            config='allocation_size=512,leaf_page_max=512')
        ds.populate()

        # Close the connection so everything is forced to disk.
        self.reopen_conn()

        start = self.session.open_cursor(uri, None)
        start.set_key(ds.key(10))
        end = self.session.open_cursor(uri, None)
        end.set_key(ds.key(10000-10))
        self.session.truncate(None, start, end, None)
        self.assertEqual(start.close(), 0)
        self.assertEqual(end.close(), 0)

        cursor = self.session.open_cursor(uri, None, self.config)
        for i in range(1,10):
            self.assertEqual(cursor.next(), 0)

    # Check that next_random fails in the presence of a set of values, all of
    # which are deleted.
    def test_cursor_random_deleted_all(self):
        uri = self.type
        ds = self.dataset(self, uri, 10000,
            config='allocation_size=512,leaf_page_max=512')
        ds.populate()

        # Close the connection so everything is forced to disk.
        self.reopen_conn()

        self.session.truncate(uri, None, None, None)

        cursor = self.session.open_cursor(uri, None, self.config)
        for i in range(1,10):
            self.assertTrue(cursor.next(), wiredtiger.WT_NOTFOUND)

# Check that opening a random cursor on column-store returns not-supported.
class test_cursor_random_column(wttest.WiredTigerTestCase):
    type_values = [
        ('file', dict(uri='file:random')),
        ('table', dict(uri='table:random'))
    ]
    valfmt_values = [
        ('string', dict(valfmt='S')),
        ('fix', dict(valfmt='8t')),
    ]
    scenarios = make_scenarios(type_values, valfmt_values)

    def test_cursor_random_column(self):
        self.session.create(self.uri, 'key_format=r,value_format={}'.format(self.valfmt))
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
    scenarios = make_scenarios(types, config)

    def test_cursor_random_invisible_all(self):
        uri = self.type
        self.session.create(uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(uri, None)

        # Start a transaction.
        self.session.begin_transaction()
        for i in range(1, 100):
            cursor[simple_key(cursor, i)] = simple_value(cursor, i)

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
        cursor[simple_key(cursor, 1)] = simple_value(cursor, 1)

        # Start a transaction.
        self.session.begin_transaction()
        for i in range(2, 100):
            cursor[simple_key(cursor, i)] = simple_value(cursor, i)

        # Open another session, the updates won't yet be visible, we should
        # return the only possible record.
        s = self.conn.open_session()
        cursor = s.open_cursor(uri, None, self.config)
        self.assertEquals(cursor.next(), 0)
        self.assertEqual(cursor.get_key(), simple_key(cursor, 1))

    def test_cursor_random_invisible_before(self):
        uri = self.type
        self.session.create(uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(uri, None)

        # Insert a single leading record.
        cursor[simple_key(cursor, 99)] = simple_value(cursor, 99)

        # Start a transaction.
        self.session.begin_transaction()
        for i in range(2, 100):
            cursor[simple_key(cursor, i)] = simple_value(cursor, i)

        # Open another session, the updates won't yet be visible, we should
        # return the only possible record.
        s = self.conn.open_session()
        cursor = s.open_cursor(uri, None, self.config)
        self.assertEquals(cursor.next(), 0)
        self.assertEqual(cursor.get_key(), simple_key(cursor, 99))

if __name__ == '__main__':
    wttest.run()
