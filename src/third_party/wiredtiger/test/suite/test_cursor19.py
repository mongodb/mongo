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
# test_cursor19.py
#   Test version cursor for modifies.
#
import wttest
import wiredtiger
from wtscenario import make_scenarios

WT_TS_MAX = 2**64-1

class test_cursor19(wttest.WiredTigerTestCase):
    uri = 'file:test_cursor19.wt'

    types = [
        ('row', dict(keyformat='i')),
        ('var', dict(keyformat='r'))
    ]

    scenarios = make_scenarios(types)

    def create(self):
        self.session.create(self.uri, 'key_format={},value_format=S'.format(self.keyformat))
    
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

    def test_modify(self):
        self.create()

        value1 = "a" * 100
        value2 = "b" + "a" * 99
        value3 = "c" + "a" * 99
        value4 = "d" + "a" * 99
        value5 = "e" + "a" * 99
        value6 = "f" + "a" * 99
        cursor = self.session.open_cursor(self.uri, None)
        # Add a value to the update chain
        self.session.begin_transaction()
        cursor[1] = value1
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        # Modify the value
        self.session.begin_transaction()
        cursor.set_key(1)
        mods = [wiredtiger.Modify("b", 0, 1)]
        self.assertEquals(cursor.modify(mods), 0)
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(5))

        # Modify the value
        self.session.begin_transaction()
        cursor.set_key(1)
        mods = [wiredtiger.Modify("c", 0, 1)]
        self.assertEquals(cursor.modify(mods), 0)
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(10))

        # Modify the value
        self.session.begin_transaction()
        cursor.set_key(1)
        mods = [wiredtiger.Modify("d", 0, 1)]
        self.assertEquals(cursor.modify(mods), 0)
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(15))

        cursor.reset()

        evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        evict_cursor.set_key(1)
        self.assertEquals(evict_cursor.search(), 0)
        evict_cursor.reset()
        self.session.rollback_transaction()

        # Modify the value
        self.session.begin_transaction()
        cursor.set_key(1)
        mods = [wiredtiger.Modify("e", 0, 1)]
        self.assertEquals(cursor.modify(mods), 0)
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(20))

        # Modify the value
        self.session.begin_transaction()
        cursor.set_key(1)
        mods = [wiredtiger.Modify("f", 0, 1)]
        self.assertEquals(cursor.modify(mods), 0)
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(25))

        # Delete the key
        self.session.begin_transaction()
        cursor.set_key(1)
        self.assertEquals(cursor.remove(), 0)
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(30))

        # Open a version cursor
        self.session.begin_transaction()
        version_cursor = self.session.open_cursor(self.uri, None, "debug=(dump_version=true)")
        version_cursor.set_key(1)
        self.assertEquals(version_cursor.search(), 0)
        self.assertEquals(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 25, 25, 30, 30, 1, 0, 0, 0, value6)
        self.assertEquals(version_cursor.next(), 0)
        self.assertEquals(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 20, 20, 25, 25, 1, 0, 0, 0, value5)
        self.assertEquals(version_cursor.next(), 0)
        self.assertEquals(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 15, 15, 20, 20, 3, 0, 0, 1, value4)
        self.assertEquals(version_cursor.next(), 0)
        self.assertEquals(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 10, 10, 15, 15, 3, 0, 0, 2, value3)
        self.assertEquals(version_cursor.next(), 0)
        self.assertEquals(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 5, 5, 10, 10, 1, 0, 0, 2, value2)
        self.assertEquals(version_cursor.next(), 0)
        self.assertEquals(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 1, 1, 5, 5, 1, 0, 0, 2, value1)
        self.assertEquals(version_cursor.next(), wiredtiger.WT_NOTFOUND)
