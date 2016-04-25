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
# test_txn04.py
#   Transactions: hot backup and recovery
#

import shutil, os
from suite_subprocess import suite_subprocess
from wtscenario import multiply_scenarios, number_scenarios
import wttest

class test_txn04(wttest.WiredTigerTestCase, suite_subprocess):
    logmax = "100K"
    tablename = 'test_txn04'
    uri = 'table:' + tablename
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
        ('insert', dict(op1=('insert', 6))),
        ('update', dict(op1=('update', 2))),
        ('remove', dict(op1=('remove', 2))),
        ('trunc-stop', dict(op1=('stop', 2))),
    ]
    txn1s = [('t1c', dict(txn1='commit')), ('t1r', dict(txn1='rollback'))]

    scenarios = number_scenarios(multiply_scenarios('.', types, op1s, txn1s))
    # Overrides WiredTigerTestCase
    def setUpConnectionOpen(self, dir):
        self.home = dir
        # Cycle through the different transaction_sync values in a
        # deterministic manner.
        self.txn_sync = self.sync_list[
            self.scenario_number % len(self.sync_list)]
        self.backup_dir = os.path.join(self.home, "WT_BACKUP")
        # Set archive false on the home directory.
        conn_params = \
                'log=(archive=false,enabled,file_max=%s),' % self.logmax + \
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

    def hot_backup(self, backup_uri, committed):
        # If we are backing up a target, assume the directory exists.
        # We just use the wt backup command.
        # A future test extension could also use a cursor.
        cmd = '-h ' + self.home + ' backup '
        if backup_uri != None:
            cmd += '-t ' + backup_uri + ' '
        else:
            shutil.rmtree(self.backup_dir, ignore_errors=True)
            os.mkdir(self.backup_dir)

        cmd += self.backup_dir
        self.runWt(cmd.split())
        backup_conn_params = 'log=(enabled,file_max=%s)' % self.logmax
        backup_conn = self.wiredtiger_open(self.backup_dir, backup_conn_params)
        try:
            self.check(backup_conn.open_session(), None, committed)
        finally:
            backup_conn.close()

    def ops(self):
        self.session.create(self.uri, self.create_params)
        c = self.session.open_cursor(self.uri, None, 'overwrite')
        # Set up the table with entries for 1-5.
        # We then truncate starting or ending in various places.
        # We use the overwrite config so insert can update as needed.
        current = {1:1, 2:1, 3:1, 4:1, 5:1}
        for k in current:
            c[k] = 1
        committed = current.copy()

        ops = (self.op1, )
        txns = (self.txn1, )
        for i, ot in enumerate(zip(ops, txns)):
            # Perform a full hot backup of the original tables.
            # The runWt command closes our connection and sessions so
            # we need to reopen them here.
            self.hot_backup(None, committed)
            c = self.session.open_cursor(self.uri, None, 'overwrite')
            c.set_value(1)
            # Then do the given modification.
            # Perform a targeted hot backup.
            self.session.begin_transaction()
            ok, txn = ot
            op, k = ok

            # print '%d: %s(%d)[%s]' % (i, ok[0], ok[1], txn)
            if op == 'insert' or op == 'update':
                c[k] = i + 2
                current[k] = i + 2
            elif op == 'remove':
                c.set_key(k)
                c.remove()
                if k in current:
                    del current[k]
            elif op == 'stop':
                # For both, the key given is the start key.  Add 2
                # for the stop key.
                c.set_key(k)
                kstart = 1
                kstop = k
                self.session.truncate(None, None, c, None)
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

        # Backup the target we modified and verify the data.
        # print 'Call hot_backup with ' + self.uri
        self.hot_backup(self.uri, committed)

    def test_ops(self):
        with self.expectedStdoutPattern('recreating metadata'):
            self.ops()

if __name__ == '__main__':
    wttest.run()
