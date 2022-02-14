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
from wtdataset import SimpleDataSet

# test_flcs01.py
#
# Test various cases of deleting values and expecting them to read back as 0.
#
# FUTURE: it would be nice to pin the page to prevent reconciliation until we
# evict it explicitly, to make sure that the first section of the test exercises
# in-memory update records. (Testing on an in-memory database does not have that
# effect.)
class test_flcs01(wttest.WiredTigerTestCase):
    conn_config = 'in_memory=false'

    # Evict the page to force reconciliation.
    def evict(self, uri, key, check_value):
        evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        v = evict_cursor[key]
        self.assertEqual(v, check_value)
        self.assertEqual(evict_cursor.reset(), 0)
        self.session.rollback_transaction()
        evict_cursor.close()

    def check_next(self, cursor, k, expected_val):
        cursor.set_key(k - 1)
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.next(), 0)
        self.assertEqual(cursor.get_key(), k)
        self.assertEqual(cursor.get_value(), expected_val)
        cursor.reset()

    def check_prev(self, cursor, k, expected_val):
        cursor.set_key(k + 1)
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.prev(), 0)
        self.assertEqual(cursor.get_key(), k)
        self.assertEqual(cursor.get_value(), expected_val)
        cursor.reset()

    def check_prev_fail(self, cursor, k):
        cursor.set_key(k + 1)
        self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
        cursor.reset()

    # Delete a value and read it back in the same transaction.
    def delete_readback_abort(self, cursor, k):
        self.session.begin_transaction()
        cursor.set_key(k)
        self.assertEqual(cursor.remove(), 0)
        cursor.reset()
        v = cursor[k]
        self.assertEqual(v, 0)
        cursor.reset()
        self.check_next(cursor, k, 0)
        self.check_prev(cursor, k, 0)
        self.session.rollback_transaction()
        self.session.begin_transaction()
        v = cursor[k]
        self.assertEqual(v, k)
        cursor.reset()
        self.session.rollback_transaction()

    # Delete a value and read it back from a different transaction.
    def delete_readback_commit(self, cursor, k):
        self.session.begin_transaction()
        cursor.set_key(k)
        self.assertEqual(cursor.remove(), 0)
        cursor.reset()
        self.session.commit_transaction()

        self.session.begin_transaction()
        v = cursor[k]
        self.assertEqual(v, 0)
        cursor.reset()
        self.check_next(cursor, k, 0)
        self.check_prev(cursor, k, 0)
        self.session.rollback_transaction()

    def test_flcs(self):
        uri = "table:test_flcs01"
        nrows = 44
        ds = SimpleDataSet(
            self, uri, nrows, key_format='r', value_format='6t', config='leaf_page_max=4096')
        ds.populate()

        updatekey1 = 33
        updatekey2 = 37
        updatekey3 = 21
        updatekey4 = 11
        appendkey1 = nrows + 10
        appendkey2 = nrows + 17

        # Write a few records.
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            cursor[i] = i
        self.session.commit_transaction()

        # Delete one of the values and read it back in the same transaction.
        self.delete_readback_abort(cursor, updatekey1)

        # Append a value, delete it, and read it back.
        self.session.begin_transaction()
        cursor.set_key(appendkey1)
        cursor.set_value(appendkey1)
        self.assertEqual(cursor.update(), 0)
        cursor.reset()
        v = cursor[appendkey1]
        self.assertEqual(v, appendkey1)
        cursor.reset()
        cursor.set_key(appendkey1)
        self.assertEqual(cursor.remove(), 0)
        cursor.reset()
        v = cursor[appendkey1]
        self.assertEqual(v, 0)
        cursor.reset()
        self.check_next(cursor, appendkey1, 0)
        self.check_prev_fail(cursor, appendkey1)
        self.session.rollback_transaction()

        # Doing that might or might not have extended the table. Accept either behavior,
        # at least for now.
        self.session.begin_transaction()
        cursor.set_key(appendkey1)
        result = cursor.search()
        if result != wiredtiger.WT_NOTFOUND:
            self.assertEqual(result, 0)
            v = cursor.get_value()
            self.assertEqual(v, 0)
        self.session.rollback_transaction()

        # Now, delete one of the values and read it back from a different transaction.
        self.delete_readback_commit(cursor, updatekey2)

        # Do the same with an append.
        self.session.begin_transaction()
        cursor.set_key(appendkey2)
        cursor.set_value(appendkey2)
        self.assertEqual(cursor.update(), 0)
        cursor.reset()
        v = cursor[appendkey2]
        self.assertEqual(v, appendkey2)
        cursor.reset()
        cursor.set_key(appendkey2)
        self.assertEqual(cursor.remove(), 0)
        cursor.reset()
        self.session.commit_transaction()

        # This should definitely have extended the table.
        self.session.begin_transaction()
        v = cursor[appendkey2]
        self.assertEqual(v, 0)
        cursor.reset()
        self.check_next(cursor, appendkey2, 0)
        self.check_prev_fail(cursor, appendkey2)
        self.session.rollback_transaction()

        # Evict the page to force reconciliation.
        self.evict(uri, 1, 1)

        # The committed zeros should still be there.
        self.session.begin_transaction()
        v = cursor[updatekey2]
        self.assertEqual(v, 0)
        cursor.reset()
        self.check_next(cursor, updatekey2, 0)
        self.check_prev(cursor, updatekey2, 0)
        self.session.rollback_transaction()

        self.session.begin_transaction()
        v = cursor[appendkey2]
        self.assertEqual(v, 0)
        cursor.reset()
        self.check_next(cursor, appendkey2, 0)
        self.check_prev_fail(cursor, appendkey2)
        self.session.rollback_transaction()

        # Now try both on-page deletes again.
        self.delete_readback_abort(cursor, updatekey3)
        self.delete_readback_commit(cursor, updatekey4)
