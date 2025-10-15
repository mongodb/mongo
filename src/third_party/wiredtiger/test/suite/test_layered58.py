#!/usr/bin/env python3
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

import random, string, wttest
from wiredtiger import stat
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered58.py
#    Test cursor walk with delta.
@disagg_test_class
class test_layered58(wttest.WiredTigerTestCase, DisaggConfigMixin):
    disagg_storages = gen_disagg_storages('test_layered58', disagg_only = True)
    scenarios = make_scenarios(disagg_storages)

    conn_config = 'disaggregated=(page_log=palm),cache_size=10MB,statistics=(all),disaggregated=(role="leader")'

    nitems = 100

    def test_cursor_walk_with_delta(self):
        uri = "layered:test_layered58"

        # Setup.
        self.session.create(uri, 'key_format=S,value_format=S')

        # Insert some data.
        cursor = self.session.open_cursor(uri, None, None)
        for i in range(1, self.nitems):
            self.session.begin_transaction()
            cursor[str(i)] = "value"
            if i == 50:
                self.session.commit_transaction(f"commit_timestamp={self.timestamp_str(20)}")
            else:
                self.session.commit_transaction(f"commit_timestamp={self.timestamp_str(10)}")

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))

        # Do a checkpoint
        self.session.checkpoint()

        # Update a key in the middle
        self.session.begin_transaction()
        cursor[str(50)] = "value2"
        self.session.commit_transaction(f"commit_timestamp={self.timestamp_str(30)}")

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))

        # Do another checkpoint
        self.session.checkpoint()

        # Verfiy that we have written a delta
        stat_cursor = self.session.open_cursor('statistics:' + uri)
        self.assertGreater(stat_cursor[stat.dsrc.rec_page_delta_leaf][2], 0)
        stat_cursor.close()

        self.reopen_conn()

        cursor = self.session.open_cursor(uri, None, None)
        # Read with the latest timestamp
        self.session.begin_transaction("read_timestamp=" + self.timestamp_str(30))
        count = 0
        while cursor.next() == 0:
            if cursor.get_key() == str(50):
                self.assertEqual(cursor.get_value(), "value2")
            else:
                self.assertEqual(cursor.get_value(), "value")
            count += 1
        self.assertEqual(count, self.nitems - 1)

        count = 0
        while cursor.prev() == 0:
            if cursor.get_key() == str(50):
                self.assertEqual(cursor.get_value(), "value2")
            else:
                self.assertEqual(cursor.get_value(), "value")
            count += 1
        self.assertEqual(count, self.nitems - 1)
        self.session.rollback_transaction()

        # Read with timestamp 10
        self.session.begin_transaction("read_timestamp=" + self.timestamp_str(10))
        count = 0
        while cursor.next() == 0:
            self.assertEqual(cursor.get_value(), "value")
            count += 1
        self.assertEqual(count, self.nitems - 2)

        count = 0
        while cursor.prev() == 0:
            self.assertEqual(cursor.get_value(), "value")
            count += 1
        self.assertEqual(count, self.nitems - 2)
        self.session.rollback_transaction()
