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
from wtscenario import make_scenarios

# test_flcs02.py
#
# Test various cases of deleting values and expecting them to read back as 0,
# in the presence of timestamps and history.
#
# FUTURE: it would be nice to pin the page to prevent reconciliation until we
# evict it explicitly, to make sure that the first section of the test exercises
# in-memory update records. (Testing on an in-memory database does not have that
# effect.)
class test_flcs02(wttest.WiredTigerTestCase):
    prepare_values = [
        ('no_prepare', dict(do_prepare=False)),
        ('prepare', dict(do_prepare=True))
    ]

    scenarios = make_scenarios(prepare_values)

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
    def delete_readback_abort(self, cursor, k, readts):
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(readts))
        cursor.set_key(k)
        self.assertEqual(cursor.remove(), 0)
        cursor.reset()
        v = cursor[k]
        self.assertEqual(v, 0)
        cursor.reset()
        self.check_next(cursor, k, 0)
        self.check_prev(cursor, k, 0)
        # Delete it again for good measure.
        cursor.set_key(k)
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_value(), 0)
        self.assertEqual(cursor.remove(), 0)
        cursor.reset()
        v = cursor[k]
        self.assertEqual(v, 0)
        cursor.reset()
        self.check_next(cursor, k, 0)
        self.check_prev(cursor, k, 0)
        self.session.rollback_transaction()
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(readts - 5))
        v = cursor[k]
        self.assertEqual(v, k)
        cursor.reset()
        self.session.rollback_transaction()
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(readts))
        v = cursor[k]
        self.assertEqual(v, k)
        cursor.reset()
        self.session.rollback_transaction()
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(readts + 5))
        v = cursor[k]
        self.assertEqual(v, k)
        cursor.reset()
        self.session.rollback_transaction()

    # Delete a value and read it back from a different transaction.
    def delete_readback_commit(self, cursor, k, readts, committs):
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(readts))
        cursor.set_key(k)
        self.assertEqual(cursor.remove(), 0)
        # Remove it again. For FLCS, this should succeed; deleted values are 0, not "deleted".
        cursor.set_key(k)
        self.assertEqual(cursor.remove(), 0)
        cursor.reset()
        if self.do_prepare:
            preparets = committs - 1
            self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(preparets))
            durable = ',durable_timestamp=' + self.timestamp_str(committs + 1)
        else:
            durable = ''
        commit = 'commit_timestamp=' + self.timestamp_str(committs) + durable
        self.session.commit_transaction(commit)

        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(committs - 5))
        v = cursor[k]
        self.assertEqual(v, k)
        cursor.reset()
        self.session.rollback_transaction()

        if self.do_prepare:
            self.session.begin_transaction('read_timestamp=' + self.timestamp_str(preparets))
            v = cursor[k]
            self.assertEqual(v, k)
            cursor.reset()
            self.session.rollback_transaction()

        def readat(readts):
            self.session.begin_transaction('read_timestamp=' + self.timestamp_str(readts))
            v = cursor[k]
            self.assertEqual(v, 0)
            cursor.reset()
            self.check_next(cursor, k, 0)
            self.check_prev(cursor, k, 0)
            self.session.rollback_transaction()

        if not self.do_prepare:
            # Avoid reading between commit and durable.
            readat(committs)
        readat(committs+1)
        readat(committs+5)

    def test_flcs(self):
        uri = "table:test_flcs02"
        nrows = 44
        ds = SimpleDataSet(
            self, uri, 0, key_format='r', value_format='6t', config='leaf_page_max=4096')
        ds.populate()

        updatekey1 = 33
        updatekey2 = 37
        updatekey3 = 21
        updatekey4 = 11
        appendkey1 = nrows + 10
        appendkey2 = nrows + 17

        # Pin oldest and stable to timestamp 1.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        # Write a few records at time 10.
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            cursor[i] = i
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))

        # Doing that might or might not have extended the table before time 10.
        # Accept either behavior, at least for now.
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(5))
        cursor.set_key(1)
        result = cursor.search()
        if result != wiredtiger.WT_NOTFOUND:
            self.assertEqual(result, 0)
            v = cursor.get_value()
            self.assertEqual(v, 0)
        self.session.rollback_transaction()

        # Delete one of the values and read it back in the same transaction, at time 20.
        self.delete_readback_abort(cursor, updatekey1, 20)

        # Append a value, delete it, and read it back, at time 20.
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(20))
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

        # Doing that might or might not have extended the table, including in the past.
        # Accept either behavior, at least for now.
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(10))
        cursor.set_key(appendkey1)
        result = cursor.search()
        if result != wiredtiger.WT_NOTFOUND:
            self.assertEqual(result, 0)
            v = cursor.get_value()
            self.assertEqual(v, 0)
        self.session.rollback_transaction()

        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(20))
        cursor.set_key(appendkey1)
        result = cursor.search()
        if result != wiredtiger.WT_NOTFOUND:
            self.assertEqual(result, 0)
            v = cursor.get_value()
            self.assertEqual(v, 0)
        self.session.rollback_transaction()

        # Now, delete one of the values and read it back from a different transaction.
        self.delete_readback_commit(cursor, updatekey2, 20, 30)

        # Do the same with an append.
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(40))
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
        if self.do_prepare:
            self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(49))
            durable = ',durable_timestamp=' + self.timestamp_str(51)
        else:
            durable = ''
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(50) + durable)

        # This might have extended the table in the past.
        # Accept either behavior, at least for now.
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(20))
        cursor.set_key(appendkey2)
        result = cursor.search()
        if result != wiredtiger.WT_NOTFOUND:
            self.assertEqual(result, 0)
            v = cursor.get_value()
            self.assertEqual(v, 0)
        self.session.rollback_transaction()

        # This should definitely have extended the table in the present.
        read_ts = 51 if self.do_prepare else 50
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(read_ts))
        v = cursor[appendkey2]
        self.assertEqual(v, 0)
        cursor.reset()
        self.check_next(cursor, appendkey2, 0)
        self.check_prev_fail(cursor, appendkey2)
        self.session.rollback_transaction()

        # Evict the page to force reconciliation.
        self.evict(uri, 1, 1)

        # The committed zeros should still be there.
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(read_ts))
        v = cursor[updatekey2]
        self.assertEqual(v, 0)
        cursor.reset()
        self.check_next(cursor, updatekey2, 0)
        self.check_prev(cursor, updatekey2, 0)
        self.session.rollback_transaction()

        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(read_ts))
        v = cursor[appendkey2]
        self.assertEqual(v, 0)
        cursor.reset()
        self.check_next(cursor, appendkey2, 0)
        self.check_prev_fail(cursor, appendkey2)
        self.session.rollback_transaction()

        # Now try both on-page deletes again.
        self.delete_readback_abort(cursor, updatekey3, 60)
        self.delete_readback_commit(cursor, updatekey4, 60, 70)
