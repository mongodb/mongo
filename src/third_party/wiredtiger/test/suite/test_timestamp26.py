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
# test_timestamp26.py
#   Timestamps: assert commit settings
#

import wiredtiger, wttest
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# Test write_timestamp_usage never setting.
class test_timestamp26_wtu_never(wttest.WiredTigerTestCase):
    commit_ts = [
        ('yes', dict(commit_ts=True)),
        ('no', dict(commit_ts=False)),
    ]
    with_ts = [
        ('yes', dict(with_ts=True)),
        ('no', dict(with_ts=False)),
    ]
    types = [
        ('fix', dict(key_format='r', value_format='8t')),
        ('row', dict(key_format='S', value_format='S')),
        ('var', dict(key_format='r', value_format='S')),
    ]
    scenarios = make_scenarios(types, commit_ts, with_ts)

    def test_wtu_never(self):
        if wiredtiger.diagnostic_build():
            self.skipTest('requires a non-diagnostic build')

        # Create an object that's never written, it's just used to generate valid k/v pairs.
        ds = SimpleDataSet(
            self, 'file:notused', 10, key_format=self.key_format, value_format=self.value_format)

        # Open the object, configuring write_timestamp usage.
        uri = 'table:ts'
        self.session.create(uri,
            'key_format={},value_format={}'.format(self.key_format, self.value_format) +
            ',write_timestamp_usage=never')

        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[ds.key(7)] = ds.value(8)

        # Commit with a timestamp.
        if self.with_ts:
            # Check both an explicit timestamp set and a set at commit.
            commit_ts = 'commit_timestamp=' + self.timestamp_str(10)
            if not self.commit_ts:
                self.session.timestamp_transaction(commit_ts)
                commit_ts = ''

            msg = '/set when disallowed/'
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: self.session.commit_transaction(commit_ts), msg)

        # Commit without a timestamp.
        else:
            self.session.commit_transaction()

# Test assert read timestamp settings.
class test_timestamp26_read_timestamp(wttest.WiredTigerTestCase):
    read_ts = [
        ('always', dict(read_ts='always')),
        ('never', dict(read_ts='never')),
        ('none', dict(read_ts='none')),
    ]
    types = [
        ('fix', dict(key_format='r', value_format='8t')),
        ('row', dict(key_format='S', value_format='S')),
        ('var', dict(key_format='r', value_format='S')),
    ]
    scenarios = make_scenarios(types, read_ts)

    def test_read_timestamp(self):
        if wiredtiger.diagnostic_build():
            self.skipTest('requires a non-diagnostic build')

        # Create an object that's never written, it's just used to generate valid k/v pairs.
        ds = SimpleDataSet(
            self, 'file:notused', 10, key_format=self.key_format, value_format=self.value_format)

        # Open the object, configuring read timestamp usage.
        uri = 'table:ts'
        self.session.create(uri,
            'key_format={},value_format={}'.format(self.key_format, self.value_format) +
            ',assert=(read_timestamp=' + self.read_ts + ')')

        c = self.session.open_cursor(uri)
        key = ds.key(10)
        value = ds.value(10)

        # Insert a data item at a timestamp (although it doesn't really matter).
        self.session.begin_transaction()
        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(10))
        c[key] = value
        self.session.timestamp_transaction()
        self.session.commit_transaction()

        # Try reading without a timestamp.
        self.session.begin_transaction()
        c.set_key(key)
        if self.read_ts != 'always':
            self.assertEquals(c.search(), 0)
            self.assertEqual(c.get_value(), value)
        else:
            msg = '/read timestamps required and none set/'
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: c.search(), msg)

        self.session.rollback_transaction()

        # Try reading with a timestamp.
        self.session.begin_transaction()
        self.session.timestamp_transaction('read_timestamp=20')
        c.set_key(key)
        if self.read_ts != 'never':
            self.assertEquals(c.search(), 0)
            self.assertEqual(c.get_value(), value)
        else:
            msg = '/read timestamps disallowed/'
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: c.search(), msg)
        self.session.rollback_transaction()

