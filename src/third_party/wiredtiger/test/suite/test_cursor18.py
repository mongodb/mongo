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

    scenarios = make_scenarios(types)

    def create(self):
        self.session.create(self.uri, 'key_format={},value_format={}'.format(self.keyformat, self.valueformat))
    
    def verify_value(self, version_cursor, expected_start_ts, expected_start_durable_ts, expected_stop_ts, expected_stop_durable_ts, expected_type, expected_prepare_state, expected_flags, expected_location, expected_value):
        values = version_cursor.get_values()
        # Ignore the transaction ids from the value in the verification
        self.assertEquals(values[1], expected_start_ts)
        self.assertEquals(values[2], expected_start_durable_ts)
        self.assertEquals(values[4], expected_stop_ts)
        self.assertEquals(values[5], expected_stop_durable_ts)
        self.assertEquals(values[6], expected_type)
        self.assertEquals(values[7], expected_prepare_state)
        self.assertEquals(values[8], expected_flags)
        self.assertEquals(values[9], expected_location)
        self.assertEquals(values[10], expected_value)

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
        version_cursor = self.session.open_cursor(self.uri, None, "debug=(dump_version=true)")
        version_cursor.set_key(1)
        self.assertEquals(version_cursor.search(), 0)
        self.assertEquals(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 5, 5, WT_TS_MAX, WT_TS_MAX, 3, 0, 0, 0, 1)
        self.assertEquals(version_cursor.next(), 0)
        self.assertEquals(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 1, 1, 5, 5, 3, 0, 0, 0, 0)
        self.assertEquals(version_cursor.next(), wiredtiger.WT_NOTFOUND)

    def test_ondisk_only(self):
        self.create()

        cursor = self.session.open_cursor(self.uri, None)
        # Add a value to the update chain
        self.session.begin_transaction()
        cursor[1] = 0
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        self.assertEquals(evict_cursor[1], 0)
        evict_cursor.reset()
        self.session.rollback_transaction()

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.session.open_cursor(self.uri, None, "debug=(dump_version=true)")
        version_cursor.set_key(1)
        self.assertEquals(version_cursor.search(), 0)
        self.assertEquals(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 1, 1, WT_TS_MAX, WT_TS_MAX, 3, 0, 0, 1, 0)
        self.assertEquals(version_cursor.next(), wiredtiger.WT_NOTFOUND)

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
        self.assertEquals(cursor.remove(), 0)
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(5))

        evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        evict_cursor.set_key(1)
        if self.valueformat == '8t':
            self.assertEquals(evict_cursor.search(), 0)
        else:
            self.assertEquals(evict_cursor.search(), wiredtiger.WT_NOTFOUND)
        evict_cursor.reset()
        self.session.rollback_transaction()

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.session.open_cursor(self.uri, None, "debug=(dump_version=true)")
        version_cursor.set_key(1)
        self.assertEquals(version_cursor.search(), 0)
        self.assertEquals(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 1, 1, 5, 5, 3, 0, 0, 1, 0)
        self.assertEquals(version_cursor.next(), wiredtiger.WT_NOTFOUND)

    def test_ondisk_with_deletion_on_update_chain(self):
        self.create()

        cursor = self.session.open_cursor(self.uri, None)
        # Add a value to the update chain
        self.session.begin_transaction()
        cursor[1] = 0
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        self.assertEquals(evict_cursor[1], 0)
        evict_cursor.reset()
        self.session.rollback_transaction()

        # Delete the value
        self.session.begin_transaction()
        cursor.set_key(1)
        self.assertEquals(cursor.remove(), 0)
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(5))

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.session.open_cursor(self.uri, None, "debug=(dump_version=true)")
        version_cursor.set_key(1)
        self.assertEquals(version_cursor.search(), 0)
        self.assertEquals(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 1, 1, 5, 5, 3, 0, 0, 1, 0)
        self.assertEquals(version_cursor.next(), wiredtiger.WT_NOTFOUND)

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
        self.assertEquals(evict_cursor[1], 1)
        evict_cursor.reset()
        self.session.rollback_transaction()

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.session.open_cursor(self.uri, None, "debug=(dump_version=true)")
        version_cursor.set_key(1)
        self.assertEquals(version_cursor.search(), 0)
        self.assertEquals(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 5, 5, WT_TS_MAX, WT_TS_MAX, 3, 0, 0, 1, 1)
        self.assertEquals(version_cursor.next(), 0)
        self.assertEquals(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 1, 1, 5, 5, 3, 0, 0, 2, 0)
        self.assertEquals(version_cursor.next(), wiredtiger.WT_NOTFOUND)

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
        self.assertEquals(evict_cursor[1], 1)
        evict_cursor.reset()
        self.session.rollback_transaction()

        # Update the value
        self.session.begin_transaction()
        cursor[1] = 2
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(10))

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.session.open_cursor(self.uri, None, "debug=(dump_version=true)")
        version_cursor.set_key(1)
        self.assertEquals(version_cursor.search(), 0)
        self.assertEquals(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 10, 10, WT_TS_MAX, WT_TS_MAX, 3, 0, 0, 0, 2)
        self.assertEquals(version_cursor.next(), 0)
        self.assertEquals(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 5, 5, 10, 10, 3, 0, 0, 1, 1)
        self.assertEquals(version_cursor.next(), 0)
        self.assertEquals(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 1, 1, 5, 5, 3, 0, 0, 2, 0)
        self.assertEquals(version_cursor.next(), wiredtiger.WT_NOTFOUND)
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
        self.assertEquals(evict_cursor[2], 2)
        evict_cursor.reset()
        self.session.rollback_transaction()

        self.session.begin_transaction()
        cursor[2] = 3
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(20))

        # Ensure that we are able to correctly traverse all versions of this new key.
        self.session.begin_transaction()
        version_cursor.set_key(2)
        self.assertEquals(version_cursor.search(), 0)
        self.assertEquals(version_cursor.get_key(), 2)
        self.verify_value(version_cursor, 20, 20, WT_TS_MAX, WT_TS_MAX, 3, 0, 0, 0, 3)
        self.assertEquals(version_cursor.next(), 0)
        self.assertEquals(version_cursor.get_key(), 2)
        self.verify_value(version_cursor, 15, 15, 20, 20, 3, 0, 0, 1, 2)
        self.assertEquals(version_cursor.next(), 0)
        self.assertEquals(version_cursor.get_key(), 2)
        self.verify_value(version_cursor, 11, 11, 15, 15, 3, 0, 0, 2, 1)
        self.assertEquals(version_cursor.next(), wiredtiger.WT_NOTFOUND)

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
        version_cursor = self.session.open_cursor(self.uri, None, "debug=(dump_version=true)")
        version_cursor.set_key(1)
        self.assertEquals(version_cursor.search(), 0)
        self.assertEquals(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 1, 0, WT_TS_MAX, WT_TS_MAX, 3, 1, 4, 0, 0)
        self.assertEquals(version_cursor.next(), 0)
        self.assertEquals(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 1, 1, 1, 0, 3, 1, 0, 1, 0)
        self.assertEquals(version_cursor.next(), wiredtiger.WT_NOTFOUND)
    
    def test_reuse_version_cursor(self):
        self.create()

        cursor = self.session.open_cursor(self.uri, None)
        # Add a value to the update chain
        self.session.begin_transaction()
        cursor[1] = 0
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        self.assertEquals(evict_cursor[1], 0)
        evict_cursor.reset()
        self.session.rollback_transaction()

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.session.open_cursor(self.uri, None, "debug=(dump_version=true)")
        version_cursor.set_key(1)
        self.assertEquals(version_cursor.search(), 0)
        self.assertEquals(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 1, 1, WT_TS_MAX, WT_TS_MAX, 3, 0, 0, 1, 0)
        self.assertEquals(version_cursor.next(), wiredtiger.WT_NOTFOUND)

        # Repeat after reset
        version_cursor.reset()
        version_cursor.set_key(1)
        self.assertEquals(version_cursor.search(), 0)
        self.assertEquals(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 1, 1, WT_TS_MAX, WT_TS_MAX, 3, 0, 0, 1, 0)
        self.assertEquals(version_cursor.next(), wiredtiger.WT_NOTFOUND)

    def test_prepare_tombstone(self):
        self.create()

        cursor = self.session.open_cursor(self.uri, None)
        # Add a value to the update chain
        self.session.begin_transaction()
        cursor[1] = 0
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        self.assertEquals(evict_cursor[1], 0)
        evict_cursor.reset()
        self.session.rollback_transaction()

        # Delete the value with prepare
        session2 = self.conn.open_session()
        cursor2 = session2.open_cursor(self.uri, None)
        # Add a value to the update chain
        session2.begin_transaction()
        cursor2.set_key(1)
        self.assertEquals(cursor2.remove(), 0)
        session2.prepare_transaction("prepare_timestamp=" + self.timestamp_str(2))

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.session.open_cursor(self.uri, None, "debug=(dump_version=true)")
        version_cursor.set_key(1)
        self.assertEquals(version_cursor.search(), 0)
        self.assertEquals(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 1, 1, 2, 0, 3, 1, 0, 1, 0)
        self.assertEquals(version_cursor.next(), wiredtiger.WT_NOTFOUND)
    
    def test_search_when_positioned(self):
        self.create()

        cursor = self.session.open_cursor(self.uri, None)
        # Add a value to the update chain
        self.session.begin_transaction()
        cursor[1] = 0
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.session.open_cursor(self.uri, None, "debug=(dump_version=true)")
        version_cursor.set_key(1)
        self.assertEquals(version_cursor.search(), 0)
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
        version_cursor = self.session.open_cursor(self.uri, None, "debug=(dump_version=true)")
        version_cursor.set_key(1)
        self.assertEquals(version_cursor.search(), 0)
        self.assertEquals(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 5, 5, WT_TS_MAX, WT_TS_MAX, 3, 0, 0, 0, 1)

        # Update the value
        session2 = self.conn.open_session()
        cursor2 = session2.open_cursor(self.uri, None)
        session2.begin_transaction()
        cursor2[1] = 2
        session2.commit_transaction("commit_timestamp=" + self.timestamp_str(5))

        self.assertEquals(version_cursor.next(), 0)
        self.assertEquals(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 1, 1, 5, 5, 3, 0, 0, 0, 0)
        self.assertEquals(version_cursor.next(), wiredtiger.WT_NOTFOUND)
