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
# test_timestamp03.py
#   Timestamps: checkpoints
#

from helper import copy_wiredtiger_home
import random
from suite_subprocess import suite_subprocess
import wiredtiger, wttest
from wtscenario import make_scenarios

class test_timestamp03(wttest.WiredTigerTestCase, suite_subprocess):
    table_ts_log     = 'ts03_ts_logged'
    table_ts_nolog   = 'ts03_ts_nologged'
    table_nots_log   = 'ts03_nots_logged'
    table_nots_nolog = 'ts03_nots_nologged'

    # XXX neither the use_cg nor the use_index values are actually used.
    # I've commented out the entries that are therefore duplicates; if/when that's fixed, put
    # them back I guess.
    types = [
        ('file-row', dict(uri='file:', key_format='i', value_format='S',
                use_cg=False, use_index=False)),
        ('file-col', dict(uri='file:', key_format='r', value_format='S',
                use_cg=False, use_index=False)),
        ('file-col-fix', dict(uri='file:', key_format='r', value_format='8t',
                use_cg=False, use_index=False)),
        ('lsm', dict(uri='lsm:', key_format='i', value_format='S',
                use_cg=False, use_index=False)),
        ('table-row', dict(uri='table:', key_format='i', value_format='S',
                use_cg=False, use_index=False)),
        #('table-row-index', dict(uri='table:', key_format='i', value_format='S',
        #       use_cg=False, use_index=True)),
        ('table-col', dict(uri='table:', key_format='r', value_format='S',
                use_cg=False, use_index=False)),
        ('table-col-fix', dict(uri='table:', key_format='r', value_format='8t',
                use_cg=False, use_index=False)),
        #('table-col-cg', dict(uri='table:', key_format='r', value_format='S',
        #       use_cg=True, use_index=False)),
    ]

    ckpt = [
        ('use_ts_def', dict(ckptcfg='', ckpt_ts=True)),
        ('use_ts_false', dict(ckptcfg='use_timestamp=false', ckpt_ts=False)),
        ('use_ts_true', dict(ckptcfg='use_timestamp=true', ckpt_ts=True)),
    ]

    conncfg = [
        ('nolog', dict(conn_config='create', using_log=False)),
        ('V1', dict(conn_config='create,log=(archive=false,enabled),compatibility=(release="2.9")', using_log=True)),
        ('V2', dict(conn_config='create,log=(archive=false,enabled)', using_log=True)),
    ]

    scenarios = make_scenarios(types, ckpt, conncfg)

    def moresetup(self):
        # Binary values.
        if self.value_format == '8t':
            self.value = 4
            self.value2 = 5
            self.value3 = 6
        else:
            self.value  = u'\u0001\u0002abcd\u0003\u0004'
            self.value2 = u'\u0001\u0002dcba\u0003\u0004'
            self.value3 = u'\u0001\u0002cdef\u0003\u0004'

    # Check that a cursor (optionally started in a new transaction), sees the
    # expected values.
    def check(self, session, txn_config, tablename, expected):

        if txn_config:
            session.begin_transaction(txn_config)

        cur = session.open_cursor(self.uri + tablename, None)
        actual = dict((k, v) for k, v in cur if v != 0)
        self.assertTrue(actual == expected)
        # Search for the expected items as well as iterating
        for k, v in expected.items():
            self.assertEqual(cur[k], v, "for key " + str(k))
        cur.close()
        if txn_config:
            session.commit_transaction()
    #
    # Take a backup of the database and verify that the value we want to
    # check exists in the tables the expected number of times.
    #
    def backup_check(
        self, check_value, expected_ts_log, expected_ts_nolog, expected_nots_log,
        expected_nots_nolog):

        newdir = "BACKUP"
        copy_wiredtiger_home(self, '.', newdir, True)

        conn = self.setUpConnectionOpen(newdir)
        session = self.setUpSessionOpen(conn)
        cur_ts_log      = session.open_cursor(self.uri + self.table_ts_log, None)
        cur_ts_nolog    = session.open_cursor(self.uri + self.table_ts_nolog, None)
        cur_nots_log    = session.open_cursor(self.uri + self.table_nots_log, None)
        cur_nots_nolog  = session.open_cursor(self.uri + self.table_nots_nolog, None)

        # In FLCS the values are bytes, which are numbers, but the tests below are via
        # string inclusion rather than just equality of values. Not sure why that is, but
        # I'm going to assume there's a reason for it and not change things. Compensate.
        if self.value_format == '8t':
            check_value = str(check_value)

        # Count how many times the check_value is present in the
        # logged timestamp table.
        actual_ts_log = 0
        for k, v in cur_ts_log:
            if check_value in str(v):
                actual_ts_log += 1
        cur_ts_log.close()
        # Count how many times the check_value is present in the
        # not logged timestamp table
        actual_ts_nolog = 0
        for k, v in cur_ts_nolog:
            if check_value in str(v):
                actual_ts_nolog += 1
        cur_ts_nolog.close()
        # Count how many times the check_value is present in the
        # logged non-timestamp table.
        actual_nots_log = 0
        for k, v in cur_nots_log:
            if check_value in str(v):
                actual_nots_log += 1
        cur_nots_log.close()
        # Count how many times the check_value is present in the
        # not logged non-timestamp table.
        actual_nots_nolog = 0
        for k, v in cur_nots_nolog:
            if check_value in str(v):
                actual_nots_nolog += 1
        cur_nots_nolog.close()
        conn.close()
        self.assertEqual(actual_ts_log, expected_ts_log)
        self.assertEqual(actual_ts_nolog, expected_ts_nolog)
        self.assertEqual(actual_nots_log, expected_nots_log)
        self.assertEqual(actual_nots_nolog, expected_nots_nolog)

    # Check that a cursor sees the expected values after a checkpoint.
    def ckpt_backup(
        self, check_value, valcnt_ts_log, valcnt_ts_nolog, valcnt_nots_log,
        valcnt_nots_nolog):

        # Take a checkpoint.  Make a copy of the database.  Open the
        # copy and verify whether or not the expected data is in there.
        self.pr("CKPT: " + self.ckptcfg)
        ckptcfg = self.ckptcfg

        self.session.checkpoint(ckptcfg)
        self.backup_check(check_value, valcnt_ts_log, valcnt_ts_nolog,
            valcnt_nots_log, valcnt_nots_nolog)

    def test_timestamp03(self):
        self.moresetup()
        uri_ts_log      = self.uri + self.table_ts_log
        uri_ts_nolog    = self.uri + self.table_ts_nolog
        uri_nots_log    = self.uri + self.table_nots_log
        uri_nots_nolog  = self.uri + self.table_nots_nolog
        #
        # Open four tables:
        # 1. Table is logged and uses timestamps.
        # 2. Table is not logged and uses timestamps.
        # 3. Table is logged and does not use timestamps.
        # 4. Table is not logged and does not use timestamps.
        #
        format = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(uri_ts_log, format)
        cur_ts_log = self.session.open_cursor(uri_ts_log)
        self.session.create(uri_ts_nolog, format + ',log=(enabled=false)')
        cur_ts_nolog = self.session.open_cursor(uri_ts_nolog)
        self.session.create(uri_nots_log, format)
        cur_nots_log = self.session.open_cursor(uri_nots_log)
        self.session.create(uri_nots_nolog, format + ',log=(enabled=false)')
        cur_nots_nolog = self.session.open_cursor(uri_nots_nolog)

        # Insert keys 1..100 each with timestamp=key, in some order
        nkeys = 100
        orig_keys = list(range(1, nkeys+1))
        keys = orig_keys[:]
        random.shuffle(keys)

        for k in keys:
            cur_nots_log[k] = self.value
            cur_nots_nolog[k] = self.value
            self.session.begin_transaction()
            cur_ts_log[k] = self.value
            cur_ts_nolog[k] = self.value
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(k))

        # Scenario: 1
        # Check that we see all the inserted values as per transaction
        # visibility when reading with out the read_timestamp.
        # All tables should see all the values.
        self.check(self.session, "", self.table_ts_log,
            dict((k, self.value) for k in orig_keys))
        self.check(self.session, "", self.table_ts_nolog,
            dict((k, self.value) for k in orig_keys))
        self.check(self.session, "", self.table_nots_log,
            dict((k, self.value) for k in orig_keys))
        self.check(self.session, "", self.table_nots_nolog,
            dict((k, self.value) for k in orig_keys))

        # Scenario: 2
        # Check that we see the inserted values as per the timestamp.
        for i, t in enumerate(orig_keys):
            # Tables using the timestamps should see the values as per the
            # given read_timestamp
            self.check(self.session, 'read_timestamp=' + self.timestamp_str(t),
                self.table_ts_log, dict((k, self.value) for k in orig_keys[:i+1]))
            self.check(self.session, 'read_timestamp=' + self.timestamp_str(t),
                self.table_ts_nolog, dict((k, self.value) for k in orig_keys[:i+1]))
            # Tables not using the timestamps should see all the values.
            self.check(self.session, 'read_timestamp=' + self.timestamp_str(t),
                self.table_nots_log, dict((k, self.value) for k in orig_keys))
            self.check(self.session, 'read_timestamp=' + self.timestamp_str(t),
                self.table_nots_nolog, dict((k, self.value) for k in orig_keys))

        # Bump the oldest_timestamp, we're not going back...
        self.assertTimestampsEqual(self.conn.query_timestamp(), self.timestamp_str(100))
        old_ts = self.timestamp_str(100)
        self.conn.set_timestamp('oldest_timestamp=' + old_ts)
        self.conn.set_timestamp('stable_timestamp=' + old_ts)

        # Scenario: 3
        # Check that we see all the data values after moving the oldest_timestamp
        # to the current timestamp
        # All tables should see all the values.
        self.check(self.session, 'read_timestamp=' + old_ts,
            self.table_ts_log, dict((k, self.value) for k in orig_keys))
        self.check(self.session, 'read_timestamp=' + old_ts,
            self.table_ts_nolog, dict((k, self.value) for k in orig_keys))
        self.check(self.session, 'read_timestamp=' + old_ts,
            self.table_nots_log, dict((k, self.value) for k in orig_keys))
        self.check(self.session, 'read_timestamp=' + old_ts,
            self.table_nots_nolog, dict((k, self.value) for k in orig_keys))

        # Update the keys and checkpoint using the stable_timestamp.
        random.shuffle(keys)
        count = 0
        for k in keys:
            # Make sure a timestamp cursor is the last one to update.  This
            # tests the scenario for a bug we found where recovery replayed
            # the last record written into the log.
            cur_nots_log[k] = self.value2
            cur_nots_nolog[k] = self.value2
            self.session.begin_transaction()
            cur_ts_log[k] = self.value2
            cur_ts_nolog[k] = self.value2
            ts = self.timestamp_str(k + 100)
            self.session.commit_transaction('commit_timestamp=' + ts)
            count += 1

        # Scenario: 4
        # Check that we don't see the updated data of timestamp tables
        # with the read_timestamp as oldest_timestamp
        # Tables using the timestamps should see old values (i.e. value) only
        self.check(self.session, 'read_timestamp=' + old_ts,
            self.table_ts_log, dict((k, self.value) for k in orig_keys))
        self.check(self.session, 'read_timestamp=' + old_ts,
            self.table_ts_nolog, dict((k, self.value) for k in orig_keys))
        # Tables not using the timestamps should see updated values (i.e. value2).
        self.check(self.session, 'read_timestamp=' + old_ts,
            self.table_nots_log, dict((k, self.value2) for k in orig_keys))
        self.check(self.session, 'read_timestamp=' + old_ts,
            self.table_nots_nolog, dict((k, self.value2) for k in orig_keys))

        # Scenario: 4a
        # This scenario is same as earlier one with read_timestamp earlier than
        # oldest_timestamp and using the option of rounding read_timestamp to
        # the oldest_timestamp
        earlier_ts = self.timestamp_str(90)
        self.check(self.session,
            'read_timestamp=' + earlier_ts +',roundup_timestamps=(read=true)',
            self.table_ts_log, dict((k, self.value) for k in orig_keys))
        self.check(self.session,
            'read_timestamp=' + earlier_ts +',roundup_timestamps=(read=true)',
            self.table_ts_nolog, dict((k, self.value) for k in orig_keys))
        # Tables not using the timestamps should see updated values (i.e. value2).
        self.check(self.session,
            'read_timestamp=' + earlier_ts +',roundup_timestamps=(read=true)',
            self.table_nots_log, dict((k, self.value2) for k in orig_keys))
        self.check(self.session,
            'read_timestamp=' + earlier_ts +',roundup_timestamps=(read=true)',
            self.table_nots_nolog, dict((k, self.value2) for k in orig_keys))

        # Scenario: 5
        # Check that we see the updated values as per the timestamp.
        # Construct expected values.
        expected_dict = dict((k, self.value) for k in orig_keys)
        for i, t in enumerate(orig_keys):
            # update expected value
            expected_dict[i+1] = self.value2
            # Tables using the timestamps should see the updated values as per
            # the given read_timestamp
            self.check(self.session, 'read_timestamp=' + self.timestamp_str(t + 100),
                self.table_ts_log, expected_dict)
            self.check(self.session, 'read_timestamp=' + self.timestamp_str(t + 100),
                self.table_ts_nolog, expected_dict)
            # Tables not using the timestamps should see all the data values as
            # updated values (i.e. value2).
            self.check(self.session, 'read_timestamp=' + self.timestamp_str(t + 100),
                self.table_nots_log, dict((k, self.value2) for k in orig_keys))
            self.check(self.session, 'read_timestamp=' + self.timestamp_str(t + 100),
                self.table_nots_nolog, dict((k, self.value2) for k in orig_keys))

        # Take a checkpoint using the given configuration.  Then verify
        # whether value2 appears in a copy of that data or not.
        valcnt_ts_log = valcnt_nots_log = valcnt_nots_nolog = nkeys
        if self.ckpt_ts == False:
            # if use_timestamp is false, then all updates will be checkpointed.
            valcnt_ts_nolog = nkeys
        else:
            # Checkpoint will happen with stable_timestamp=100.
            if self.using_log == True:
                # only table_ts_nolog will have old values when logging is enabled
                self.ckpt_backup(self.value, 0, nkeys, 0, 0)
            else:
                # Both table_ts_nolog and table_ts_log will have old values when
                # logging is disabled.
                self.ckpt_backup(self.value, nkeys, nkeys, 0, 0)
            # table_ts_nolog will not have any new values (i.e. value2)
            valcnt_ts_nolog = 0

        if self.ckpt_ts == False:
            valcnt_ts_log = nkeys
        # Take a checkpoint using the given configuration.  Then verify
        # whether value2 appears in a copy of that data or not.
        valcnt_ts_log = valcnt_nots_log = valcnt_nots_nolog = nkeys
        if self.ckpt_ts == False:
            # if use_timestamp is false, then all updates will be checkpointed.
            valcnt_ts_nolog = nkeys
        else:
            # Checkpoint will happen with stable_timestamp=100.
            if self.using_log == True:
                # only table_ts_nolog will have old values when logging is enabled
                self.ckpt_backup(self.value, 0, nkeys, 0, 0)
            else:
                # Both table_ts_nolog and table_ts_log will have old values when
                # logging is disabled.
                self.ckpt_backup(self.value, nkeys, nkeys, 0, 0)
            # table_ts_nolog will not have any new values (i.e. value2)
            valcnt_ts_nolog = 0

        if self.ckpt_ts == False:
            valcnt_ts_log = nkeys
        else:
            # When log is enabled, table_ts_log will have all new values, else
            # none.
            if self.using_log == True:
                valcnt_ts_log = nkeys
            else:
                valcnt_ts_log = 0

        self.ckpt_backup(self.value2, valcnt_ts_log, valcnt_ts_nolog,
            valcnt_nots_log, valcnt_nots_nolog)

        # Update the stable_timestamp to the latest, but not the
        # oldest_timestamp and make sure we can see the data.  Once the
        # stable_timestamp is moved we should see all keys with value2.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(100+nkeys))
        self.ckpt_backup(self.value2, nkeys, nkeys, nkeys, nkeys)

        # If we're not using the log we're done.
        if not self.using_log:
            return

        # Scenario: 7
        # Update the keys and log_flush with out checkpoint.
        random.shuffle(keys)
        count = 0
        for k in keys:
            # Make sure a timestamp cursor is the last one to update.
            #
            cur_nots_log[k] = self.value3
            cur_nots_nolog[k] = self.value3
            self.session.begin_transaction()
            cur_ts_log[k] = self.value3
            cur_ts_nolog[k] = self.value3
            ts = self.timestamp_str(k + 200)
            self.session.commit_transaction('commit_timestamp=' + ts)
            count += 1

        # Flush the log but don't checkpoint
        self.session.log_flush('sync=on')

        # Take a backup and then verify whether value3 appears in a copy
        # of that data or not.  Both tables that are logged should see
        # all the data regardless of timestamps.  Both tables that are not
        # logged should not see any of it.
        valcnt_ts_nolog = valcnt_nots_nolog = 0
        valcnt_ts_log = valcnt_nots_log = nkeys
        self.backup_check(self.value3, valcnt_ts_log, valcnt_ts_nolog,
            valcnt_nots_log, valcnt_nots_nolog)

if __name__ == '__main__':
    wttest.run()