# Test alter of timestamp settings.
class test_timestamp26_alter(wttest.WiredTigerTestCase):
    types = [
        ('fix', dict(key_format='r', value_format='8t')),
        ('row', dict(key_format='S', value_format='S')),
        ('var', dict(key_format='r', value_format='S')),
    ]
    scenarios = make_scenarios(types)

    def test_alter(self):
        if wiredtiger.diagnostic_build():
            self.skipTest('requires a non-diagnostic build')

        # Create an object that's never written, it's just used to generate valid k/v pairs.
        ds = SimpleDataSet(
            self, 'file:notused', 10, key_format=self.key_format, value_format=self.value_format)

        # Open the object, configuring "never" timestamp usage.
        # Check it.
        # Switch the object to "ordered" usage.
        # Check it.
        uri = 'table:ts'
        self.session.create(uri,
            'key_format={},value_format={}'.format(self.key_format, self.value_format) +
            ',write_timestamp_usage=never')

        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[ds.key(10)] = ds.value(10)
        msg = '/set when disallowed by table configuration/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10)),
            msg)
        c.close()

        self.session.alter(uri, 'write_timestamp_usage=ordered')

        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[ds.key(10)] = ds.value(10)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(11))
        self.session.begin_transaction()
        c[ds.key(10)] = ds.value(10)
        msg = '/always use timestamps/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.commit_transaction(), msg)

# Test timestamp settings with alter and inconsistent updates.
class test_timestamp26_alter_inconsistent_update(wttest.WiredTigerTestCase):
    types = [
        ('fix', dict(key_format='r', value_format='8t')),
        ('row', dict(key_format='S', value_format='S')),
        ('var', dict(key_format='r', value_format='S')),
    ]
    scenarios = make_scenarios(types)

    def test_alter_inconsistent_update(self):
        if wiredtiger.diagnostic_build():
            self.skipTest('requires a non-diagnostic build')

        # Create an object that's never written, it's just used to generate valid k/v pairs.
        ds = SimpleDataSet(
            self, 'file:notused', 10, key_format=self.key_format, value_format=self.value_format)

        # Create the table and create a few items with and without timestamps. Then alter the
        # setting and verify the inconsistent usage is detected.
        uri = 'table:ts'
        self.session.create(uri,
            'key_format={},value_format={}'.format(self.key_format, self.value_format))

        c = self.session.open_cursor(uri)
        key = ds.key(10)

        # Insert a data item at timestamp 2.
        self.session.begin_transaction()
        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(2))
        c[key] = ds.value(10)
        self.session.commit_transaction()

        # Update the data item with no timestamp.
        self.session.begin_transaction('no_timestamp=true')
        c[key] = ds.value(11)
        self.session.commit_transaction()

        key = ds.key(12)

        # Insert a non-timestamped item, then update with a timestamp and then without a timestamp.
        self.session.begin_transaction()
        c[key] = ds.value(12)
        self.session.commit_transaction()

        self.session.begin_transaction()
        c[key] = ds.value(13)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))

        self.session.begin_transaction('no_timestamp=true')
        c[key] = ds.value(14)
        self.session.commit_transaction()

        # Now alter the setting and make sure we detect incorrect usage. We must move the oldest
        # timestamp forward in order to alter, otherwise alter will fail with EBUSY.
        c.close()
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))
        self.session.alter(uri, 'write_timestamp_usage=ordered')

        c = self.session.open_cursor(uri)
        key = ds.key(15)

        # Detect decreasing timestamp.
        self.session.begin_transaction()
        c[key] = ds.value(15)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(15))

        msg = '/before the previous update/'
        self.session.begin_transaction()
        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(14))
        c[key] = ds.value(16)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.commit_transaction(), msg)

        # Detect not using a timestamp.
        msg = '/use timestamps once they are first used/'
        self.session.begin_transaction()
        c[key] = ds.value(17)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.commit_transaction(), msg)

