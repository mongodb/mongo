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
# test_timestamp07.py
#   Timestamps: checkpoints.
#

from helper import copy_wiredtiger_home
import random
from suite_subprocess import suite_subprocess
import wiredtiger, wttest
from wtscenario import make_scenarios

class test_timestamp07(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'ts07_ts_nologged'
    tablename2 = 'ts07_nots_logged'

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column-fix', dict(key_format='r', value_format='8t')),
        ('row', dict(key_format='i', value_format='S')),
    ]

    types = [
        ('file', dict(uri='file:', use_cg=False, use_index=False)),
        ('table-cg', dict(uri='table:', use_cg=True, use_index=False)),
    ]

    conncfg = [
        ('log', dict(conn_config='create,log=(enabled,file_max=1M,remove=false),cache_size=2M')),
    ]

    nkeys = [
        ('100keys', dict(nkeys=100)),
        ('500keys', dict(nkeys=500)),
        ('1000keys', dict(nkeys=1000)),
    ]

    scenarios = make_scenarios(format_values, types, conncfg, nkeys)

    # Binary values.
    def moreinit(self):
        if self.value_format == '8t':
            self.value = 2
            self.value2 = 4
            self.value3 = 6
        else:
            self.value = u'\u0001\u0002abcd\u0007\u0004'
            self.value2 = u'\u0001\u0002dcba\u0007\u0004'
            self.value3 = u'\u0001\u0002cdef\u0007\u0004'

    # Check that a cursor (optionally started in a new transaction), sees the expected value for a
    # key.
    def check(self, session, txn_config, k, expected, flcs_expected):
        # In FLCS the table extends under uncommitted writes and we expect to
        # see zero rather than NOTFOUND.
        if self.value_format == '8t' and flcs_expected is not None:
            expected = flcs_expected
        if txn_config:
            session.begin_transaction(txn_config)
        c = session.open_cursor(self.uri + self.tablename, None)
        if expected is None:
            c.set_key(k)
            self.assertEqual(c.search(), wiredtiger.WT_NOTFOUND)
        else:
            self.assertEqual(c[k], expected)
        c.close()
        if txn_config:
            session.commit_transaction()

    # Check reads of all tables at a timestamp
    def check_reads(self, session, txn_config, check_value, valcnt, valcnt2, valcnt3):
        if txn_config:
            session.begin_transaction(txn_config)
        c = session.open_cursor(self.uri + self.tablename, None)
        c2 = session.open_cursor(self.uri + self.tablename2, None)

        # In FLCS the values are bytes, which are numbers, but the tests below are via string
        # inclusion rather than just equality of values.
        if self.value_format == '8t':
            check_value = str(check_value)

        count = 0
        for k, v in c:
            if check_value in str(v):
                count += 1
        c.close()
        count2 = 0
        for k, v in c2:
            if check_value in str(v):
                count2 += 1
        c2.close()
        if txn_config:
            session.commit_transaction()
        self.assertEqual(count, valcnt)
        self.assertEqual(count2, valcnt2)

    # Take a backup of the database and verify that the value we want to check exists in the tables
    # the expected number of times.
    def backup_check(self, check_value, valcnt, valcnt2, valcnt3):
        newdir = "BACKUP"
        copy_wiredtiger_home(self, '.', newdir, True)

        conn = self.setUpConnectionOpen(newdir)
        session = self.setUpSessionOpen(conn)
        c = session.open_cursor(self.uri + self.tablename, None)
        c2 = session.open_cursor(self.uri + self.tablename2, None)

        # In FLCS the values are bytes, which are numbers, but the tests below are via string
        # inclusion rather than just equality of values.
        if self.value_format == '8t':
            check_value = str(check_value)

        # Count how many times the second value is present
        count = 0
        for k, v in c:
            if check_value in str(v):
                # print "check_value found in key " + str(k)
                count += 1
        c.close()

        # Count how many times the second value is present in the non-timestamp table.
        count2 = 0
        for k, v in c2:
            if check_value in str(v):
                # print "check_value found in key " + str(k)
                count2 += 1
        c2.close()
        conn.close()
        # print "CHECK BACKUP: Count " +\
        #     str(count) + " Count2 " + str(count2) + " Count3 " + str(count3)
        # print "CHECK BACKUP: Expect value2 count " + str(valcnt)
        # print "CHECK BACKUP: 2nd table Expect value2 count " + str(valcnt2)
        # print "CHECK BACKUP: 3rd table Expect value2 count " + str(valcnt3)
        self.assertEqual(count, valcnt)
        self.assertEqual(count2, valcnt2)

    # Check that a cursor sees the expected values after a checkpoint.
    def ckpt_backup(self, check_value, valcnt, valcnt2, valcnt3):

        # Take a checkpoint.  Make a copy of the database.  Open the copy and verify whether or not
        # the expected data is in there.
        ckptcfg = 'use_timestamp=true'
        self.session.checkpoint(ckptcfg)
        self.backup_check(check_value, valcnt, valcnt2, valcnt3)

    def check_stable(self, check_value, valcnt, valcnt2, valcnt3):
        self.ckpt_backup(check_value, valcnt, valcnt2, valcnt3)
        self.check_reads(self.session, 'read_timestamp=' + self.stablets,
            check_value, valcnt, valcnt2, valcnt)

    def test_timestamp07(self):
        self.moreinit()

        # Open two tables:
        # 1. Table is not logged and uses timestamps.
        # 2. Table is logged and so timestamps are ignored.
        #
        format = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        uri = self.uri + self.tablename
        self.session.create(uri, format + ',log=(enabled=false)')
        c = self.session.open_cursor(uri)
        uri2 = self.uri + self.tablename2
        self.session.create(uri2, format)
        c2 = self.session.open_cursor(uri2)
        # print "tables created"

        # Insert keys 1..nkeys each with timestamp=key, in some order.
        orig_keys = list(range(1, self.nkeys+1))
        keys = orig_keys[:]
        random.shuffle(keys)

        for k in keys:
            c2[k] = self.value
            self.session.begin_transaction()
            c[k] = self.value
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(k))

        # print "value inserted in all tables, reading..."

        # Now check that we see the expected state when reading at each timestamp.
        for k in orig_keys:
            self.check(self.session, 'read_timestamp=' + self.timestamp_str(k),
                k, self.value, None)
            self.check(self.session, 'read_timestamp=' + self.timestamp_str(k),
                k + 1, None, None if k == self.nkeys else 0)

        # print "all values read, updating timestamps"

        # Bump the oldest timestamp, we're not going back...
        self.assertTimestampsEqual(self.conn.query_timestamp(), self.timestamp_str(self.nkeys))
        self.oldts = self.stablets = self.timestamp_str(self.nkeys)
        self.conn.set_timestamp('oldest_timestamp=' + self.oldts)
        self.conn.set_timestamp('stable_timestamp=' + self.stablets)
        # print "Oldest " + self.oldts

        # print "inserting value2 in all tables"

        # Update them and retry.
        random.shuffle(keys)
        count = 0
        for k in keys:
            # Make sure a timestamp cursor is the last one to update.  This
            # tests the scenario for a bug we found where recovery replayed
            # the last record written into the log.
            #
            # print "Key " + str(k) + " to value2"
            self.session.begin_transaction()
            c[k] = self.value2
            c2[k] = self.value2
            ts = self.timestamp_str(k + self.nkeys)
            self.session.commit_transaction('commit_timestamp=' + ts)
            # print "Commit key " + str(k) + " ts " + ts
            count += 1

        # print "Updated " + str(count) + " keys to value2"

        # Take a checkpoint using the given configuration.  Then verify the value doesn't appear
        # in that copy of that data.
        # print "check_stable 1"
        self.check_stable(self.value2, 0, self.nkeys, 0)

        # Update the stable timestamp to the latest, but not the oldest
        # timestamp and make sure we can see the data.  Once the stable
        # timestamp is moved we should see all keys with value2.
        self.stablets = self.timestamp_str(self.nkeys*2)
        self.conn.set_timestamp('stable_timestamp=' + self.stablets)
        # print "check_stable 2"
        self.check_stable(self.value2, self.nkeys, self.nkeys, self.nkeys)

        # Update the key and retry.  This time take a backup and recover.
        random.shuffle(keys)
        count = 0
        for k in keys:
            # Make sure a timestamp cursor is the last one to update.  This
            # tests the scenario for a bug we found where recovery replayed
            # the last record written into the log.
            #
            # print "Key " + str(k) + " to value3"
            self.session.begin_transaction()
            c[k] = self.value3
            c2[k] = self.value3
            ts = self.timestamp_str(k + self.nkeys*2)
            self.session.commit_transaction('commit_timestamp=' + ts)
            # print "Commit key " + str(k) + " ts " + ts
            count += 1

        # print "Updated " + str(count) + " keys to value3"

        # Flush the log but don't checkpoint
        self.session.log_flush('sync=on')

        # Take a backup and then verify whether value3 appears in a copy
        # of that data or not.  Both tables that are logged should see
        # all the data regardless of timestamps.  The table that is not
        # logged should not see any of it.
        # print "check_stable 3"
        self.check_stable(self.value3, 0, self.nkeys, self.nkeys)

if __name__ == '__main__':
    wttest.run()
