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

from rollback_to_stable_util import verify_rts_logs
import wiredtiger, wttest
from wtdataset import SimpleDataSet, ComplexDataSet
from wtscenario import make_scenarios

# test_rollback_to_stable30.py
# Test RTS fails with active transactions and the subsequent transaction resolution succeeds.
class test_rollback_to_stable30(wttest.WiredTigerTestCase):

    format_values = [
        ('table-f', dict(keyfmt='r', valfmt='8t', dataset=SimpleDataSet)),
        ('table-r', dict(keyfmt='r', valfmt='S', dataset=SimpleDataSet)),
        ('table-S', dict(keyfmt='S', valfmt='S', dataset=SimpleDataSet)),
        ('table-r-complex', dict(keyfmt='r', valfmt=None, dataset=ComplexDataSet)),
        ('table-S-complex', dict(keyfmt='S', valfmt=None, dataset=ComplexDataSet)),
    ]

    worker_thread_values = [
        ('0', dict(threads=0)),
        ('4', dict(threads=4)),
        ('8', dict(threads=8))
    ]

    scenarios = make_scenarios(format_values, worker_thread_values)

    # Don't raise errors for these, the expectation is that the RTS verifier will
    # run on the test output.
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.ignoreStdoutPattern('WT_VERB_RTS')
        self.addTearDownAction(verify_rts_logs)

    def conn_config(self):
        return 'verbose=(rts:5)'

    def prepare_resolve(self, resolve):
        ds = self.dataset(self, "table:rts30", 10, key_format=self.keyfmt, value_format=self.valfmt)
        ds.populate()

        # Pin oldest and stable timestamps to 1.
        self.conn.set_timestamp(
            'oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        # Position a cursor.
        cursor = self.session.open_cursor(ds.uri)
        cursor.next()

        # Roll back to stable should fail because there's an active cursor.
        msg = '/rollback_to_stable illegal with active file cursors/'
        with self.expectedStdoutPattern('positioned cursors'):
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                    lambda:self.conn.rollback_to_stable('threads=' + str(self.threads)), msg)

        # Write some data and prepare it.
        self.session.begin_transaction()

        # Modify a row.
        cursor[ds.key(5)] = ds.value(20)

        # Prepare at timestamp 10.
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(10))

        # Roll back to stable should fail because there's an active transaction.
        msg = '/rollback_to_stable illegal with active transactions/'
        with self.expectedStdoutPattern('transaction'):
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda:self.conn.rollback_to_stable('threads=' + str(self.threads)), msg)

        # Set commit, durable timestamps to 20.
        self.session.timestamp_transaction(
            'commit_timestamp=' + self.timestamp_str(20) +
            ',durable_timestamp=' + self.timestamp_str(20))

        # Commit or abort the prepared transaction.
        resolve()

        # Test roll back to stable succeeds after closing all active cursors and transactions.
        self.conn.rollback_to_stable('threads=' + str(self.threads))

    def test_rts_prepare_commit(self):
        self.prepare_resolve(self.session.commit_transaction)

    def test_rts_prepare_rollback(self):
        self.prepare_resolve(self.session.rollback_transaction)