# Test timestamp settings with inconsistent updates.
class test_timestamp26_inconsistent_update(wttest.WiredTigerTestCase):
    types = [
        ('fix', dict(key_format='r', value_format='8t')),
        ('row', dict(key_format='S', value_format='S')),
        ('var', dict(key_format='r', value_format='S')),
    ]
    scenarios = make_scenarios(types)

    def test_timestamp_inconsistent_update(self):
        if wiredtiger.diagnostic_build():
            self.skipTest('requires a non-diagnostic build')

        # Create an object that's never written, it's just used to generate valid k/v pairs.
        ds = SimpleDataSet(
            self, 'file:notused', 10, key_format=self.key_format, value_format=self.value_format)

        # Create the table with the key consistency checking turned on. That checking will verify
        # any individual key is always or never used with a timestamp. And if it is used with a
        # timestamp that the timestamps are in increasing order for that key.
        uri = 'table:ts'
        self.session.create(uri,
            'key_format={},value_format={}'.format(self.key_format, self.value_format) +
            ',write_timestamp_usage=ordered')

        c = self.session.open_cursor(uri)
        key = ds.key(1)

        # Insert an item at timestamp 2.
        self.session.begin_transaction()
        c[key] = ds.value(1)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))

        # Upate the data item at timestamp 1, which should fail.
        self.session.begin_transaction()
        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(1))
        c[key] = ds.value(2)
        msg = '/before the previous update/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.commit_transaction(), msg)

        # Make sure we can successfully add a different key at timestamp 1.
        self.session.begin_transaction()
        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(1))
        c[ds.key(2)] = ds.value(3)
        self.session.commit_transaction()
        
        # Insert key1 at timestamp 10 and key2 at 15. Then update both keys in one transaction at
        # timestamp 13, and we should get a complaint about usage.
        key1 = ds.key(3)
        key2 = ds.key(4)
        self.session.begin_transaction()
        c[key1] = ds.value(3)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        self.session.begin_transaction()
        c[key2] = ds.value(4)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(15))

        self.session.begin_transaction()
        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(13))
        c[key1] = ds.value(5)
        c[key2] = ds.value(6)
        msg = '/before the previous update/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.commit_transaction(), msg)
        self.ignoreStdoutPatternIfExists(msg)

    # Try to update a key previously used with timestamps without one. We should get the
    # inconsistent usage error/message.
    def test_timestamp_ts_then_nots(self):
        if wiredtiger.diagnostic_build():
            self.skipTest('requires a non-diagnostic build')

        # Create an object that's never written, it's just used to generate valid k/v pairs.
        ds = SimpleDataSet(
            self, 'file:notused', 10, key_format=self.key_format, value_format=self.value_format)

        # Create the table with the key consistency checking turned on. That checking will verify
        # any individual key is always or never used with a timestamp. And if it is used with a
        # timestamp that the timestamps are in increasing order for that key.
        uri = 'table:ts'
        self.session.create(uri,
            'key_format={},value_format={}'.format(self.key_format, self.value_format) +
            ',write_timestamp_usage=ordered')

        c = self.session.open_cursor(uri)
        key = ds.key(5)

        self.session.begin_transaction()
        c[key] = ds.value(11)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))

        self.session.begin_transaction()
        c[key] = ds.value(12)
        msg ='/configured to always use timestamps once they are first used/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.commit_transaction(), msg)

        self.ignoreStdoutPatternIfExists(msg)

    # Smoke test setting the timestamp at various points in the transaction.
    def test_timestamp_ts_order(self):
        if wiredtiger.diagnostic_build():
            self.skipTest('requires a non-diagnostic build')

        # Create an object that's never written, it's just used to generate valid k/v pairs.
        ds = SimpleDataSet(
            self, 'file:notused', 10, key_format=self.key_format, value_format=self.value_format)

        # Create the table with the key consistency checking turned on. That checking will verify
        # any individual key is always or never used with a timestamp. And if it is used with a
        # timestamp that the timestamps are in increasing order for that key.
        uri = 'table:ts'
        self.session.create(uri,
            'key_format={},value_format={}'.format(self.key_format, self.value_format) +
            ',write_timestamp_usage=ordered')

        c = self.session.open_cursor(uri)
        key1 = ds.key(6)
        key2 = ds.key(7)

        self.session.begin_transaction()
        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(30))
        c[key1] = ds.value(14)
        c[key2] = ds.value(15)
        self.session.commit_transaction()
        self.assertEquals(c[key1], ds.value(14))
        self.assertEquals(c[key2], ds.value(15))

        self.session.begin_transaction()
        c[key1] = ds.value(16)
        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(31))
        c[key2] = ds.value(17)
        self.session.commit_transaction()
        self.assertEquals(c[key1], ds.value(16))
        self.assertEquals(c[key2], ds.value(17))

        self.session.begin_transaction()
        c[key1] = ds.value(18)
        c[key2] = ds.value(19)
        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(32))
        self.session.commit_transaction()
        self.assertEquals(c[key1], ds.value(18))
        self.assertEquals(c[key2], ds.value(19))

        self.session.begin_transaction()
        c[key1] = ds.value(20)
        c[key2] = ds.value(21)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(33))
        self.assertEquals(c[key1], ds.value(20))
        self.assertEquals(c[key2], ds.value(21))

