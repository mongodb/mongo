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

# test_flcs03.py
#
# Test various cases associated with the location of the end of the table.
#
# FUTURE: it would be nice to pin the page to prevent reconciliation in various
# places, to make sure that we're testing on in-memory update records when we intend
# to be. (We do test on an in-memory database, but that isn't sufficient by itself;
# pages are still reconciled and that can eliminate the update configuration we're
# trying to test.
class test_flcs03(wttest.WiredTigerTestCase):

    in_memory_values = [
        ('no_inmem', dict(in_memory=False)),
        ('inmem', dict(in_memory=True))
    ]

    # Test scenarios (all appends, obviously):
    #   - pending uncommitted updates, before or after reconciliation
    #   - prepared updates, before or after reconciliation
    #   - committed updates that are deletes, before or after reconciliation
    #   - aborted updates, right away while the update records still exist
    #   - aborted updates, if reconciled right away while the update records still exist
    #   - aborted updates after the update records have been G/C'd
    #   - aborted updates, if reconciled after the update records have been G/C'd
    #     (this last is not that interesting but it follows naturally from the scenario generation)
    #
    # The case of reading at a time before some committed updates is tested
    # on every run using the initial update set.
    operation_values = [
        ('uncommitted', dict(op='uncommitted', prepare=False, gc=False)),
        ('prepared', dict(op='uncommitted', prepare=True, gc=False)),
        ('committed_deletes', dict(op='deleted', prepare=False, gc=False)),
        ('committed_prepared_deletes', dict(op='deleted', prepare=True, gc=False)),
        ('aborted', dict(op='aborted', prepare=False, gc=False)),
        ('aborted_GC', dict(op='aborted', prepare=False, gc=True)),
    ]

    reconcile_values = [
        ('no_reconcile', dict(reconcile=False)),
        ('reconcile', dict(reconcile=True)),
    ]

    scenarios = make_scenarios(in_memory_values, operation_values, reconcile_values)

    def conn_config(self):
        if self.in_memory:
            return 'in_memory=true'
        else:
            return 'in_memory=false'

    # Evict the page to force reconciliation.
    def evict(self, uri, key, check_value):
        evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        v = evict_cursor[key]
        self.assertEqual(v, check_value)
        self.assertEqual(evict_cursor.reset(), 0)
        self.session.rollback_transaction()
        evict_cursor.close()

    def check_end(self, cursor, hint, ts, expected_last_key):
        readtime = 'read_timestamp=' + self.timestamp_str(ts)
        self.session.begin_transaction(readtime + ',ignore_prepare=true')
        cursor.reset()
        if hint is not None:
            cursor.set_key(hint)
            self.assertEqual(cursor.search(), 0)
        last = None
        while cursor.next() != wiredtiger.WT_NOTFOUND:
            last = cursor.get_key()
        self.assertEqual(last, expected_last_key)
        cursor.reset()
        self.assertEqual(cursor.prev(), 0)
        last = cursor.get_key()
        self.assertEqual(last, expected_last_key)
        self.session.rollback_transaction()

    def check_end_empty(self, cursor, ts):
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(ts))
        cursor.reset()
        self.assertEqual(cursor.next(), wiredtiger.WT_NOTFOUND)
        cursor.reset()
        self.assertEqual(cursor.prev(), wiredtiger.WT_NOTFOUND)
        self.session.rollback_transaction()

    def test_flcs(self):
        uri = "table:test_flcs03"
        nrows = 10
        ds = SimpleDataSet(self, uri,
            0, key_format='r', value_format='6t', config='leaf_page_max=4096,log=(enabled=false)')
        ds.populate()

        # Pin oldest and stable to timestamp 1.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        cursor = self.session.open_cursor(uri)

        # The table should end without any keys.
        self.check_end_empty(cursor, 5)

        # Write a few records at time 10.
        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            cursor[i] = i
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))

        # Current behavior: this extends the table before the commit as well as at/after.
        self.check_end(cursor, None, 5, nrows)
        self.check_end(cursor, None, 10, nrows)
        self.check_end(cursor, None, 11, nrows)

        # Make another session to do the operation in.
        session2 = self.conn.open_session()
        cursor2 = session2.open_cursor(uri)

        append_key = nrows * 2
        append_key_lite = nrows + 2

        # Do the requested operation.
        if self.op == 'uncommitted':
            session2.begin_transaction('read_timestamp=' + self.timestamp_str(15))
            cursor2[append_key] = 20
        elif self.op == 'deleted':
            session2.begin_transaction('read_timestamp=' + self.timestamp_str(15))
            cursor2[append_key] = 21
            cursor2.set_key(append_key)
            self.assertEqual(cursor2.remove(), 0)
        else:
            self.assertEqual(self.op, 'aborted')
            session2.begin_transaction('read_timestamp=' + self.timestamp_str(15))
            cursor2[append_key] = 22
            session2.rollback_transaction()

        if self.prepare:
            session2.prepare_transaction('prepare_timestamp=' + self.timestamp_str(19))

        if self.op == 'deleted':
            committime = 'commit_timestamp=' + self.timestamp_str(20)
            if self.prepare:
                durabletime = ',durable_timestamp=' + self.timestamp_str(21)
            else:
                durabletime = ''
            session2.commit_transaction(committime + durabletime)

        if self.gc:
            session2.begin_transaction('read_timestamp=' + self.timestamp_str(25))
            # Churn the append list enough to clean out the previous aborted updates.
            for i in range(0, 20):
                cursor2[append_key_lite] = 30 + i
                cursor2[append_key_lite + 1] = 31 + i
            session2.commit_transaction('commit_timestamp=' + self.timestamp_str(30))

        if self.reconcile:
            # This will all fit on one page, so we only need to evict once.
            self.evict(uri, nrows, nrows)

        # For aborted without G/C, we could lose the aborted update records at any point, so
        # do the most interesting checks first.
        #
        # Current behavior for all cases is that the table extends out to the append key,
        # including in the past.
        #
        # Without a way to prevent reconciliation it's not entirely clear that the aborted
        # with G/C case doesn't get reconciled (which currently physically extends the table
        # in a way that cannot be rolled back) before the updates are tossed. If that happens,
        # the table definitely extends. If it doesn't, it might not. Maybe.
        if self.op == 'aborted' and not self.gc:
            self.check_end(cursor, nrows-1, 30, append_key)
            self.check_end(cursor, nrows-1, 10, append_key)
            self.check_end(cursor, None, 5, append_key)
        else:
            self.check_end(cursor, None, 5, append_key)
            self.check_end(cursor, None, 10, append_key)
            self.check_end(cursor, None, 19, append_key)
            self.check_end(cursor, None, 20, append_key)
            self.check_end(cursor, None, 21, append_key)
            self.check_end(cursor, None, 30, append_key)
