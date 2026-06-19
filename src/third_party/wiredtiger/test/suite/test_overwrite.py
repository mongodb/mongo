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
from wtdataset import SimpleDataSet, SimpleIndexDataSet
from wtdataset import ComplexDataSet
from wtscenario import filter_scenarios, make_scenarios

# cursor overwrite configuration method
class test_overwrite(wttest.WiredTigerTestCase):
    name = 'overwrite'
    keyfmt = [
        ('row', dict(keyfmt='S')),
        ('row-int', dict(keyfmt='i')),
        ('var', dict(keyfmt='r')),
    ]
    valuefmt = 'S'
    types = [
        ('file', dict(uri='file:', ds=SimpleDataSet)),
        ('table-complex', dict(uri='table:', ds=ComplexDataSet)),
        ('table-index', dict(uri='table:', ds=SimpleIndexDataSet)),
        ('table-simple', dict(uri='table:', ds=SimpleDataSet)),
    ]

    # The cursor fast path logic checks against the exact string only, but other valid
    # configurations should also disable overwrite through the normal config parsing flow.
    cursor_configs = [
        ('cfg1', dict(cursor_cfg='overwrite=false', valid=True)),
        ('cfg2', dict(cursor_cfg='overwrite=false,', valid=True)),
        ('cfg3', dict(cursor_cfg='overwrite=false,,,', valid=True)),
        ('cfg4', dict(cursor_cfg=',,,,overwrite=false', valid=True)),
        ('cfg5', dict(cursor_cfg='append=false,overwrite=false', valid=True)),
    ]
    scenarios = make_scenarios(types, keyfmt, cursor_configs)

    # Confirm a cursor configured with/without overwrite correctly handles
    # non-existent records during insert and update operations.
    def test_overwrite_insert(self):
        uri = self.uri + self.name
        ds = self.ds(self, uri, 100, key_format=self.keyfmt, value_format=self.valuefmt)
        ds.populate()

        # Insert of an existing record with overwrite off fails.
        cursor = ds.open_cursor(uri, None, self.cursor_cfg)
        cursor.set_key(ds.key(5))
        cursor.set_value(ds.value(1000))
        self.assertRaises(wiredtiger.WiredTigerError, lambda: cursor.insert())

        # One additional test for the insert method: duplicate the cursor with overwrite
        # configured and then the insert should succeed.  This test is only for the insert method
        # because the update method's failure modes are for non-existent records, and you cannot
        # duplicate a cursor pointing to non-existent records.
        cursor = ds.open_cursor(uri, None, self.cursor_cfg)
        cursor.set_key(ds.key(5))
        if 'layered:' in cursor.uri:
            self.pr("skipping duplicate cursor testing with layered tables")
        else:
            dupc = self.session.open_cursor(None, cursor, "overwrite=true")
            dupc.set_value(ds.value(1001))
            self.assertEqual(dupc.insert(), 0)

        # Insert of an existing record with overwrite on succeeds.
        cursor = ds.open_cursor(uri, None)
        cursor.set_key(ds.key(6))
        cursor.set_value(ds.value(1002))
        self.assertEqual(cursor.insert(), 0)

        # Insert of a non-existent record with overwrite off succeeds.
        cursor = ds.open_cursor(uri, None, self.cursor_cfg)
        cursor.set_key(ds.key(200))
        cursor.set_value(ds.value(1003))
        self.assertEqual(cursor.insert(), 0)

        # Insert of a non-existent record with overwrite on succeeds.
        cursor = ds.open_cursor(uri, None)
        cursor.set_key(ds.key(201))
        cursor.set_value(ds.value(1004))
        self.assertEqual(cursor.insert(), 0)

    # Historically, overwrite applied to cursor.remove as well. Confirm that is no longer the case.
    def test_overwrite_remove(self):
        uri = self.uri + self.name
        ds = self.ds(self, uri, 100, key_format=self.keyfmt, value_format=self.valuefmt)
        ds.populate()

        # Remove of an existing record with overwrite off succeeds.
        cursor = ds.open_cursor(uri, None, self.cursor_cfg)
        cursor.set_key(ds.key(5))
        self.assertEqual(cursor.remove(), 0)

        # Remove of an existing record with overwrite on succeeds.
        cursor = ds.open_cursor(uri, None)
        cursor.set_key(ds.key(6))
        self.assertEqual(cursor.remove(), 0)

        # Remove of a non-existent record with overwrite off fails.
        cursor = ds.open_cursor(uri, None, self.cursor_cfg)
        cursor.set_key(ds.key(200))
        self.assertEqual(cursor.remove(), wiredtiger.WT_NOTFOUND)

        # Remove of a non-existent record with overwrite on fails.
        cursor = ds.open_cursor(uri, None)
        cursor.set_key(ds.key(201))
        self.assertEqual(cursor.remove(), wiredtiger.WT_NOTFOUND)

    def test_overwrite_update(self):
        uri = self.uri + self.name
        ds = self.ds(self, uri, 100, key_format=self.keyfmt, value_format=self.valuefmt)
        ds.populate()

        # Update of an existing record with overwrite off succeeds.
        cursor = ds.open_cursor(uri, None, self.cursor_cfg)
        cursor.set_key(ds.key(5))
        cursor.set_value(ds.value(1005))
        self.assertEqual(cursor.update(), 0)

        # Update of an existing record with overwrite on succeeds.
        cursor = ds.open_cursor(uri, None)
        cursor.set_key(ds.key(6))
        cursor.set_value(ds.value(1006))
        self.assertEqual(cursor.update(), 0)

        # Update of a non-existent record with overwrite off fails.
        cursor = ds.open_cursor(uri, None, self.cursor_cfg)
        cursor.set_key(ds.key(200))
        cursor.set_value(ds.value(1007))
        self.assertEqual(cursor.update(), wiredtiger.WT_NOTFOUND)

        # Update of a non-existent record with overwrite on succeeds.
        cursor = ds.open_cursor(uri, None)
        cursor.set_key(ds.key(201))
        cursor.set_value(ds.value(1008))
        self.assertEqual(cursor.update(), 0)

