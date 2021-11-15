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
# test_timestamp06.py
#   Timestamps: multistep transactions
#

from helper import copy_wiredtiger_home
import random
from suite_subprocess import suite_subprocess
import wiredtiger, wttest
from wtscenario import make_scenarios

class test_timestamp06(wttest.WiredTigerTestCase, suite_subprocess):
    table_ts_log     = 'table:ts06_ts_logged'
    table_ts_nolog   = 'table:ts06_ts_nologged'

    types = [
        ('col_fix', dict(empty=1, extra_config=',key_format=r,value_format=8t')),
        ('col_var', dict(empty=0, extra_config=',key_format=r')),
        ('lsm', dict(empty=0, extra_config=',type=lsm')),
        ('row', dict(empty=0, extra_config='',)),
    ]
    ckpt = [
        ('ckpt_ts_def', dict(ckptcfg='', ckpt_ts=True)),
        ('ckpt_ts_false', dict(ckptcfg='use_timestamp=false', ckpt_ts=False)),
        ('ckpt_ts_true', dict(ckptcfg='use_timestamp=true', ckpt_ts=True)),
    ]

    conncfg = [
        ('nolog', dict(conn_config='create', using_log=False)),
        ('V1', dict(conn_config='create,log=(archive=false,enabled),compatibility=(release="2.9")', using_log=True)),
        ('V2', dict(conn_config='create,log=(archive=false,enabled)', using_log=True)),
    ]
    session_config = 'isolation=snapshot'

    scenarios = make_scenarios(conncfg, types, ckpt)

    # Check that a cursor (optionally started in a new transaction), sees the
    # expected values.
    def check(self, session, txn_config, tablename, expected):
        if txn_config:
            session.begin_transaction(txn_config)

        cur = session.open_cursor(tablename, None)
        actual = dict((k, v) for k, v in cur if v != 0)
        if actual != expected:
            print("missing: ", sorted(set(expected.items()) - set(actual.items())))
            print("extras: ", sorted(set(actual.items()) - set(expected.items())))
        self.assertTrue(actual == expected)
        # Search for the expected items as well as iterating
        for k, v in expected.items():
            self.assertEqual(cur[k], v, "for key " + str(k))
        cur.close()
        if txn_config:
            session.commit_transaction()

    # Take a backup of the database and verify that the value we want to
    # check exists in the tables the expected number of times.
    def backup_check(self, check_value, expected_ts_log, expected_ts_nolog):
        newdir = "BACKUP"
        copy_wiredtiger_home(self, '.', newdir, True)

        conn = self.setUpConnectionOpen(newdir)
        session = self.setUpSessionOpen(conn)
        cur_ts_log      = session.open_cursor(self.table_ts_log)
        cur_ts_nolog    = session.open_cursor(self.table_ts_nolog)
        # Count how many times the given value is present in the
        # logged timestamp table.
        actual_ts_log = 0
        for k, v in cur_ts_log:
            if check_value == v:
                actual_ts_log += 1
        cur_ts_log.close()
        # Count how many times the given value is present in the
        # not logged timestamp table
        actual_ts_nolog = 0
        for k, v in cur_ts_nolog:
            if check_value == v:
                actual_ts_nolog += 1
        cur_ts_nolog.close()
        conn.close()
        self.assertEqual(actual_ts_log, expected_ts_log)
        self.assertEqual(actual_ts_nolog, expected_ts_nolog)

    # Check that a cursor sees the expected values after a checkpoint.
    def ckpt_backup(self, check_value, valcnt_ts_log, valcnt_ts_nolog):
        # Take a checkpoint.  Make a copy of the database.  Open the
        # copy and verify whether or not the expected data is in there.
        self.pr("CKPT: " + self.ckptcfg)
        ckptcfg = self.ckptcfg

        self.session.checkpoint(ckptcfg)
        self.backup_check(check_value, valcnt_ts_log, valcnt_ts_nolog)

    def test_timestamp06(self):
        # Open two timestamp tables:
        # 1. Table is logged and uses timestamps.
        # 2. Table is not logged and uses timestamps.
        self.session.create(self.table_ts_log, 'key_format=i,value_format=i' + self.extra_config)
        cur_ts_log = self.session.open_cursor(self.table_ts_log)
        self.session.create(self.table_ts_nolog, 'key_format=i,value_format=i,log=(enabled=false)' + self.extra_config)
        cur_ts_nolog = self.session.open_cursor(self.table_ts_nolog)

        # Insert keys 1..100
        nkeys = 100
        orig_keys = list(range(1, nkeys+1))
        keys = orig_keys[:]
        random.shuffle(keys)

        self.session.begin_transaction()
        # Make three updates with different timestamps.
        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(1))
        for k in keys:
            cur_ts_log[k] = 1
            cur_ts_nolog[k] = 1

        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(101))
        for k in keys:
            cur_ts_log[k] = 2
            cur_ts_nolog[k] = 2

        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(201))
        for k in keys:
            cur_ts_log[k] = 3
            cur_ts_nolog[k] = 3

        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(301))
        cur_ts_log.close()
        cur_ts_nolog.close()

        # Scenario: 1
        # Check that we see all the latest values (i.e. 3) as per transaction
        # visibility when reading without the read timestamp.
        # All tables should see all the values.
        self.check(self.session, "", self.table_ts_log,
            dict((k, 3) for k in orig_keys))
        self.check(self.session, "", self.table_ts_nolog,
            dict((k, 3) for k in orig_keys))

        # Scenario: 2
        # Check that we see the values till correctly from checkpointed data
        # files in case of multistep transactions.
        # Set oldest and stable timestamps
        old_ts = self.timestamp_str(100)
        # Set the stable timestamp such that last update is beyond it.
        stable_ts = self.timestamp_str(200)
        self.conn.set_timestamp('oldest_timestamp=' + old_ts)
        self.conn.set_timestamp('stable_timestamp=' + stable_ts)

        # Check that we see the values correctly till stable timestamp.
        self.check(self.session, 'read_timestamp=' + stable_ts,
            self.table_ts_log, dict((k, 2) for k in orig_keys))
        self.check(self.session, 'read_timestamp=' + stable_ts,
            self.table_ts_nolog, dict((k, 2) for k in orig_keys))

        # For logged table we should see latest values (i.e. 3) when logging
        # is enabled.
        if self.using_log == True:
            valcnt_ts_log = nkeys
        else:
            # When logging is disabled, we should not see the values beyond the
            # stable timestamp with timestamped checkpoints.
            if self.ckpt_ts == True:
                valcnt_ts_log = 0
            else:
                valcnt_ts_log = nkeys

        # For non-logged table we should not see the values beyond the
        # stable timestamp with timestamped checkpoints.
        if self.ckpt_ts == True:
            valcnt_ts_nolog = 0
        else:
            valcnt_ts_nolog = nkeys

        # Check to see the count of latest values as expected from checkpoint.
        self.ckpt_backup(3, valcnt_ts_log, valcnt_ts_nolog)
        self.ckpt_backup(2, (nkeys - valcnt_ts_log), (nkeys - valcnt_ts_nolog))

        # Scenario: 3
        # Check we see all the data values correctly after rollback. Skip the case where the most
        # recent checkpoint wasn't based on the last stable timestamp, those can't be rolled back.
        if self.ckpt_ts == False:
                return
        self.conn.rollback_to_stable()

        # All tables should see the values correctly when read with
        # read timestamp as stable timestamp.
        self.check(self.session, 'read_timestamp=' + stable_ts,
            self.table_ts_nolog, dict((k, 2) for k in orig_keys))
        self.check(self.session, 'read_timestamp=' + stable_ts,
            self.table_ts_log, dict((k, 2) for k in orig_keys))

        # Scenario: 4
        # Check that we see the values correctly when read without any
        # timestamp.
        if self.using_log == True:
            # For logged table we should see latest values (i.e. 3) when logging
            # is enabled.
            self.check(self.session, "",
                self.table_ts_log, dict((k, 3) for k in orig_keys))
        else:
            # When logging is disabled, we should not see the values beyond the
            # stable timestamp with timestamped checkpoints.
            self.check(self.session, "",
                self.table_ts_log, dict((k, 2) for k in orig_keys))

        # For non-logged table we should not see the values beyond the
        # stable timestamp with timestamped checkpoints.
        self.check(self.session, "",
            self.table_ts_nolog, dict((k, 2) for k in orig_keys))

if __name__ == '__main__':
    wttest.run()
