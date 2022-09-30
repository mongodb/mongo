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
# test_cursor17.py
#   Test the largest_key interface under various scenarios.
#
import wttest
import wiredtiger
from wtdataset import SimpleDataSet, ComplexDataSet, ComplexLSMDataSet
from wtscenario import make_scenarios

class test_cursor17(wttest.WiredTigerTestCase):
    tablename = 'test_cursor17'

    # Enable the lsm tests once it is supported.
    types = [
        ('file-row', dict(type='file:', keyformat='i', valueformat='i', dataset=SimpleDataSet)),
        ('table-row', dict(type='table:', keyformat='i', valueformat='i', dataset=SimpleDataSet)),
        ('file-var', dict(type='file:', keyformat='r', valueformat='i', dataset=SimpleDataSet)),
        ('table-var', dict(type='table:', keyformat='r', valueformat='i', dataset=SimpleDataSet)),
        ('file-fix', dict(type='file:', keyformat='r', valueformat='8t', dataset=SimpleDataSet)),
        # ('lsm', dict(type='lsm:', keyformat='i', valueformat='i', dataset=SimpleDataSet)),
        ('table-r-complex', dict(type='table:', keyformat='r', valueformat=None,
            dataset=ComplexDataSet)),
        # ('table-i-complex-lsm', dict(type='table:', keyformat='i', valueformat=None,
        #   dataset=ComplexLSMDataSet)),
    ]

    scenarios = make_scenarios(types)

    def populate(self, rownum):
        if self.valueformat != None:
            self.ds = self.dataset(self, self.type + self.tablename, rownum, key_format=self.keyformat, value_format=self.valueformat)
        else:
            self.ds = self.dataset(self, self.type + self.tablename, rownum, key_format=self.keyformat)
        self.ds.populate()

    @wttest.skip_for_hook("timestamp", "fails assertion 99")  # FIXME-WT-9809
    def test_globally_deleted_key(self):
        self.populate(100)

        # Delete the largest key.
        cursor = self.ds.open_cursor(self.type + self.tablename, None)
        self.session.begin_transaction()
        cursor.set_key(100)
        self.assertEqual(cursor.remove(), 0)
        self.session.commit_transaction()

        # Verify the key is not visible.
        self.session.begin_transaction()
        cursor.set_key(100)
        if self.valueformat != '8t':
            self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
        else:
            self.assertEqual(cursor.search(), 0)
        self.session.rollback_transaction()

        # Verify the largest key.
        self.session.begin_transaction()
        self.assertEqual(cursor.largest_key(), 0)
        self.assertEqual(cursor.get_key(), 100)
        self.session.rollback_transaction()

        # Verify the key is still not visible after the largest call.
        self.session.begin_transaction()
        cursor.set_key(100)
        if self.valueformat != '8t':
            self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
        else:
            self.assertEqual(cursor.search(), 0)
        self.session.rollback_transaction()

        # Use evict cursor to evict the key from memory.
        evict_cursor = self.ds.open_cursor(self.type + self.tablename, None, "debug=(release_evict)")
        evict_cursor.set_key(100)
        if self.valueformat != '8t':
            self.assertEquals(evict_cursor.search(), wiredtiger.WT_NOTFOUND)
        else:
            self.assertEquals(evict_cursor.search(), 0)
        evict_cursor.close()

        # Verify the largest key changed.
        self.session.begin_transaction()
        self.assertEqual(cursor.largest_key(), 0)
        if self.valueformat != '8t':
            self.assertEqual(cursor.get_key(), 99)
        else:
            self.assertEquals(cursor.get_key(), 100)
        self.session.rollback_transaction()

    def test_uncommitted_insert(self):
        self.populate(100)

        session2 = self.setUpSessionOpen(self.conn)
        cursor2 = session2.open_cursor(self.type + self.tablename, None)
        session2.begin_transaction()
        cursor2[200] = self.ds.value(200)

        cursor = self.session.open_cursor(self.type + self.tablename, None)

        # Verify the largest key.
        self.session.begin_transaction()
        self.assertEqual(cursor.largest_key(), 0)
        self.assertEqual(cursor.get_key(), 200)
        self.session.rollback_transaction()

        session2.rollback_transaction()

    def test_aborted_insert(self):
        self.populate(100)

        cursor = self.session.open_cursor(self.type + self.tablename, None)
        self.session.begin_transaction()
        cursor[200] = self.ds.value(200)
        self.session.rollback_transaction()

        # Verify the largest key.
        self.session.begin_transaction()
        self.assertEqual(cursor.largest_key(), 0)
        self.assertEquals(cursor.get_key(), 200)
        self.session.rollback_transaction()
    
    def test_invisible_timestamp(self):
        self.populate(100)

        cursor = self.session.open_cursor(self.type + self.tablename, None)
        self.session.begin_transaction()
        cursor[200] = self.ds.value(200)
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(10))

        # Verify the largest key.
        self.session.begin_transaction("read_timestamp=" + self.timestamp_str(5))
        self.assertEqual(cursor.largest_key(), 0)
        self.assertEqual(cursor.get_key(), 200)
        self.session.rollback_transaction()
    
    def test_prepared_update(self):
        self.populate(100)

        session2 = self.setUpSessionOpen(self.conn)
        cursor2 = session2.open_cursor(self.type + self.tablename, None)
        session2.begin_transaction()
        cursor2[200] = self.ds.value(200)
        session2.prepare_transaction("prepare_timestamp=" + self.timestamp_str(10))

        cursor = self.session.open_cursor(self.type + self.tablename, None)

        # Verify the largest key.
        self.session.begin_transaction("read_timestamp=" + self.timestamp_str(20))
        self.assertEqual(cursor.largest_key(), 0)
        self.assertEqual(cursor.get_key(), 200)
        self.session.rollback_transaction()

    def test_not_positioned(self):
        self.populate(100)

        cursor = self.session.open_cursor(self.type + self.tablename, None)
        # Verify the largest key.
        self.session.begin_transaction()
        self.assertEqual(cursor.largest_key(), 0)
        self.assertEqual(cursor.get_key(), 100)

        # Call prev
        self.assertEqual(cursor.prev(), 0)
        self.assertEqual(cursor.get_key(), 100)

        # Verify the largest key again.
        self.assertEqual(cursor.largest_key(), 0)
        self.assertEqual(cursor.get_key(), 100)

        self.assertEqual(cursor.next(), 0)
        self.assertEqual(cursor.get_key(), 1)
        self.session.rollback_transaction()

    def test_get_value(self):
        self.populate(100)

        cursor = self.session.open_cursor(self.type + self.tablename, None)
        # Verify the largest key.
        self.session.begin_transaction()
        self.assertEqual(cursor.largest_key(), 0)
        self.assertEqual(cursor.get_key(), 100)
        with self.expectedStderrPattern("requires value be set"):
            try:
                cursor.get_value()
            except wiredtiger.WiredTigerError as e:
                gotException = True
                self.pr('got expected exception: ' + str(e))
                self.assertTrue(str(e).find('nvalid argument') >= 0)
        self.assertTrue(gotException, msg = 'expected exception')
        self.session.rollback_transaction()

    def test_empty_table(self):
        self.populate(0)

        cursor = self.session.open_cursor(self.type + self.tablename, None)
        # Verify the largest key.
        self.session.begin_transaction()
        self.assertEquals(cursor.largest_key(), wiredtiger.WT_NOTFOUND)
        self.session.rollback_transaction()

    @wttest.prevent(["timestamp"])  # this test uses timestamps, hooks should not
    def test_fast_truncate(self):
        self.populate(100)

        # evict all the pages
        evict_cursor = self.session.open_cursor(self.type + self.tablename, None, "debug=(release_evict)")
        self.session.begin_transaction()
        for i in range(1, 101):
            evict_cursor.set_key(i)
            self.assertEquals(evict_cursor.search(), 0)
        self.session.rollback_transaction()
        evict_cursor.close()

        # truncate
        cursor = self.session.open_cursor(self.type + self.tablename, None)
        self.session.begin_transaction()
        cursor.set_key(1)
        self.session.truncate(None, cursor, None, None)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(5))

        # verify the largest key
        self.session.begin_transaction()
        self.assertEqual(cursor.largest_key(), 0)
        self.assertEqual(cursor.get_key(), 100)
        self.session.rollback_transaction()

    @wttest.prevent(["timestamp"])  # this test uses timestamps, hooks should not
    def test_slow_truncate(self):
        self.populate(100)

        # truncate
        cursor = self.session.open_cursor(self.type + self.tablename, None)
        self.session.begin_transaction()
        cursor.set_key(100)
        self.session.truncate(None, cursor, None, None)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(5))

        # verify the largest key
        self.session.begin_transaction()
        self.assertEqual(cursor.largest_key(), 0)
        self.assertEqual(cursor.get_key(), 100)
        self.session.rollback_transaction()