# test_overwrite_invisible_update
#    A committed update newer than the transaction's read timestamp is invisible to the reader.
#    A write that depends on the key's state must then report:
#      - a write conflict (WT_ROLLBACK)
#      - except insert without overwrite: it sees the still-visible older value and reports
#        a duplicate.
#
#    Scenario per key: insert@10, update@30 (both committed); a read txn at read_ts=20 sees the @10
#    value but not the @30 update. The "insert (no prior value)" row instead commits only insert@30,
#    so at read_ts=20 the key has no visible value and there is no duplicate to detect.
#
#    | method                | overwrite=false  | overwrite=true |
#    +-----------------------+------------------+----------------+
#    | insert (prior value)  | WT_DUPLICATE_KEY | WT_ROLLBACK    |
#    | insert (no prior)     | WT_ROLLBACK      | WT_ROLLBACK    |
#    | update (prior value)  | WT_ROLLBACK      | WT_ROLLBACK    |
#    | update (no prior)     | WT_ROLLBACK      | WT_ROLLBACK    |
#    | modify                | WT_ROLLBACK      | WT_ROLLBACK    |
#    | reserve               | WT_ROLLBACK      | WT_ROLLBACK    |
#    | remove                | WT_ROLLBACK      | WT_ROLLBACK    |
#
class test_overwrite_invisible_update(wttest.WiredTigerTestCase):
    name = 'overwrite_invis'
    types = [
        ('file', dict(uri='file:')),
        ('table', dict(uri='table:')),
    ]
    keyfmt = [
        ('row', dict(keyfmt='S')),
        ('row-int', dict(keyfmt='i')),
        ('var', dict(keyfmt='r')),
    ]
    scenarios = make_scenarios(types, keyfmt)

    def make_ds(self):
        uri = self.uri + self.name
        ds = SimpleDataSet(self, uri, 0, key_format=self.keyfmt, value_format='S')
        ds.create()
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        return ds, uri

    # Commit a value at ts=10 then a newer value at ts=30; the @30 update is invisible at read_ts=20.
    def setup_invisible(self, ds, uri, k):
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        cursor[ds.key(k)] = ds.value(k)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        self.session.begin_transaction()
        cursor[ds.key(k)] = ds.value(k + 1)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))
        cursor.close()

    # Commit only an insert at ts=30; at read_ts=20 the key has no visible value at all.
    def setup_invisible_insert(self, ds, uri, k):
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        cursor[ds.key(k)] = ds.value(k)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))
        cursor.close()

    # Run a write method against key k at read_ts=20, then roll back. Returns the method's result;
    # WT_ROLLBACK / WT_DUPLICATE_KEY surface as exceptions.
    def attempt(self, ds, uri, k, overwrite, method, setup=None):
        (setup or self.setup_invisible)(ds, uri, k)
        cursor = self.session.open_cursor(uri, None, None if overwrite else 'overwrite=false')
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(20))
        cursor.set_key(ds.key(k))
        if method in ('insert', 'update'):
            cursor.set_value(ds.value(k + 2))
        try:
            if method == 'insert':
                return cursor.insert()
            if method == 'update':
                return cursor.update()
            if method == 'modify':
                return cursor.modify([wiredtiger.Modify('X', 0, 1)])
            if method == 'reserve':
                return cursor.reserve()
            if method == 'remove':
                return cursor.remove()
        finally:
            self.session.rollback_transaction()
            cursor.close()

    def assert_rollback(self, ds, uri, k, overwrite, method, setup=None):
        self.assertRaises(wiredtiger.WiredTigerRollbackError,
            lambda: self.attempt(ds, uri, k, overwrite, method, setup))

    def test_invisible_insert(self):
        ds, uri = self.make_ds()
        # overwrite=false: the still-visible @10 value makes this a duplicate.
        self.assertRaisesHavingMessage(
            wiredtiger.WiredTigerError, lambda: self.attempt(ds, uri, 1, False, 'insert'),
            '/WT_DUPLICATE_KEY/')
        # overwrite=true: the blind write conflicts with the invisible committed update.
        self.assert_rollback(ds, uri, 2, True, 'insert')

    def test_invisible_insert_no_prior(self):
        ds, uri = self.make_ds()
        # No visible value: the only committed state is the invisible insert@30, so there is no
        # duplicate to detect and even overwrite=false reaches the conflict check.
        self.assert_rollback(ds, uri, 1, False, 'insert', self.setup_invisible_insert)
        self.assert_rollback(ds, uri, 2, True, 'insert', self.setup_invisible_insert)

    def test_invisible_update(self):
        ds, uri = self.make_ds()
        self.assert_rollback(ds, uri, 1, False, 'update')
        self.assert_rollback(ds, uri, 2, True, 'update')

    def test_invisible_update_no_prior(self):
        ds, uri = self.make_ds()
        # No visible value: update's conflict check still fires on the invisible insert@30 (it would
        # be WT_NOTFOUND only if there were no committed update at all).
        self.assert_rollback(ds, uri, 1, False, 'update', self.setup_invisible_insert)
        self.assert_rollback(ds, uri, 2, True, 'update', self.setup_invisible_insert)

    def test_invisible_modify(self):
        ds, uri = self.make_ds()
        self.assert_rollback(ds, uri, 1, False, 'modify')
        self.assert_rollback(ds, uri, 2, True, 'modify')

    def test_invisible_reserve(self):
        ds, uri = self.make_ds()
        self.assert_rollback(ds, uri, 1, False, 'reserve')
        self.assert_rollback(ds, uri, 2, True, 'reserve')

    def test_invisible_remove(self):
        ds, uri = self.make_ds()
        self.assert_rollback(ds, uri, 1, False, 'remove')
        self.assert_rollback(ds, uri, 2, True, 'remove')