# Test that timestamps are ignored in logged files.
class test_timestamp26_log_ts(wttest.WiredTigerTestCase):
    # Turn on logging to cause timestamps to be ignored.
    conn_config = 'log=(enabled=true)'

    types = [
        ('fix', dict(key_format='r', value_format='8t')),
        ('row', dict(key_format='S', value_format='S')),
        ('var', dict(key_format='r', value_format='S')),
    ]
    always = [
        ('always', dict(always=True)),
        ('never', dict(always=False)),
    ]
    scenarios = make_scenarios(types, always)

    # Smoke test that logged files don't complain about timestamps.
    def test_log_ts(self):
        if wiredtiger.diagnostic_build():
            self.skipTest('requires a non-diagnostic build')

        # Create an object that's never written, it's just used to generate valid k/v pairs.
        ds = SimpleDataSet(
            self, 'file:notused', 10, key_format=self.key_format, value_format=self.value_format)

        # Open the object, configuring write_timestamp usage.
        uri = 'table:ts'
        config = ',write_timestamp_usage='
        config += 'always' if self.always else 'never'
        self.session.create(uri,
            'key_format={},value_format={}'.format(self.key_format, self.value_format) + config)

        c = self.session.open_cursor(uri)

        # Commit with a timestamp.
        self.session.begin_transaction()
        c[ds.key(1)] = ds.value(1)
        self.session.breakpoint()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))

        # Commit without a timestamp.
        self.session.begin_transaction()
        c[ds.key(2)] = ds.value(2)
        self.session.commit_transaction()

# Test that timestamps are ignored in in-memory configurations and that object configurations always
# override.
class test_timestamp26_in_memory_ts(wttest.WiredTigerTestCase):
    types = [
        ('fix', dict(key_format='r', value_format='8t')),
        ('row', dict(key_format='S', value_format='S')),
        ('var', dict(key_format='r', value_format='S')),
    ]

    # The two connection configurations that default to ignoring timestamps.
    conn = [
        ('true', dict(conn_config='in_memory=true')),
        ('false', dict(conn_config='log=(enabled=true)')),
    ]

    # Objects can explicitly enable or ignore timestamps or default to the environment's behavior.
    object = [
        ('true', dict(obj_ignore=True, obj_config='log=(enabled=true)')),
        ('false', dict(obj_ignore=False, obj_config='log=(enabled=false)')),
        ('default', dict(obj_ignore=True, obj_config='')),
    ]

    always = [
        ('always', dict(always=True)),
        ('never', dict(always=False)),
    ]
    scenarios = make_scenarios(types, conn, object, always)

    # Test that timestamps are ignored in in-memory configurations and that object configurations
    # always override.
    def test_in_memory_ts(self):
        if wiredtiger.diagnostic_build():
            self.skipTest('requires a non-diagnostic build')

        # Create an object that's never written, it's just used to generate valid k/v pairs.
        ds = SimpleDataSet(
            self, 'file:notused', 10, key_format=self.key_format, value_format=self.value_format)

        # Open the object, configuring write_timestamp usage.
        uri = 'table:ts'
        config = ',' + self.obj_config
        config += ',write_timestamp_usage='
        config += 'ordered' if self.always else 'never'
        self.session.breakpoint()
        self.session.create(uri,
            'key_format={},value_format={}'.format(self.key_format, self.value_format) + config)

        c = self.session.open_cursor(uri)

        # Commit with a timestamp.
        self.session.begin_transaction()
        c[ds.key(1)] = ds.value(1)
        if self.always == True or self.obj_ignore == True:
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(1))
        else:
            msg = '/unexpected timestamp usage/'
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(1)),
                msg)

        # Commit without a timestamp (but first with a timestamp if in ordered mode so we get
        # a failure).
        if self.always:
            self.session.begin_transaction()
            c[ds.key(2)] = ds.value(2)
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))
        self.session.begin_transaction()
        c[ds.key(2)] = ds.value(2)
        if self.always == False or self.obj_ignore == True:
            self.session.commit_transaction()
        else:
            msg = '/no timestamp provided/'
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: self.session.commit_transaction(), msg)

        self.ignoreStdoutPatternIfExists('/unexpected timestamp usage/')
        self.ignoreStdoutPatternIfExists('/no timestamp provided/')
if __name__ == '__main__':
    wttest.run()
