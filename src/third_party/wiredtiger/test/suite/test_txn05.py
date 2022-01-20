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
# test_txn05.py
# Transactions: commits and rollbacks
#

import fnmatch, os, shutil, time
from suite_subprocess import suite_subprocess
from wtscenario import make_scenarios
import wttest

class test_txn05(wttest.WiredTigerTestCase, suite_subprocess):
    logmax = "100K"
    tablename = 'test_txn05'
    uri = 'table:' + tablename
    remove_list = ['true', 'false']
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
        ('trunc-all', dict(op1=('all', 0))),
        ('trunc-both', dict(op1=('both', 2))),
        ('trunc-start', dict(op1=('start', 2))),
        ('trunc-stop', dict(op1=('stop', 2))),
    ]
    txn1s = [('t1c', dict(txn1='commit')), ('t1r', dict(txn1='rollback'))]

    scenarios = make_scenarios(types, op1s, txn1s)

    def conn_config(self):
        # Cycle through the different transaction_sync values in a
        # deterministic manner.
        txn_sync = self.sync_list[
            self.scenario_number % len(self.sync_list)]
        # Set remove=false on the home directory.
        return 'log=(enabled,file_max=%s,remove=false),' % self.logmax + \
            'transaction_sync="%s",' % txn_sync

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

        # Opening a clone of the database home directory should run
        # recovery and see the committed results.  Flush the log because
        # the backup may not get all the log records if we are running
        # without a sync option.  Use sync=off to force a write to the OS.
        self.session.log_flush('sync=off')
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
        # repeated recovery and log removal even if later recoveries
        # are essentially no-ops. Confirm that the backup contains
        # the committed operations after recovery.
        #
        # Cycle through the different remove values in a deterministic manner.
        self.remove = self.remove_list[
            self.scenario_number % len(self.remove_list)]
        backup_conn_params = \
            'log=(enabled,file_max=%s,remove=%s)' % (self.logmax, self.remove)
        orig_logs = fnmatch.filter(os.listdir(self.backup_dir), "*gerLog*")
        endcount = 2
        count = 0
        while count < endcount:
            backup_conn = self.wiredtiger_open(self.backup_dir,
                                               backup_conn_params)
            try:
                 session = backup_conn.open_session()
            finally:
                self.check(session, None, committed)
                # Sleep long enough so that the removal thread is guaranteed
                # to run before we close the connection.
                time.sleep(1.0)
                if count == 0:
                    first_logs = \
                        fnmatch.filter(os.listdir(self.backup_dir), "*gerLog*")
                backup_conn.close()
            count += 1
        #
        # Check logs after repeated openings. The first log should
        # have been removed if configured. Subsequent openings would not
        # be removed because no checkpoint is written due to no modifications.
        #
        cur_logs = fnmatch.filter(os.listdir(self.backup_dir), "*gerLog*")
        for o in orig_logs:
            # Creating the backup was effectively an unclean shutdown so
            # even after sleeping, we should never remove log files
            # because a checkpoint has not run.  Later opens and runs of
            # recovery will detect a clean shutdown and allow removal.
            self.assertEqual(True, o in first_logs)
            if self.remove == 'true':
                self.assertEqual(False, o in cur_logs)
            else:
                self.assertEqual(True, o in cur_logs)
        #
        # Run printlog and make sure it exits with zero status.
        #
        self.runWt(['-h', self.backup_dir, 'printlog'], outfilename='printlog.out')

    def test_ops(self):
        self.backup_dir = os.path.join(self.home, "WT_BACKUP")
        self.session2 = self.conn.open_session()
        # print "Creating %s with config '%s'" % (self.uri, self.create_params)
        self.session.create(self.uri, self.create_params)
        # Set up the table with entries for 1-5.
        # We then truncate starting or ending in various places.
        c = self.session.open_cursor(self.uri, None)
        current = {1:1, 2:1, 3:1, 4:1, 5:1}
        for k in current:
            c[k] = 1
        committed = current.copy()

        ops = (self.op1, )
        txns = (self.txn1, )
        for i, ot in enumerate(zip(ops, txns)):
            self.session.begin_transaction()
            ok, txn = ot
            # print '%d: %s(%d)[%s]' % (i, ok[0], ok[1], txn)
            op, k = ok

            # print '%d: %s(%d)[%s]' % (i, ok[0], ok[1], txn)
            if op == 'stop':
                c.set_key(k)
                self.session.truncate(None, None, c, None)
                kstart = 1
                kstop = k
            elif op == 'start':
                c.set_key(k)
                self.session.truncate(None, c, None, None)
                kstart = k
                kstop = len(current)
            elif op == 'both':
                c2 = self.session.open_cursor(self.uri, None)
                # For both, the key given is the start key.  Add 2
                # for the stop key.
                kstart = k
                kstop = k + 2
                c.set_key(kstart)
                c2.set_key(kstop)
                self.session.truncate(None, c, c2, None)
                c2.close()
            elif op == 'all':
                c2 = self.session.open_cursor(self.uri, None)
                kstart = 1
                kstop = len(current)
                c.set_key(kstart)
                c2.set_key(kstop)
                self.session.truncate(None, c, c2, None)
                c2.close()

            while (kstart <= kstop):
                del current[kstart]
                kstart += 1

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

        # Check the log state after the entire op completes
        # and run recovery.
        if self.scenario_number % (len(test_txn05.scenarios) // 100 + 1) == 0:
            self.check_log(committed)

if __name__ == '__main__':
    wttest.run()
