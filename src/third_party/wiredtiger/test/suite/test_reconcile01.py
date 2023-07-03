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
from wtscenario import make_scenarios
from wtdataset import SimpleDataSet

# test_reconcile01.py
#
# Test scenarios of removing a non existing key/recno leading to implicit deleted
# records only to the fixed length column store. Performing eviction on the page
# when they are globally visible or not and expecting them to read back as 0.
class test_reconcile01(wttest.WiredTigerTestCase):
    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column-fix', dict(key_format='r', value_format='8t')),
        ('integer-row', dict(key_format='i', value_format='S')),
    ]

    long_running_txn_values = [
       ('long-running', dict(long_run_txn=True)),
       ('no-long-running', dict(long_run_txn=False))
    ]

    scenarios = make_scenarios(format_values, long_running_txn_values)

    # Evict the page to force reconciliation.
    def evict(self, ds, uri, key, check_value):
        evict_cursor = ds.open_cursor(uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        v = evict_cursor[key]
        self.assertEqual(v, check_value)
        self.assertEqual(evict_cursor.reset(), 0)
        self.session.rollback_transaction()
        evict_cursor.close()

    def test_reconcile(self):
        uri = "table:test_reconcile01"
        nrows = 44
        ds = SimpleDataSet(
            self, uri, nrows, key_format=self.key_format, value_format=self.value_format, config='leaf_page_max=4096')
        ds.populate()

        appendkey1 = nrows + 10
        appendkey2 = nrows + 17

        # Write a few records.
        cursor = ds.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            if self.value_format == '8t':
                cursor[i] = i
            else:
                cursor[i] = str(i)
        self.session.commit_transaction()

        # Start a long running transaction.
        if self.long_run_txn:
            session2 = self.conn.open_session()
            session2.begin_transaction()

        # Append a new key is necessary otherwise the next remove fails without inserting
        # the implicitly deleted record.
        cursor.set_key(appendkey2)
        if self.value_format == '8t':
            cursor.set_value(appendkey2)
        else:
            cursor.set_value(str(appendkey2))
        self.assertEqual(cursor.insert(), 0)

        # Remove the key that doesn't exist.
        cursor.set_key(appendkey1)
        if self.value_format == '8t':
            self.assertEqual(cursor.remove(), 0)
        else:
            self.assertEqual(cursor.remove(), wiredtiger.WT_NOTFOUND)

        # Validate the appended and removed keys.
        cursor.set_key(appendkey1)
        if self.value_format == '8t':
            self.assertEqual(cursor.search(), 0)
            self.assertEqual(cursor.get_value(), 0)
        else:
            self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)

        v = cursor[appendkey2]
        if self.value_format == '8t':
            self.assertEqual(v, appendkey2)
        else:
            self.assertEqual(v, str(appendkey2))
        cursor.reset()

        # Evict the page to force reconciliation. As part of the reconciliation selecting the
        # implicit tombstone can go wrong for FLCS if it is not globally visible.
        if self.value_format == '8t':
            self.evict(ds, uri, 1, 1)
        else:
            self.evict(ds, uri, 1, '1')

        # Validate the appended and removed keys.
        cursor.set_key(appendkey1)
        if self.value_format == '8t':
            self.assertEqual(cursor.search(), 0)
            self.assertEqual(cursor.get_value(), 0)
        else:
            self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)

        v = cursor[appendkey2]
        if self.value_format == '8t':
            self.assertEqual(v, appendkey2)
        else:
            self.assertEqual(v, str(appendkey2))

