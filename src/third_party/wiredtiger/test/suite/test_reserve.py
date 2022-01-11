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
# test_reserve.py
#       Reserve update tests.

import wiredtiger, wttest
from wtdataset import SimpleDataSet, SimpleIndexDataSet
from wtdataset import SimpleLSMDataSet, ComplexDataSet, ComplexLSMDataSet
from wtscenario import make_scenarios

# Test WT_CURSOR.reserve.
class test_reserve(wttest.WiredTigerTestCase):

    format_values = [
        ('integer', dict(keyfmt='i', valfmt='S')),
        ('recno', dict(keyfmt='r', valfmt='S')),
        ('fix', dict(keyfmt='r', valfmt='8t')),
        ('string', dict(keyfmt='S', valfmt='S')),
    ]
    types = [
        ('file', dict(uri='file', ds=SimpleDataSet)),
        ('lsm', dict(uri='lsm', ds=SimpleDataSet)),
        ('table-complex', dict(uri='table', ds=ComplexDataSet)),
        ('table-complex-lsm', dict(uri='table', ds=ComplexLSMDataSet)),
        ('table-index', dict(uri='table', ds=SimpleIndexDataSet)),
        ('table-simple', dict(uri='table', ds=SimpleDataSet)),
        ('table-simple-lsm', dict(uri='table', ds=SimpleLSMDataSet)),
    ]

    def keep(name, d):
        if d['keyfmt'] == 'r' and (d['uri'] == 'lsm' or d['ds'].is_lsm()):
            return False
        # The complex data sets have their own built-in value schemas that are not FLCS.
        if d['valfmt'] == '8t' and d['ds'] == ComplexDataSet:
            return False
        return True

    scenarios = make_scenarios(types, format_values, include=keep)

    def test_reserve(self):
        uri = self.uri + ':test_reserve'

        ds = self.ds(self, uri, 500, key_format=self.keyfmt, value_format=self.valfmt)
        ds.populate()
        s = self.conn.open_session()
        c = s.open_cursor(uri, None)

        # Repeatedly update a record.
        for i in range(1, 5):
            s.begin_transaction()
            c.set_key(ds.key(100))
            c.set_value(ds.value(100))
            self.assertEquals(c.update(), 0)
            s.commit_transaction()

        # Confirm reserve fails if the record doesn't exist.
        s.begin_transaction()
        c.set_key(ds.key(600))
        self.assertRaises(wiredtiger.WiredTigerError, lambda:c.reserve())
        s.rollback_transaction()

        # Repeatedly reserve a record and commit.
        for i in range(1, 5):
            s.begin_transaction()
            c.set_key(ds.key(100))
            self.assertEquals(c.reserve(), 0)
            s.commit_transaction()

        # Repeatedly reserve a record and rollback.
        for i in range(1, 5):
            s.begin_transaction()
            c.set_key(ds.key(100))
            self.assertEquals(c.reserve(), 0)
            s.rollback_transaction()

        # Repeatedly reserve, then update, a record, and commit.
        for i in range(1, 5):
            s.begin_transaction()
            c.set_key(ds.key(100))
            self.assertEquals(c.reserve(), 0)
            c.set_value(ds.value(100))
            self.assertEquals(c.update(), 0)
            s.commit_transaction()

        # Repeatedly reserve, then update, a record, and rollback.
        for i in range(1, 5):
            s.begin_transaction()
            c.set_key(ds.key(100))
            self.assertEquals(c.reserve(), 0)
            c.set_value(ds.value(100))
            self.assertEquals(c.update(), 0)
            s.commit_transaction()

        # Reserve a slot, repeatedly try and update a record from another
        # transaction (which should fail), repeatedly update a record and
        # commit.
        s2 = self.conn.open_session()
        c2 = s2.open_cursor(uri, None)
        for i in range(1, 2):
            s.begin_transaction()
            c.set_key(ds.key(100))
            self.assertEquals(c.reserve(), 0)

            s2.begin_transaction()
            c2.set_key(ds.key(100))
            c2.set_value(ds.value(100))
            self.assertRaises(wiredtiger.WiredTigerError, lambda:c2.update())
            s2.rollback_transaction()

            c.set_key(ds.key(100))
            c.set_value(ds.value(100))
            self.assertEquals(c.update(), 0)
            s.commit_transaction()

    # Test cursor.reserve will fail if a key has not yet been set.
    def test_reserve_without_key(self):

        uri = self.uri + ':test_reserve_without_key'

        ds = self.ds(self, uri, 10, key_format=self.keyfmt, value_format=self.valfmt)
        ds.populate()
        s = self.conn.open_session()
        c = s.open_cursor(uri, None)
        s.begin_transaction()
        msg = "/requires key be set/"
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, lambda:c.reserve(), msg)

    # Test cursor.reserve will fail if there's no running transaction.
    def test_reserve_without_txn(self):

        uri = self.uri + ':test_reserve_without_txn'

        ds = self.ds(self, uri, 10, key_format=self.keyfmt, value_format=self.valfmt)
        ds.populate()
        s = self.conn.open_session()
        c = s.open_cursor(uri, None)
        c.set_key(ds.key(5))
        msg = "/only permitted in a running transaction/"
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, lambda:c.reserve(), msg)

    # Test cursor.reserve returns a value on success.
    def test_reserve_returns_value(self):

        uri = self.uri + ':test_reserve_returns_value'

        ds = self.ds(self, uri, 10, key_format=self.keyfmt, value_format=self.valfmt)
        ds.populate()
        s = self.conn.open_session()
        c = s.open_cursor(uri, None)
        s.begin_transaction()
        c.set_key(ds.key(5))
        self.assertEquals(c.reserve(), 0)
        self.assertEqual(c.get_value(), ds.comparable_value(5))

    # Test cursor.reserve fails on non-standard cursors.
    def test_reserve_not_supported(self):

        uri = self.uri + ':test_reserve_not_supported'
        s = self.conn.open_session()
        s.create(uri, 'key_format=' + self.keyfmt + ",value_format=" + self.valfmt)

        list = [ "bulk", "dump=json" ]
        for l in list:
                c = s.open_cursor(uri, None, l)
                msg = "/Operation not supported/"
                self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                    lambda:self.assertEquals(c.reserve(), 0), msg)
                c.close()

        list = [ "backup:", "config:" "log:" "metadata:" "statistics:" ]
        for l in list:
                c = s.open_cursor(l, None, None)
                msg = "/Operation not supported/"
                self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                    lambda:self.assertEquals(c.reserve(), 0), msg)

if __name__ == '__main__':
    wttest.run()
