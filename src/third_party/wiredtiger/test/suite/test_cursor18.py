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
# test_cursor18.py
#   Test version cursor under various scenarios.
#
import wttest
import wiredtiger
from wtscenario import make_scenarios

WT_TS_MAX = 2**64-1

class test_cursor18(wttest.WiredTigerTestCase):
    uri = 'file:test_cursor18.wt'

    types = [
        ('row', dict(keyformat='i', valueformat='i')),
        ('var', dict(keyformat='r', valueformat='i')),
        ('fix', dict(keyformat='r', valueformat='8t')),
    ]

    cross_key_config = [
        ('default', dict(cross_key_config='')),
        ('cross_key', dict(cross_key_config='cross_key=true'))
    ]

    scenarios = make_scenarios(types, cross_key_config)

    def create(self):
        self.session.create(self.uri, 'key_format={},value_format={}'.format(self.keyformat, self.valueformat))

    def verify_value(self, version_cursor, expected_start_ts, expected_start_durable_ts, expected_stop_ts, expected_stop_durable_ts, expected_type, expected_prepare_state, expected_flags, expected_location, expected_value):
        values = version_cursor.get_values()
        # Ignore the transaction ids from the value in the verification
        self.assertEqual(values[1], expected_start_ts)
        self.assertEqual(values[2], expected_start_durable_ts)
        self.assertEqual(values[4], expected_stop_ts)
        self.assertEqual(values[5], expected_stop_durable_ts)
        self.assertEqual(values[6], expected_type)
        self.assertEqual(values[7], expected_prepare_state)
        self.assertEqual(values[8], expected_flags)
        self.assertEqual(values[9], expected_location)
        self.assertEqual(values[10], expected_value)

    def open_version_cursor(self, visible_only=False, start_ts=None):
        internal_config = ["enabled=true", self.cross_key_config]
        if visible_only:
            internal_config.append("visible_only=true")
        if start_ts is not None:
            internal_config.append("start_timestamp=" + self.timestamp_str(start_ts))

        config = "debug=(dump_version=(" + ",".join(internal_config) + "))"

        return self.session.open_cursor(self.uri, None, config)

    def test_update_chain_only(self):
        self.create()

        cursor = self.session.open_cursor(self.uri, None)
        # Add a value to the update chain
        self.session.begin_transaction()
        cursor[1] = 0
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        # Update the value
        self.session.begin_transaction()
        cursor[1] = 1
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(5))

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.open_version_cursor()
        version_cursor.set_key(1)
        self.assertEqual(version_cursor.search(), 0)
        self.assertEqual(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 5, 5, WT_TS_MAX, 0, 3, 0, 0, 0, 1)
        self.assertEqual(version_cursor.next(), 0)
        self.assertEqual(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 1, 1, 5, 5, 3, 0, 0, 0, 0)
        self.assertEqual(version_cursor.next(), wiredtiger.WT_NOTFOUND)

    def test_ondisk_only(self):
        self.create()

        cursor = self.session.open_cursor(self.uri, None)
        # Add a value to the update chain
        self.session.begin_transaction()
        cursor[1] = 0
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        self.assertEqual(evict_cursor[1], 0)
        evict_cursor.reset()
        self.session.rollback_transaction()

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.open_version_cursor()
        version_cursor.set_key(1)
        self.assertEqual(version_cursor.search(), 0)
        self.assertEqual(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 1, 1, WT_TS_MAX, 0, 3, 0, 0, 1, 0)
        self.assertEqual(version_cursor.next(), wiredtiger.WT_NOTFOUND)

    def test_ondisk_only_with_deletion(self):
        self.create()

        cursor = self.session.open_cursor(self.uri, None)
        # Add a value to the update chain
        self.session.begin_transaction()
        cursor[1] = 0
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        # Delete the value
        self.session.begin_transaction()
        cursor.set_key(1)
        self.assertEqual(cursor.remove(), 0)
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(5))

        evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        evict_cursor.set_key(1)
        if self.valueformat == '8t':
            self.assertEqual(evict_cursor.search(), 0)
        else:
            self.assertEqual(evict_cursor.search(), wiredtiger.WT_NOTFOUND)
        evict_cursor.reset()
        self.session.rollback_transaction()

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.open_version_cursor()
        version_cursor.set_key(1)
        self.assertEqual(version_cursor.search(), 0)
        self.assertEqual(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 1, 1, 5, 5, 3, 0, 0, 1, 0)
        self.assertEqual(version_cursor.next(), wiredtiger.WT_NOTFOUND)

    def test_ondisk_with_deletion_on_update_chain(self):
        self.create()

        cursor = self.session.open_cursor(self.uri, None)
        # Add a value to the update chain
        self.session.begin_transaction()
        cursor[1] = 0
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        self.assertEqual(evict_cursor[1], 0)
        evict_cursor.reset()
        self.session.rollback_transaction()

        # Delete the value
        self.session.begin_transaction()
        cursor.set_key(1)
        self.assertEqual(cursor.remove(), 0)
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(5))

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.open_version_cursor()
        version_cursor.set_key(1)
        self.assertEqual(version_cursor.search(), 0)
        self.assertEqual(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 1, 1, 5, 5, 3, 0, 0, 1, 0)
        self.assertEqual(version_cursor.next(), wiredtiger.WT_NOTFOUND)

    def test_ondisk_with_hs(self):
        self.create()

        cursor = self.session.open_cursor(self.uri, None)
        # Add a value to the update chain
        self.session.begin_transaction()
        cursor[1] = 0
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        # Update the value
        self.session.begin_transaction()
        cursor[1] = 1
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(5))

        evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        self.assertEqual(evict_cursor[1], 1)
        evict_cursor.reset()
        self.session.rollback_transaction()

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.open_version_cursor()
        version_cursor.set_key(1)
        self.assertEqual(version_cursor.search(), 0)
        self.assertEqual(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 5, 5, WT_TS_MAX, 0, 3, 0, 0, 1, 1)
        self.assertEqual(version_cursor.next(), 0)
        self.assertEqual(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 1, 1, 5, 5, 3, 0, 0, 2, 0)
        self.assertEqual(version_cursor.next(), wiredtiger.WT_NOTFOUND)

    def test_update_chain_ondisk_hs(self):
        self.create()

        cursor = self.session.open_cursor(self.uri, None)
        # Add a value to the update chain
        self.session.begin_transaction()
        cursor[1] = 0
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        # Update the value
        self.session.begin_transaction()
        cursor[1] = 1
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(5))

        evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        self.assertEqual(evict_cursor[1], 1)
        evict_cursor.reset()
        self.session.rollback_transaction()

        # Update the value
        self.session.begin_transaction()
        cursor[1] = 2
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(10))

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.open_version_cursor()
        version_cursor.set_key(1)
        self.assertEqual(version_cursor.search(), 0)
        self.assertEqual(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 10, 10, WT_TS_MAX, 0, 3, 0, 0, 0, 2)
        self.assertEqual(version_cursor.next(), 0)
        self.assertEqual(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 5, 5, 10, 10, 3, 0, 0, 1, 1)
        self.assertEqual(version_cursor.next(), 0)
        self.assertEqual(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 1, 1, 5, 5, 3, 0, 0, 2, 0)
        self.assertEqual(version_cursor.next(), wiredtiger.WT_NOTFOUND)
        self.session.rollback_transaction()

        # We want to test that the version cursor is working as expected when used multiple times
        # on the same table on different keys.
        self.session.begin_transaction()
        cursor[2] = 1
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(11))

        self.session.begin_transaction()
        cursor[2] = 2
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(15))

        self.session.begin_transaction()
        self.assertEqual(evict_cursor[2], 2)
        evict_cursor.reset()
        self.session.rollback_transaction()

        self.session.begin_transaction()
        cursor[2] = 3
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(20))

        # Ensure that we are able to correctly traverse all versions of this new key.
        self.session.begin_transaction()
        version_cursor.set_key(2)
        self.assertEqual(version_cursor.search(), 0)
        self.assertEqual(version_cursor.get_key(), 2)
        self.verify_value(version_cursor, 20, 20, WT_TS_MAX, 0, 3, 0, 0, 0, 3)
        self.assertEqual(version_cursor.next(), 0)
        self.assertEqual(version_cursor.get_key(), 2)
        self.verify_value(version_cursor, 15, 15, 20, 20, 3, 0, 0, 1, 2)
        self.assertEqual(version_cursor.next(), 0)
        self.assertEqual(version_cursor.get_key(), 2)
        self.verify_value(version_cursor, 11, 11, 15, 15, 3, 0, 0, 2, 1)
        self.assertEqual(version_cursor.next(), wiredtiger.WT_NOTFOUND)

    def test_prepare(self):
        self.create()

        session2 = self.conn.open_session()
        cursor = session2.open_cursor(self.uri, None)
        # Add a value to the update chain
        session2.begin_transaction()
        cursor[1] = 0
        session2.prepare_transaction("prepare_timestamp=" + self.timestamp_str(1))

        evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        evict_cursor.set_key(1)
        try:
            evict_cursor.search()
        except wiredtiger.WiredTigerError as e:
            if wiredtiger.wiredtiger_strerror(wiredtiger.WT_PREPARE_CONFLICT) not in str(e):
                raise e
        evict_cursor.reset()
        self.session.rollback_transaction()

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.open_version_cursor()
        version_cursor.set_key(1)
        self.assertEqual(version_cursor.search(), 0)
        self.assertEqual(version_cursor.get_key(), 1)

        self.verify_value(version_cursor, 1, 0, WT_TS_MAX, 0, 3, 1, 64, 0, 0)
        self.assertEqual(version_cursor.next(), 0)
        self.assertEqual(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 1, 1, 1, 0, 3, 1, 0, 1, 0)
        self.assertEqual(version_cursor.next(), wiredtiger.WT_NOTFOUND)

    def test_reuse_version_cursor(self):
        self.create()

        cursor = self.session.open_cursor(self.uri, None)
        # Add a value to the update chain
        self.session.begin_transaction()
        cursor[1] = 0
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        self.assertEqual(evict_cursor[1], 0)
        evict_cursor.reset()
        self.session.rollback_transaction()

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.open_version_cursor()
        version_cursor.set_key(1)
        self.assertEqual(version_cursor.search(), 0)
        self.assertEqual(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 1, 1, WT_TS_MAX, 0, 3, 0, 0, 1, 0)
        self.assertEqual(version_cursor.next(), wiredtiger.WT_NOTFOUND)

        # Repeat after reset
        version_cursor.reset()
        version_cursor.set_key(1)
        self.assertEqual(version_cursor.search(), 0)
        self.assertEqual(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 1, 1, WT_TS_MAX, 0, 3, 0, 0, 1, 0)
        self.assertEqual(version_cursor.next(), wiredtiger.WT_NOTFOUND)

    def test_prepare_tombstone(self):
        self.create()

        cursor = self.session.open_cursor(self.uri, None)
        # Add a value to the update chain
        self.session.begin_transaction()
        cursor[1] = 0
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        self.assertEqual(evict_cursor[1], 0)
        evict_cursor.reset()
        self.session.rollback_transaction()

        # Delete the value with prepare
        session2 = self.conn.open_session()
        cursor2 = session2.open_cursor(self.uri, None)
        session2.begin_transaction()
        cursor2.set_key(1)
        self.assertEqual(cursor2.remove(), 0)
        session2.prepare_transaction("prepare_timestamp=" + self.timestamp_str(2))

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.open_version_cursor()
        version_cursor.set_key(1)
        self.assertEqual(version_cursor.search(), 0)
        self.assertEqual(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 1, 1, 2, 0, 3, 1, 0, 1, 0)
        self.assertEqual(version_cursor.next(), wiredtiger.WT_NOTFOUND)

    def test_search_when_positioned(self):
        self.create()

        cursor = self.session.open_cursor(self.uri, None)
        # Add a value to the update chain
        self.session.begin_transaction()
        cursor[1] = 0
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.open_version_cursor()
        version_cursor.set_key(1)
        self.assertEqual(version_cursor.search(), 0)
        try:
            version_cursor.search()
        except wiredtiger.WiredTigerError as e:
            gotException = True
            self.pr('got expected exception: ' + str(e))
            self.assertTrue(str(e).find('WT_ROLLBACK') >= 0)
        self.assertTrue(gotException, msg = 'expected exception')

    def test_concurrent_insert(self):
        self.create()

        cursor = self.session.open_cursor(self.uri, None)
        # Add a value to the update chain
        self.session.begin_transaction()
        cursor[1] = 0
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        # Update the value
        self.session.begin_transaction()
        cursor[1] = 1
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(5))

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.open_version_cursor()

        version_cursor.set_key(1)
        self.assertEqual(version_cursor.search(), 0)
        self.assertEqual(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 5, 5, WT_TS_MAX, 0, 3, 0, 0, 0, 1)

        # Update the value
        session2 = self.conn.open_session()
        cursor2 = session2.open_cursor(self.uri, None)
        session2.begin_transaction()
        cursor2[1] = 2
        session2.commit_transaction("commit_timestamp=" + self.timestamp_str(5))

        self.assertEqual(version_cursor.next(), 0)
        self.assertEqual(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 1, 1, 5, 5, 3, 0, 0, 0, 0)
        self.assertEqual(version_cursor.next(), wiredtiger.WT_NOTFOUND)

    def test_skip_invisible_updates(self):
        self.create()

        session2 = self.conn.open_session()
        cursor = session2.open_cursor(self.uri, None)
        # Add a value to the update chain
        session2.begin_transaction()
        cursor[1] = 0

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.open_version_cursor(visible_only=True)
        version_cursor.set_key(1)
        self.assertEqual(version_cursor.search(), wiredtiger.WT_NOTFOUND)

    def test_skip_prepare_update_chain(self):
        self.create()

        session2 = self.conn.open_session()
        cursor = session2.open_cursor(self.uri, None)
        # Add a value to the update chain
        session2.begin_transaction()
        cursor[1] = 0
        session2.prepare_transaction("prepare_timestamp=" + self.timestamp_str(1))

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.open_version_cursor(visible_only=True)
        version_cursor.set_key(1)
        self.assertEqual(version_cursor.search(), wiredtiger.WT_NOTFOUND)

    def test_skip_prepare_on_disk(self):
        self.create()

        session2 = self.conn.open_session()
        cursor = session2.open_cursor(self.uri, None)
        # Add a value to the update chain
        session2.begin_transaction()
        cursor[1] = 0
        session2.prepare_transaction("prepare_timestamp=" + self.timestamp_str(1))

        evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        evict_cursor.set_key(1)
        try:
            evict_cursor.search()
        except wiredtiger.WiredTigerError as e:
            if wiredtiger.wiredtiger_strerror(wiredtiger.WT_PREPARE_CONFLICT) not in str(e):
                raise e
        evict_cursor.reset()
        self.session.rollback_transaction()

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.open_version_cursor(visible_only=True)
        version_cursor.set_key(1)
        self.assertEqual(version_cursor.search(), wiredtiger.WT_NOTFOUND)

    def test_skip_prepare_tombstone_and_full_value_on_disk(self):
        self.create()

        session2 = self.conn.open_session()
        cursor = session2.open_cursor(self.uri, None)
        # Add a value to the update chain
        session2.begin_transaction()
        cursor[1] = 0
        cursor.set_key(1)
        cursor.remove()
        session2.prepare_transaction("prepare_timestamp=" + self.timestamp_str(1))

        evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        evict_cursor.set_key(1)
        try:
            evict_cursor.search()
        except wiredtiger.WiredTigerError as e:
            if wiredtiger.wiredtiger_strerror(wiredtiger.WT_PREPARE_CONFLICT) not in str(e):
                raise e
        evict_cursor.reset()
        self.session.rollback_transaction()

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.open_version_cursor(visible_only=True)
        version_cursor.set_key(1)
        self.assertEqual(version_cursor.search(), wiredtiger.WT_NOTFOUND)

    def test_skip_tombstone_on_disk(self):
        self.create()

        session2 = self.conn.open_session()
        cursor = session2.open_cursor(self.uri, None)
        # Add a value to the update chain
        session2.begin_transaction()
        cursor[1] = 0
        session2.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        # Delete the value with prepare
        session2.begin_transaction()
        cursor.set_key(1)
        cursor.remove()
        session2.prepare_transaction("prepare_timestamp=" + self.timestamp_str(2))

        evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        evict_cursor.set_key(1)
        try:
            evict_cursor.search()
        except wiredtiger.WiredTigerError as e:
            if wiredtiger.wiredtiger_strerror(wiredtiger.WT_PREPARE_CONFLICT) not in str(e):
                raise e
        evict_cursor.reset()
        self.session.rollback_transaction()

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.open_version_cursor(visible_only=True)
        version_cursor.set_key(1)
        self.assertEqual(version_cursor.search(), 0)
        self.assertEqual(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 1, 1, WT_TS_MAX, 0, 3, 0, 0, 0, 0)
        self.assertEqual(version_cursor.next(), 0)
        self.verify_value(version_cursor, 1, 1, WT_TS_MAX, 0, 3, 0, 0, 1, 0)
        self.assertEqual(version_cursor.next(), wiredtiger.WT_NOTFOUND)

    def test_unpositioned_cursor(self):
        self.create()

        # Add a key/value pair to the table
        cursor = self.session.open_cursor(self.uri, None)
        self.session.begin_transaction()
        cursor[1] = 0
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        version_cursor = self.open_version_cursor()

        if not self.cross_key_config:
            self.assertRaises(wiredtiger.WiredTigerError, lambda: version_cursor.next())
        else:
            # Cross-key version cursors are allowed to iterate from the unpositioned state.
            self.assertEqual(version_cursor.next(), 0)
            self.assertEqual(version_cursor.get_key(), 1)
            self.verify_value(version_cursor, 1, 1, WT_TS_MAX, 0, 3, 0, 0, 0, 0)

    def test_multiple_keys(self):
        self.create()

        cursor = self.session.open_cursor(self.uri, None)

        keys = range(1, 10)
        # Add multiple values
        self.session.begin_transaction()
        for key in keys:
            cursor[key] = 0
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        # Update the values
        self.session.begin_transaction()
        for key in keys:
            cursor[key] = 1
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(5))

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.open_version_cursor()
        version_cursor.set_key(keys[0])
        self.assertEqual(version_cursor.search(), 0)

        # By default version cursor cannot iterate accross multiple keys
        if not self.cross_key_config:
            keys = [1]

        for key in keys:
            self.assertEqual(version_cursor.get_key(), key)
            self.verify_value(version_cursor, 5, 5, WT_TS_MAX, 0, 3, 0, 0, 0, 1)

            self.assertEqual(version_cursor.next(), 0)
            self.assertEqual(version_cursor.get_key(), key)
            self.verify_value(version_cursor, 1, 1, 5, 5, 3, 0, 0, 0, 0)

            err_code = wiredtiger.WT_NOTFOUND if key == keys[-1] else 0
            self.assertEqual(version_cursor.next(), err_code)

    def test_update_chain_start_timestamp(self):
        self.create()
        cursor = self.session.open_cursor(self.uri, None)

        # Add a value to the update chain
        self.session.begin_transaction()
        cursor[1] = 0
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        # Update the value
        self.session.begin_transaction()
        cursor[1] = 1
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(5))

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.open_version_cursor(start_ts=1)
        version_cursor.set_key(1)
        self.assertEqual(version_cursor.search(), 0)
        self.assertEqual(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 5, 5, WT_TS_MAX, 0, 3, 0, 0, 0, 1)
        self.assertEqual(version_cursor.next(), wiredtiger.WT_NOTFOUND)

    def test_update_chain_start_timestamp_with_remove(self):
        self.create()

        cursor = self.session.open_cursor(self.uri, None)

        # Add a value to the update chain
        self.session.begin_transaction()
        cursor[1] = 0
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        # Add a value to the update chain
        self.session.begin_transaction()
        cursor.set_key(1)
        cursor.remove()
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(2))

        # Update the value
        self.session.begin_transaction()
        cursor[1] = 1
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(5))

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.open_version_cursor(start_ts=1)
        version_cursor.set_key(1)
        self.assertEqual(version_cursor.search(), 0)
        self.assertEqual(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 5, 5, WT_TS_MAX, 0, 3, 0, 0, 0, 1)
        self.assertEqual(version_cursor.next(), 0)
        self.assertEqual(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 1, 1, 2, 2, 3, 0, 0, 0, 0)
        self.assertEqual(version_cursor.next(), wiredtiger.WT_NOTFOUND)

    def test_update_chain_start_timestamp_with_remove_exclusive(self):
        self.create()

        cursor = self.session.open_cursor(self.uri, None)

        # Add a value to the update chain
        self.session.begin_transaction()
        cursor[1] = 0
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        # Add a value to the update chain
        self.session.begin_transaction()
        cursor.set_key(1)
        cursor.remove()
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(2))

        # Update the value
        self.session.begin_transaction()
        cursor[1] = 1
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(5))

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.open_version_cursor(start_ts=2)
        version_cursor.set_key(1)
        self.assertEqual(version_cursor.search(), 0)
        self.assertEqual(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 5, 5, WT_TS_MAX, 0, 3, 0, 0, 0, 1)
        self.assertEqual(version_cursor.next(), wiredtiger.WT_NOTFOUND)

    def test_ondisk_start_timestamp(self):
        self.create()

        cursor = self.session.open_cursor(self.uri, None)

        # Add a value to the update chain
        self.session.begin_transaction()
        cursor[1] = 0
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        self.assertEqual(evict_cursor[1], 0)
        evict_cursor.reset()
        self.session.rollback_transaction()

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.open_version_cursor(start_ts=1)
        version_cursor.set_key(1)
        self.assertEqual(version_cursor.search(), wiredtiger.WT_NOTFOUND)

    def test_ondisk_with_deletion_on_update_chain_start_timestamp(self):
        self.create()

        cursor = self.session.open_cursor(self.uri, None)

        # Add a value to the update chain
        self.session.begin_transaction()
        cursor[1] = 0
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        self.assertEqual(evict_cursor[1], 0)
        evict_cursor.reset()
        self.session.rollback_transaction()

        # Delete the value
        self.session.begin_transaction()
        cursor.set_key(1)
        self.assertEqual(cursor.remove(), 0)
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(5))

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.open_version_cursor(start_ts=1)
        version_cursor.set_key(1)
        self.assertEqual(version_cursor.search(), 0)
        self.assertEqual(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 1, 1, 5, 5, 3, 0, 0, 1, 0)
        self.assertEqual(version_cursor.next(), wiredtiger.WT_NOTFOUND)

    def test_ondisk_with_deletion_on_update_chain_start_timestamp_exclusive(self):
        self.create()

        cursor = self.session.open_cursor(self.uri, None)

        # Add a value to the update chain
        self.session.begin_transaction()
        cursor[1] = 0
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        self.assertEqual(evict_cursor[1], 0)
        evict_cursor.reset()
        self.session.rollback_transaction()

        # Delete the value
        self.session.begin_transaction()
        cursor.set_key(1)
        self.assertEqual(cursor.remove(), 0)
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(5))

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.open_version_cursor(start_ts=5)
        version_cursor.set_key(1)
        self.assertEqual(version_cursor.search(), wiredtiger.WT_NOTFOUND)

    def test_ondisk_only_with_deletion_start_timestamp(self):
        self.create()

        cursor = self.session.open_cursor(self.uri, None)

        # Add a value to the update chain
        self.session.begin_transaction()
        cursor[1] = 0
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        # Delete the value
        self.session.begin_transaction()
        cursor.set_key(1)
        self.assertEqual(cursor.remove(), 0)
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(5))

        evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        evict_cursor.set_key(1)
        if self.valueformat == '8t':
            self.assertEqual(evict_cursor.search(), 0)
        else:
            self.assertEqual(evict_cursor.search(), wiredtiger.WT_NOTFOUND)
        evict_cursor.reset()
        self.session.rollback_transaction()

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.open_version_cursor(start_ts=1)
        version_cursor.set_key(1)
        self.assertEqual(version_cursor.search(), 0)
        self.assertEqual(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 1, 1, 5, 5, 3, 0, 0, 1, 0)
        self.assertEqual(version_cursor.next(), wiredtiger.WT_NOTFOUND)

    def test_ondisk_only_with_deletion_start_timestamp_exclusive(self):
        self.create()

        cursor = self.session.open_cursor(self.uri, None)

        # Add a value to the update chain
        self.session.begin_transaction()
        cursor[1] = 0
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        # Delete the value
        self.session.begin_transaction()
        cursor.set_key(1)
        self.assertEqual(cursor.remove(), 0)
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(5))

        evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        evict_cursor.set_key(1)
        if self.valueformat == '8t':
            self.assertEqual(evict_cursor.search(), 0)
        else:
            self.assertEqual(evict_cursor.search(), wiredtiger.WT_NOTFOUND)
        evict_cursor.reset()
        self.session.rollback_transaction()

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.open_version_cursor(start_ts=5)
        version_cursor.set_key(1)
        self.assertEqual(version_cursor.search(), wiredtiger.WT_NOTFOUND)

    def test_ondisk_with_hs_start_timestamp(self):
        self.create()

        cursor = self.session.open_cursor(self.uri, None)

        # Add a value to the update chain
        self.session.begin_transaction()
        cursor[1] = 0
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        # Update the value
        self.session.begin_transaction()
        cursor[1] = 1
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(5))

        evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        self.assertEqual(evict_cursor[1], 1)
        evict_cursor.reset()
        self.session.rollback_transaction()

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.open_version_cursor(start_ts=1)
        version_cursor.set_key(1)
        self.assertEqual(version_cursor.search(), 0)
        self.assertEqual(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 5, 5, WT_TS_MAX, 0, 3, 0, 0, 1, 1)
        self.assertEqual(version_cursor.next(), 0)
        self.assertEqual(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 1, 1, 5, 5, 3, 0, 0, 2, 0)
        self.assertEqual(version_cursor.next(), wiredtiger.WT_NOTFOUND)

    def test_ondisk_with_hs_start_timestamp_exclusive(self):
        self.create()

        cursor = self.session.open_cursor(self.uri, None)

        # Add a value to the update chain
        self.session.begin_transaction()
        cursor[1] = 0
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        # Update the value
        self.session.begin_transaction()
        cursor[1] = 1
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(5))

        # Update the value again
        self.session.begin_transaction()
        cursor[1] = 2
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(8))

        evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        self.assertEqual(evict_cursor[1], 2)
        evict_cursor.reset()
        self.session.rollback_transaction()

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.open_version_cursor(start_ts=5)
        version_cursor.set_key(1)
        self.assertEqual(version_cursor.search(), 0)
        self.assertEqual(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 8, 8, WT_TS_MAX, 0, 3, 0, 0, 1, 2)
        self.assertEqual(version_cursor.next(), 0)
        self.assertEqual(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 5, 5, 8, 8, 3, 0, 0, 2, 1)
        self.assertEqual(version_cursor.next(), wiredtiger.WT_NOTFOUND)
