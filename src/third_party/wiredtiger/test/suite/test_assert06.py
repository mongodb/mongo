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
from wtdataset import SimpleDataSet

class test_assert06(wttest.WiredTigerTestCase, suite_subprocess):
    key_format_values = [
        ('fix', dict(key_format='r', value_format='8t')),
        ('row', dict(key_format='S', value_format='S')),
        ('var', dict(key_format='r', value_format='S')),
    ]
    scenarios = make_scenarios(key_format_values)

    msg_usage='/use timestamps once they are first used/'

    def apply_timestamps(self, timestamp, prepare):
        if prepare:
            self.session.prepare_transaction(
                'prepare_timestamp=' + self.timestamp_str(timestamp))
        self.session.timestamp_transaction(
            'commit_timestamp=' + self.timestamp_str(timestamp))
        if prepare:
            self.session.timestamp_transaction(
                'durable_timestamp=' + self.timestamp_str(timestamp))

    def test_timestamp_alter(self):
        if wiredtiger.diagnostic_build():
            self.skipTest('requires a non-diagnostic build')

        # Create an object that's never written, it's just used to generate valid k/v pairs.
        ds = SimpleDataSet(
            self, 'file:notused', 10, key_format=self.key_format, value_format=self.value_format)

        cfg_on = 'write_timestamp_usage=ordered'
        cfg_off = 'write_timestamp_usage=none'

        # Create a few items with and without timestamps.
        # Then alter the setting and verify the inconsistent usage is detected.
        uri = 'file:assert06'
        self.session.create(uri,
            'key_format={},value_format={}'.format(self.key_format, self.value_format))
        c = self.session.open_cursor(uri)

        # Insert a data item at timestamp 2.
        key = ds.key(1)
        self.session.begin_transaction()
        c[key] = ds.value(1)
        self.apply_timestamps(2, True)
        self.session.commit_transaction()

        # Modify the data item without a timestamp.
        self.session.begin_transaction('no_timestamp=true')
        c[key] = ds.value(2)
        self.session.commit_transaction()

        # Insert an item without a timestamp.
        # Then modify with a timestamp.
        # Then modify without a timestamp.
        key = ds.key(2)
        self.session.begin_transaction('no_timestamp=true')
        c[key] = ds.value(3)
        self.session.commit_transaction()
        self.session.begin_transaction()
        c[key] = ds.value(4)
        self.apply_timestamps(2, True)
        self.session.commit_transaction()
        self.session.begin_transaction('no_timestamp=true')
        c[key] = ds.value(5)
        self.session.commit_transaction()

        # Now alter the setting and make sure we detect incorrect usage.
        # We must move the oldest timestamp forward in order to alter, otherwise alter closing the
        # file will fail with EBUSY.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(2))
        c.close()
        self.session.alter(uri, cfg_on)
        c = self.session.open_cursor(uri)

        # Update at timestamp 5, then detect not using a timestamp.
        key = ds.key(3)
        self.session.begin_transaction()
        c[key] = ds.value(6)
        self.apply_timestamps(5, True)
        self.session.commit_transaction()
        self.session.begin_transaction()
        c[key] = ds.value(6)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.commit_transaction(), self.msg_usage)

        self.ignoreStdoutPatternIfExists(self.msg_usage)

    def test_timestamp_usage(self):
        if wiredtiger.diagnostic_build():
            self.skipTest('requires a non-diagnostic build')

        # Create an object that's never written, it's just used to generate valid k/v pairs.
        ds = SimpleDataSet(
            self, 'file:notused', 10, key_format=self.key_format, value_format=self.value_format)

        # Create the table with the key consistency checking turned on. That checking will verify
        # any individual key is always or never used with a timestamp. And if it is used with a
        # timestamp that the timestamps are in increasing order for that key.
        uri = 'file:assert06'
        self.session.create(uri,
            'key_format={},value_format={},'.format(self.key_format, self.value_format) +
            'write_timestamp_usage=ordered,assert=(write_timestamp=on)')
        c = self.session.open_cursor(uri)

        # Insert a data item at timestamp 2.
        self.session.begin_transaction()
        c[ds.key(1)] = ds.value(1)
        self.apply_timestamps(2, True)
        self.session.commit_transaction()

        # Make sure we can successfully add a different key at timestamp 1.
        self.session.begin_transaction()
        c[ds.key(2)] = ds.value(2)
        self.apply_timestamps(1, True)
        self.session.commit_transaction()

        # Insert key_ts3 at timestamp 10 and key_ts4 at 15, then modify both keys in one transaction
        # at timestamp 13, which should result in an error message.
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[ds.key(3)] = ds.value(3)
        self.apply_timestamps(10, True)
        self.session.commit_transaction()
        self.session.begin_transaction()
        c[ds.key(4)] = ds.value(4)
        self.apply_timestamps(15, True)
        self.session.commit_transaction()
        self.session.begin_transaction()
        c[ds.key(3)] = ds.value(5)
        c[ds.key(4)] = ds.value(6)
        self.apply_timestamps(13, False)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.commit_transaction(), '/unexpected timestamp usage/')
        self.assertEquals(c[ds.key(3)], ds.value(3))
        self.assertEquals(c[ds.key(4)], ds.value(4))

        # Modify a key previously used with timestamps without one. We should get the inconsistent
        # usage message.
        key = ds.key(5)
        self.session.begin_transaction()
        c[key] = ds.value(7)
        self.apply_timestamps(14, True)
        self.session.commit_transaction()
        self.session.begin_transaction()
        c[key] = ds.value(8)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.commit_transaction(), self.msg_usage)

        # Set the timestamp in the beginning, middle or end of the transaction.
        key = ds.key(6)
        self.session.begin_transaction()
        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(16))
        c[key] = ds.value(9)
        self.session.commit_transaction()
        self.assertEquals(c[key], ds.value(9))

        key = ds.key(7)
        self.session.begin_transaction()
        c[key] = ds.value(10)
        c[key] = ds.value(11)
        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(17))
        c[key] = ds.value(12)
        c[key] = ds.value(13)
        self.session.commit_transaction()
        self.assertEquals(c[key], ds.value(13))

        key = ds.key(8)
        self.session.begin_transaction()
        c[key] = ds.value(14)
        self.apply_timestamps(18, True)
        self.session.commit_transaction()
        self.assertEquals(c[key], ds.value(14))

        # Confirm it is okay to set the durable timestamp on the commit call.
        key = ds.key(9)
        self.session.begin_transaction()
        c[key] = ds.value(15)
        c[key] = ds.value(16)
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(22))
        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(22))
        self.session.timestamp_transaction('durable_timestamp=' + self.timestamp_str(22))
        self.session.commit_transaction()

        # Confirm that rolling back after preparing doesn't fire an assertion.
        key = ds.key(10)
        self.session.begin_transaction()
        c[key] = ds.value(17)
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(30))
        self.session.rollback_transaction()

        self.ignoreStdoutPatternIfExists('/unexpected timestamp usage/')

if __name__ == '__main__':
    wttest.run()
