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
# test_txn09.py
#   Transactions: recovery toggling logging
#

from suite_subprocess import suite_subprocess
from wtscenario import make_scenarios
import wttest

class test_txn09(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_txn09'
    uri = 'table:' + tablename
    log_enabled = True

    types = [
        ('row', dict(tabletype='row',
                    create_params = 'key_format=i,value_format=i')),
        ('var', dict(tabletype='var',
                    create_params = 'key_format=r,value_format=i')),
        ('fix', dict(tabletype='fix',
                    create_params = 'key_format=r,value_format=8t')),
    ]
    op1s = [
        ('i4', dict(op1=('insert', 4))),
        ('r1', dict(op1=('remove', 1))),
        ('u10', dict(op1=('update', 10))),
    ]
    op2s = [
        ('i6', dict(op2=('insert', 6))),
        ('r4', dict(op2=('remove', 4))),
        ('u4', dict(op2=('update', 4))),
    ]
    op3s = [
        ('i12', dict(op3=('insert', 12))),
        ('r4', dict(op3=('remove', 4))),
        ('u4', dict(op3=('update', 4))),
    ]
    op4s = [
        ('i14', dict(op4=('insert', 14))),
        ('r12', dict(op4=('remove', 12))),
        ('u12', dict(op4=('update', 12))),
    ]
    txn1s = [('t1c', dict(txn1='commit')), ('t1r', dict(txn1='rollback'))]
    txn2s = [('t2c', dict(txn2='commit')), ('t2r', dict(txn2='rollback'))]
    txn3s = [('t3c', dict(txn3='commit')), ('t3r', dict(txn3='rollback'))]
    txn4s = [('t4c', dict(txn4='commit')), ('t4r', dict(txn4='rollback'))]

    # This test generates thousands of potential scenarios.
    # For default runs, we'll use a small subset of them, for
    # long runs (when --long is set) we'll set a much larger limit.
    scenarios = make_scenarios(types,
        op1s, txn1s, op2s, txn2s, op3s, txn3s, op4s, txn4s,
        prune=20, prunelong=5000)

    def conn_config(self):
        return 'log=(enabled=%s,remove=false),' % int(self.log_enabled) + \
            'transaction_sync=(enabled=false)'

    # Check that a cursor (optionally started in a new transaction), sees the
    # expected values.
    def check(self, session, txn_config, expected):
        if txn_config:
            session.begin_transaction(txn_config)
        c = session.open_cursor(self.uri, None)
        actual = dict((k, v) for k, v in c if v != 0)
        # Search for the expected items as well as iterating
        for k, v in expected.items():
            self.assertEqual(c[k], v)
        c.close()
        if txn_config:
            session.commit_transaction()
        self.assertEqual(actual, expected)

    # Check the state of the system with respect to the current cursor and
    # different isolation levels.
    def check_all(self, current, committed):
        # Transactions see their own changes.
        # Read-uncommitted transactions see all changes.
        # Snapshot and read-committed transactions should not see changes.
        self.check(self.session, None, current)
        self.check(self.session2, "isolation=snapshot", committed)
        self.check(self.session2, "isolation=read-committed", committed)
        self.check(self.session2, "isolation=read-uncommitted", current)

    def test_ops(self):
        # print "Creating %s with config '%s'" % (self.uri, self.create_params)
        self.session.create(self.uri, self.create_params)
        # Set up the table with entries for 1, 2, 10 and 11.
        # We use the overwrite config so insert can update as needed.
        c = self.session.open_cursor(self.uri, None, 'overwrite')
        c[1] = c[2] = c[10] = c[11] = 1
        current = {1:1, 2:1, 10:1, 11:1}
        committed = current.copy()

        ops = (self.op1, self.op2, self.op3, self.op4)
        txns = (self.txn1, self.txn2, self.txn3, self.txn4)
        # for ok, txn in zip(ops, txns):
        # print ', '.join('%s(%d)[%s]' % (ok[0], ok[1], txn)
        for i, ot in enumerate(zip(ops, txns)):
            ok, txn = ot
            op, k = ok

            # Close and reopen the connection and cursor, toggling the log
            self.log_enabled = not self.log_enabled
            self.reopen_conn()
            self.session2 = self.conn.open_session()
            c = self.session.open_cursor(self.uri, None, 'overwrite')

            self.session.begin_transaction(
                (self.scenario_number % 2) and 'sync' or None)
            # Test multiple operations per transaction by always
            # doing the same operation on key k + 1.
            k1 = k + 1
            # print '%d: %s(%d)[%s]' % (i, ok[0], ok[1], txn)
            if op == 'insert' or op == 'update':
                c[k] = c[k1] = i + 2
                current[k] = current[k1] = i + 2
            elif op == 'remove':
                c.set_key(k)
                c.remove()
                c.set_key(k1)
                c.remove()
                if k in current:
                    del current[k]
                if k1 in current:
                    del current[k1]

            # print current
            # Check the state after each operation.
            self.check_all(current, committed)

            if txn == 'commit':
                committed = current.copy()
                self.session.commit_transaction()
            elif txn == 'rollback':
                current = committed.copy()
                self.session.rollback_transaction()

            # Check the state after each commit/rollback.
            self.check_all(current, committed)

if __name__ == '__main__':
    wttest.run()
