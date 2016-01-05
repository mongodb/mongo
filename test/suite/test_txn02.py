#!/usr/bin/env python
#
# Public Domain 2014-2016 MongoDB, Inc.
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
# test_txn02.py
#   Transactions: commits and rollbacks
#

import fnmatch, os, shutil, time
from suite_subprocess import suite_subprocess
from wtscenario import multiply_scenarios, number_scenarios, prune_scenarios
import wttest

class test_txn02(wttest.WiredTigerTestCase, suite_subprocess):
    logmax = "100K"
    tablename = 'test_txn02'
    uri = 'table:' + tablename
    archive_list = ['true', 'false']
    conn_list = ['reopen', 'stay_open']
    sync_list = [
        '(method=dsync,enabled)',
        '(method=fsync,enabled)',
        '(method=none,enabled)',
        '(enabled=false)'
    ]

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

    all_scenarios = multiply_scenarios('.', types,
        op1s, txn1s, op2s, txn2s, op3s, txn3s, op4s, txn4s)

    # This test generates thousands of potential scenarios.
    # For default runs, we'll use a small subset of them, for
    # long runs (when --long is set) we'll set a much larger limit.
    scenarios = number_scenarios(prune_scenarios(all_scenarios, 20, 5000))

    # Each check_log() call takes a second, so we don't call it for
    # every scenario, we'll limit it to the value of checklog_calls.
    checklog_calls = 100 if wttest.islongtest() else 2
    checklog_mod = (len(scenarios) / checklog_calls + 1)

    # scenarios = number_scenarios(multiply_scenarios('.', types,
    # op1s, txn1s, op2s, txn2s, op3s, txn3s, op4s, txn4s)) [:3]
    # Overrides WiredTigerTestCase
    def setUpConnectionOpen(self, dir):
        self.home = dir
        # Cycle through the different transaction_sync values in a
        # deterministic manner.
        self.txn_sync = self.sync_list[
            self.scenario_number % len(self.sync_list)]
        #
        # We don't want to run zero fill with only the same settings, such
        # as archive or sync, which are an even number of options.
        #
        freq = 3
        zerofill = 'false'
        if self.scenario_number % freq == 0:
            zerofill = 'true'
        self.backup_dir = os.path.join(self.home, "WT_BACKUP")
        conn_params = \
                'log=(archive=false,enabled,file_max=%s),' % self.logmax + \
                'log=(zero_fill=%s),' % zerofill + \
                'create,error_prefix="%s: ",' % self.shortid() + \
                'transaction_sync="%s",' % self.txn_sync
        # print "Creating conn at '%s' with config '%s'" % (dir, conn_params)
        conn = self.wiredtiger_open(dir, conn_params)
        self.pr(`conn`)
        self.session2 = conn.open_session()
        return conn

    # Check that a cursor (optionally started in a new transaction), sees the
    # expected values.
    def check(self, session, txn_config, expected):
        if txn_config:
            session.begin_transaction(txn_config)
        c = session.open_cursor(self.uri, None)
        actual = dict((k, v) for k, v in c if v != 0)
        # Search for the expected items as well as iterating
        for k, v in expected.iteritems():
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

        # Opening a clone of the database home directory should run
        # recovery and see the committed results.
        self.backup(self.backup_dir)
        backup_conn_params = 'log=(enabled,file_max=%s)' % self.logmax
        backup_conn = self.wiredtiger_open(self.backup_dir, backup_conn_params)
        try:
            self.check(backup_conn.open_session(), None, committed)
        finally:
            backup_conn.close()

    def check_log(self, committed):
        self.backup(self.backup_dir)
        #
        # Open and close the backup connection a few times to force
        # repeated recovery and log archiving even if later recoveries
        # are essentially no-ops. Confirm that the backup contains
        # the committed operations after recovery.
        #
        # Cycle through the different archive values in a
        # deterministic manner.
        self.archive = self.archive_list[
            self.scenario_number % len(self.archive_list)]
        backup_conn_params = \
            'log=(enabled,file_max=%s,archive=%s)' % (self.logmax, self.archive)
        orig_logs = fnmatch.filter(os.listdir(self.backup_dir), "*Log*")
        endcount = 2
        count = 0
        while count < endcount:
            backup_conn = self.wiredtiger_open(self.backup_dir,
                                               backup_conn_params)
            try:
                self.check(backup_conn.open_session(), None, committed)
            finally:
                # Sleep long enough so that the archive thread is guaranteed
                # to run before we close the connection.
                time.sleep(1.0)
                backup_conn.close()
            count += 1
        #
        # Check logs after repeated openings. The first log should
        # have been archived if configured. Subsequent openings would not
        # archive because no checkpoint is written due to no modifications.
        #
        cur_logs = fnmatch.filter(os.listdir(self.backup_dir), "*Log*")
        for o in orig_logs:
            if self.archive == 'true':
                self.assertEqual(False, o in cur_logs)
            else:
                self.assertEqual(True, o in cur_logs)
        #
        # Run printlog and make sure it exits with zero status.
        # Printlog should not run recovery nor advance the logs.  Make sure
        # it does not.
        #
        self.runWt(['-h', self.backup_dir, 'printlog'], outfilename='printlog.out')
        pr_logs = fnmatch.filter(os.listdir(self.backup_dir), "*Log*")
        self.assertEqual(cur_logs, pr_logs)

    def test_ops(self):
        # print "Creating %s with config '%s'" % (self.uri, self.create_params)
        self.session.create(self.uri, self.create_params)
        # Set up the table with entries for 1, 2, 10 and 11.
        # We use the overwrite config so insert can update as needed.
        c = self.session.open_cursor(self.uri, None, 'overwrite')
        c[1] = c[2] = c[10] = c[11] = 1
        current = {1:1, 2:1, 10:1, 11:1}
        committed = current.copy()

        reopen = self.conn_list[
            self.scenario_number % len(self.conn_list)]
        ops = (self.op1, self.op2, self.op3, self.op4)
        txns = (self.txn1, self.txn2, self.txn3, self.txn4)
        # for ok, txn in zip(ops, txns):
        # print ', '.join('%s(%d)[%s]' % (ok[0], ok[1], txn)
        for i, ot in enumerate(zip(ops, txns)):
            ok, txn = ot
            op, k = ok

            # Close and reopen the connection and cursor.
            if reopen == 'reopen':
                self.reopen_conn()
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

        # check_log() is slow, we don't run it on every scenario.
        if self.scenario_number % test_txn02.checklog_mod == 0:
            self.check_log(committed)

if __name__ == '__main__':
    wttest.run()
