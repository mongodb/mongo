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

# test_layered59.py
#    Test never build an internal page delta if the first key is modified.
@disagg_test_class
class test_layered59(wttest.WiredTigerTestCase, DisaggConfigMixin):
    disagg_storages = gen_disagg_storages('test_layered59', disagg_only = True)
    scenarios = make_scenarios(disagg_storages)

    conn_config = 'disaggregated=(page_log=palm),page_delta=(delta_pct=100),disaggregated=(role="leader")'

    nitems = 1000

    def test_single_update(self):
        uri = "layered:test_layered59"

        # Setup.
        self.session.create(uri, 'key_format=S,value_format=S')

        # Insert some data.
        cursor = self.session.open_cursor(uri, None, None)
        for i in range(1, self.nitems):
            self.session.begin_transaction()
            cursor[str(i)] = "value " + str(i) * 1000
            self.session.commit_transaction(f"commit_timestamp={self.timestamp_str(10)}")

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))

        # Do a checkpoint
        self.session.checkpoint()

        self.reopen_conn()

        # Update a key at the start
        cursor = self.session.open_cursor(uri, None, None)
        self.session.begin_transaction()
        cursor[str(1)] = "value " + str(i) * 1000
        self.session.commit_transaction(f"commit_timestamp={self.timestamp_str(20)}")

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))

        # Do another checkpoint
        self.session.checkpoint()

        # Verfiy that we haven't written any internal page delta
        stat_cursor = self.session.open_cursor('statistics:' + uri)
        self.assertEqual(stat_cursor[stat.dsrc.rec_page_delta_internal][2], 0)
        stat_cursor.close()

    def test_inserts_to_split(self):
        uri = "layered:test_layered59"

        # Setup.
        self.session.create(uri, 'key_format=S,value_format=S')

        # Insert some data.
        cursor = self.session.open_cursor(uri, None, None)
        for i in range(self.nitems, 2 * self.nitems):
            self.session.begin_transaction()
            cursor[str(i)] = "value " + str(i) * 100
            self.session.commit_transaction(f"commit_timestamp={self.timestamp_str(10)}")

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))

        # Do a checkpoint
        self.session.checkpoint()

        self.reopen_conn()

        # Insert a lot of keys at start to split the first leaf page.
        cursor = self.session.open_cursor(uri, None, None)
        for i in range(1, self.nitems):
            self.session.begin_transaction()
            cursor[str(i)] = "value " + str(i) * 100
            self.session.commit_transaction(f"commit_timestamp={self.timestamp_str(20)}")

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))

        # Do another checkpoint
        self.session.checkpoint()

        # Verfiy that we haven't written any internal page delta
        stat_cursor = self.session.open_cursor('statistics:' + uri)
        self.assertEqual(stat_cursor[stat.dsrc.rec_page_delta_internal][2], 0)
        stat_cursor.close()

    def test_deletes(self):
        uri = "layered:test_layered59"

        # Setup.
        self.session.create(uri, 'key_format=S,value_format=S')

        # Insert some data.
        cursor = self.session.open_cursor(uri, None, None)
        for i in range(self.nitems, 2 * self.nitems):
            self.session.begin_transaction()
            cursor[str(i)] = "value " + str(i) * 100
            self.session.commit_transaction(f"commit_timestamp={self.timestamp_str(10)}")

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))

        # Do a checkpoint
        self.session.checkpoint()

        self.reopen_conn()

        # Delete half of the keys from the start.
        cursor = self.session.open_cursor(uri, None, None)
        for i in range(1, self.nitems // 2):
            self.session.begin_transaction()
            cursor.set_key(str(i))
            cursor.remove()
            self.session.commit_transaction(f"commit_timestamp={self.timestamp_str(20)}")
        cursor.close()

        # Make the delete globally visible.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30) + ',oldest_timestamp=' + self.timestamp_str(20))

        # Do another checkpoint
        self.session.checkpoint()

        # Verfiy that we haven't written any internal page delta
        stat_cursor = self.session.open_cursor('statistics:' + uri)
        self.assertEqual(stat_cursor[stat.dsrc.rec_page_delta_internal][2], 0)
        stat_cursor.close()
