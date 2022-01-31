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
# test_assert06.py
#   Timestamps: verify ordered setting for durable timestamps
#

from suite_subprocess import suite_subprocess
import wiredtiger, wttest
from wtscenario import make_scenarios

class test_assert06(wttest.WiredTigerTestCase, suite_subprocess):

    key_format_values = [
        ('column', dict(key_format='r', usestrings=False)),
        ('string-row', dict(key_format='S', usestrings=True))
    ]
    scenarios = make_scenarios(key_format_values)

    def apply_timestamps(self, timestamp):
        self.session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(timestamp))
        self.session.timestamp_transaction(
            'commit_timestamp=' + self.timestamp_str(timestamp))
        self.session.timestamp_transaction(
            'durable_timestamp=' + self.timestamp_str(timestamp))

    def test_timestamp_alter(self):
        base = 'assert06'
        uri = 'file:' + base
        cfg_on = 'write_timestamp_usage=ordered,assert=(write_timestamp=on)'
        cfg_off = 'write_timestamp_usage=never,assert=(write_timestamp=off)'
        msg_ooo='/out of order/'
        msg_usage='/used inconsistently/'

        key_nots = 'key_nots' if self.usestrings else 5
        key_ts1 = 'key_ts1' if self.usestrings else 16

        # Create the table without the key consistency checking turned on.
        # Create a few items breaking the rules. Then alter the setting and
        # verify the inconsistent usage is detected.
        self.session.create(uri, 'key_format={},value_format=S'.format(self.key_format))
        # Insert a data item at timestamp 2.
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_ts1] = 'value2'
        self.apply_timestamps(2)
        self.session.commit_transaction()
        c.close()

        # Modify the data item at timestamp 1.
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_ts1] = 'value1'
        self.apply_timestamps(1)
        self.session.commit_transaction()
        c.close()

        # Insert a non-timestamped item. Then modify with a timestamp. And
        # again modify without a timestamp.
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_nots] = 'value_nots'
        self.session.commit_transaction()
        c.close()

        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_nots] = 'value2'
        self.apply_timestamps(2)
        self.session.commit_transaction()
        c.close()

        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_nots] = 'value_nots2'
        self.session.commit_transaction()
        c.close()

        # We must move the oldest timestamp forward in order to alter.
        # Otherwise alter closing the file will fail with EBUSY.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(2))

        # Now alter the setting and make sure we detect incorrect usage.
        self.session.alter(uri, cfg_on)

        # Detect decreasing timestamp.
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_ts1] = 'value5'
        self.apply_timestamps(5)
        self.session.commit_transaction()
        c.close()

        '''
        Commented out for now: the system panics if we fail after preparing a transaction.

        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_ts1] = 'value4'
        self.apply_timestamps(4)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.commit_transaction(), msg_ooo)
        c.close()
        '''

        # Detect not using a timestamp.
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_ts1] = 'value_nots3'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.commit_transaction(), msg_usage)
        c.close()

        # Detect using a timestamp on the non-timestamp key.
        # We must first use a non timestamped operation on the key
        # in order to violate the key consistency condition in the
        # following transaction.
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_nots] = 'value_nots3'
        self.session.commit_transaction()
        c.close()

        '''
        Commented out for now: the system panics if we fail after preparing a transaction.

        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_nots] = 'value3'
        self.apply_timestamps(3)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.commit_transaction(), msg_usage)
        c.close()
        self.session.checkpoint()
        '''

        c = self.session.open_cursor(uri)
        self.assertEquals(c[key_ts1], 'value5')
        self.assertEquals(c[key_nots], 'value_nots3')
        c.close()

        # Test to make sure that key consistency can be turned off
        # after turning it on.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(5))
        self.session.alter(uri, cfg_off)

        # Detection is off we can successfully change the same key with and
        # without a timestamp.
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_nots] = 'value_nots4'
        self.session.commit_transaction()
        c.close()

        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_nots] = 'value6'
        self.apply_timestamps(6)
        self.session.commit_transaction()
        c.close()

    def test_timestamp_usage(self):
        base = 'assert06'
        uri = 'file:' + base
        msg_ooo='/out of order/'
        msg_usage='/used inconsistently/'

        key_nots = 'key_nots' if self.usestrings else 5
        key_ts1 = 'key_ts1' if self.usestrings else 16
        key_ts2 = 'key_ts2' if self.usestrings else 17
        key_ts3 = 'key_ts3' if self.usestrings else 18
        key_ts4 = 'key_ts4' if self.usestrings else 19
        key_ts5 = 'key_ts5' if self.usestrings else 20
        key_ts6 = 'key_ts6' if self.usestrings else 21

        # Create the table with the key consistency checking turned on.
        # That checking will verify any individual key is always or never
        # used with a timestamp. And if it is used with a timestamp that
        # the timestamps are in increasing order for that key.
        self.session.create(uri, 'key_format={},value_format=S,verbose=(write_timestamp),write_timestamp_usage=ordered,assert=(write_timestamp=on)'.format(self.key_format))

        # Insert a data item at timestamp 2.
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_ts1] = 'value2'
        self.apply_timestamps(2)
        self.session.commit_transaction()
        c.close()

        '''
        Commented out for now: the system panics if we fail after preparing a transaction.

        # Modify the data item at timestamp 1. We should detect it is wrong.
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_ts1] = 'value1'
        self.apply_timestamps(1)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.commit_transaction(), msg_ooo)
        c.close()
        '''

        # Make sure we can successfully add a different key at timestamp 1.
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_ts2] = 'value1'
        self.apply_timestamps(1)
        self.session.commit_transaction()
        c.close()

        #
        # Insert key_ts3 at timestamp 10 and key_ts4 at 15.
        # Then modify both keys in one transaction at timestamp 13.
        # We should not be allowed to modify the one from 15.
        # So the whole transaction should fail.
        #
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_ts3] = 'value10'
        self.apply_timestamps(10)
        self.session.commit_transaction()
        self.session.begin_transaction()
        c[key_ts4] = 'value15'
        self.apply_timestamps(15)
        self.session.commit_transaction()

        '''
        Commented out for now: the system panics if we fail after preparing a transaction.

        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_ts3] = 'value13'
        c[key_ts4] = 'value13'
        self.apply_timestamps(13)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.commit_transaction(), msg_ooo)
        c.close()
        '''

        c = self.session.open_cursor(uri)
        self.assertEquals(c[key_ts3], 'value10')
        self.assertEquals(c[key_ts4], 'value15')
        c.close()

        #
        # Separately, we should be able to update key_ts3 at timestamp 10
        # but not update key_ts4 inserted at timestamp 15.
        #
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_ts3] = 'value13'
        self.apply_timestamps(13)
        self.session.commit_transaction()

        '''
        Commented out for now: the system panics if we fail after preparing a transaction.

        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_ts4] = 'value13'
        self.apply_timestamps(13)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.commit_transaction(), msg_ooo)
        c.close()
        '''

        '''
        Commented out for now: the system panics if we fail after preparing a transaction.

        # Make sure multiple update attempts still fail and eventually
        # succeed with a later timestamp. This tests that aborted entries
        # in the update chain are not considered for the timestamp check.
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_ts4] = 'value14'
        self.apply_timestamps(14)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.commit_transaction(), msg_ooo)
        c.close()
        c = self.session.open_cursor(uri)
        self.assertEquals(c[key_ts4], 'value15')
        c.close()
        '''

        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_ts4] = 'value16'
        self.apply_timestamps(16)
        self.session.commit_transaction()
        c.close()
        c = self.session.open_cursor(uri)
        self.assertEquals(c[key_ts4], 'value16')
        c.close()

        # Now try to modify a key previously used with timestamps without
        # one. We should get the inconsistent usage message.
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_ts4] = 'value_nots'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.commit_transaction(), msg_usage)
        c.close()

        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_ts4] = 'value_nots'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.commit_transaction(), msg_usage)
        c.close()
        c = self.session.open_cursor(uri)
        self.assertEquals(c[key_ts4], 'value16')
        c.close()

        # Now confirm the other way. Create a key without a timestamp and then
        # attempt to modify it with a timestamp. The only error checking that
        # makes sense here is the inconsistent usage.
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_nots] = 'value_nots'
        self.session.commit_transaction()
        c.close()

        '''
        Commented out for now: the system panics if we fail after preparing a transaction.

        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_nots] = 'value16'
        self.apply_timestamps(16)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.commit_transaction(), msg_usage)
        c.close()
        '''

        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_nots] = 'value_nots1'
        self.session.commit_transaction()
        c.close()

        '''
        Commented out for now: the system panics if we fail after preparing a transaction.

        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_nots] = 'value17'
        self.apply_timestamps(17)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.commit_transaction(), msg_usage)
        c.close()
        '''

        c = self.session.open_cursor(uri)
        self.assertEquals(c[key_nots], 'value_nots1')
        c.close()

        # Confirm it is okay to set the timestamp in the middle or end of the
        # transaction. That should set the timestamp for the whole thing.
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_ts5] = 'value_notsyet'
        c[key_ts5] = 'value20'
        self.apply_timestamps(20)
        self.session.commit_transaction()
        c.close()

        c = self.session.open_cursor(uri)
        self.assertEquals(c[key_ts5], 'value20')
        c.close()

        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_ts6] = 'value_notsyet'
        c[key_ts6] = 'value21_after'
        self.apply_timestamps(21)
        self.session.commit_transaction()
        c.close()

        c = self.session.open_cursor(uri)
        self.assertEquals(c[key_ts6], 'value21_after')
        c.close()

        # Confirm it is okay to set the durable timestamp on the commit call.
        # That should set the timestamp for the whole thing.
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_ts6] = 'value_committs1'
        c[key_ts6] = 'value22'
        self.session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(22))
        self.session.timestamp_transaction(
            'commit_timestamp=' + self.timestamp_str(22))
        self.session.timestamp_transaction(
            'durable_timestamp=' + self.timestamp_str(22))
        self.session.commit_transaction()
        c.close()

        '''
        Commented out for now: the system panics if we fail after preparing a transaction.

        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_nots] = 'value23'
        self.session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(23))
        self.session.timestamp_transaction(
            'commit_timestamp=' + self.timestamp_str(23))
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.commit_transaction(
            'durable_timestamp=' + self.timestamp_str(23)), msg_usage)
        c.close()
        '''

        # Confirm that rolling back after preparing doesn't fire an assertion.
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_ts6] = 'value24'
        self.session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(24))
        self.session.rollback_transaction()
        c.close()

if __name__ == '__main__':
    wttest.run()
