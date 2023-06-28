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

import wttest
from wiredtiger import stat
from wtscenario import make_scenarios

# test_hs30.py
#
# Make sure that the history store works as intended for non-timestamped tables, both
# logged and unlogged.
#
# Values are written to the history store when the page is reconciled; the newest
# committed value is written to the data store and any older values that might be needed
# by a reader are sent to history. For non-timestamped values there are two ways this can
# happen: (1) during a checkpoint when a transaction commits after the checkpoint starts
# and we write out some but not all of its updates; and (2) when evicting a page with a
# long-running transaction holding a sufficiently old snapshot open in the background.
#
# In case (1) the history store is used if we crash without another checkpoint by the
# recovery-time rollback to stable. This is tested elsewhere (though not, as of this
# writing, very extensively). It does not appear possible to read the values from the
# history store directly (at runtime) in case (1): starting a read transaction after
# the update transaction commits will not read the old values, and starting a read
# transction before the update transaction commits (and waiting for it) is not really
# different from case (2).
#
# However, case (2) to date is not tested elsewhere. This test checks case (2).
#
# Note that in this case the values are only needed while the reader transaction is
# running, so we don't need to (and have no way to) check if they're still available
# after a crash. Whether they eventually get G/C'd from the history store is another
# question. FUTURE.
#
# If anyone ever implements non-timestamped prepare, this test should be extended to
# cover it.

class test_hs30(wttest.WiredTigerTestCase):
    session_config = 'isolation=snapshot'

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('integer_row', dict(key_format='i', value_format='S')),
    ]
    logging_values = [
        ('nolog', dict(logging=False, conn_config='statistics=(all)')),
        ('log', dict(logging=True, conn_config='statistics=(all),log=(enabled)')),
    ]
    early_ckpt_values = [
        ('early_ckpt', dict(early_ckpt=True)),
        ('no_early_ckpt', dict(early_ckpt=False)),
    ]
    middle_ckpt_values = [
        ('middle_ckpt', dict(middle_ckpt=True)),
        ('no_middle_ckpt', dict(middle_ckpt=False)),
    ]
    evict_values = [
        ('evict', dict(do_evict=True)),
        ('no_evict', dict(do_evict=False)),
    ]

    scenarios = make_scenarios(format_values, logging_values,
        early_ckpt_values, middle_ckpt_values, evict_values)

    def large_updates(self, uri, nrows, value):
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            cursor[i] = value
        self.session.commit_transaction()
        cursor.close()

    def evict(self, uri, nrows, value):
        evict_cursor = self.session.open_cursor(uri, None, 'debug=(release_evict)')
        self.session.begin_transaction()
        # Evict every 5th key; hopefully that's enough. FUTURE: when we have a way to
        # do this iteration one page at a time, do that.
        for i in range(1, nrows + 1, 5):
            self.assertEqual(evict_cursor[1], value)
            evict_cursor.reset()
        self.session.rollback_transaction()
        evict_cursor.close()

    def test_insert_updates_hs(self):
        uri = 'table:test_hs30'
        format = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        logging = 'log=(enabled={})'.format('true' if self.logging else 'false')
        self.session.create(uri, format + ',' + logging)

        nrows = 100

        if self.value_format == '8t':
            value_a = 97
            value_b = 98
            value_c = 99
            value_d = 100
            value_e = 101
        else:
            value_a = 'aaaaa' * 10
            value_b = 'bbbbb' * 10
            value_c = 'ccccc' * 10
            value_d = 'ddddd' * 10
            value_e = 'eeeee' * 10

        session2 = self.conn.open_session()
        cursor2 = session2.open_cursor(uri)
        session3 = self.conn.open_session()
        cursor3 = session3.open_cursor(uri)

        # Write some initial data.
        self.large_updates(uri, nrows, value_a)

        if self.early_ckpt:
            self.session.checkpoint()
        
        # Read in the other session, and then leave the transaction open.
        session2.begin_transaction()
        v = cursor2[1]
        self.assertEqual(v, value_a)
        cursor2.reset()

        # Now churn a bit.
        self.large_updates(uri, nrows, value_b)
        self.large_updates(uri, nrows, value_c)

        if self.middle_ckpt:
            self.session.checkpoint()

        # Start another reader.
        session3.begin_transaction()
        v = cursor3[1]
        self.assertEqual(v, value_c)
        cursor3.reset()

        # Churn some more.
        self.large_updates(uri, nrows, value_d)
        self.large_updates(uri, nrows, value_e)

        if self.do_evict:
            # Evict the lot.
            self.evict(uri, nrows, value_e)

        # Confirm that we still read the right values in the reader transactions.
        for k, v in cursor2:
            self.assertEqual(v, value_a)
        for k, v in cursor3:
            self.assertEqual(v, value_c)

        # Done with these.
        session2.rollback_transaction()
        session3.rollback_transaction()

        # Check via stats that the history store was used.
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        hs_read = stat_cursor[stat.conn.cache_hs_read][2]
        stat_cursor.close()

        if self.do_evict:
            # Should have at least read nrows twice.
            self.assertGreaterEqual(hs_read, nrows * 2)
        else:
            # Apparently even if we checkpoint, without an explicit evict the old in-memory
            # updates hang around and the history store isn't accessed.
            self.assertEqual(hs_read, 0)

        # Tidy up.
        cursor2.close()
        cursor3.close()
