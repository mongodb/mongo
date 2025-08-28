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

import wttest
import os
from wiredtiger import stat
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered45.py
# Entires have been durable are not included in the new delta

@disagg_test_class
class test_layered45(wttest.WiredTigerTestCase, DisaggConfigMixin):
    uri = "layered:test_layered45"
    conn_base_config = 'statistics=(all),statistics_log=(wait=1,json=true,on_close=true),transaction_sync=(enabled,method=fsync),' \
                     + 'page_delta=(delta_pct=50),disaggregated=(page_log=palm),precise_checkpoint=true,preserve_prepared=true,'
    #conn_config = conn_base_config + 'disaggregated=(role="leader")'
    disagg_storages = gen_disagg_storages('test_layered45', disagg_only = True)

    # Make scenarios for different cloud service providers
    scenarios = make_scenarios(disagg_storages)

    nitems = 10

    def session_create_config(self):
        # The delta percentage of 100 is an arbitrary large value, intended to produce
        # deltas a lot of the time.
        return 'key_format=S,value_format=S'

    def conn_config(self):
        return self.conn_base_config + 'disaggregated=(role="leader")'

    # Load the storage store extension.
    def conn_extensions(self, extlist):
        if os.name == 'nt':
            extlist.skip_if_missing = True
        extlist.extension('page_log', 'palm')

    def test_normal_update(self):
        self.session.create(self.uri, self.session_create_config())

        cursor = self.session.open_cursor(self.uri, None, None)
        value1 = "a"
        value2 = "b"

        for i in range(self.nitems):
            self.session.begin_transaction()
            cursor[str(i)] = value1
            self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(5)}')

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(5))

        self.session.checkpoint()

        self.session.begin_transaction()
        cursor[str(5)] = value2
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(10)}')

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))

        # We should build a delta
        self.session.checkpoint()

        stat_cursor = self.session.open_cursor('statistics:' + self.uri)
        self.assertEqual(stat_cursor[stat.dsrc.rec_page_delta_leaf][2], 1)
        stat_cursor.close()

        session2 = self.conn.open_session()
        # Do an uncommitted update
        session2.begin_transaction()
        cursor2 = session2.open_cursor(self.uri, None, None)
        cursor2[str(2)] = value1

        # We should build an empty delta
        self.session.checkpoint()

        stat_cursor = self.session.open_cursor('statistics:' + self.uri)
        self.assertEqual(stat_cursor[stat.dsrc.rec_page_delta_leaf][2], 1)
        stat_cursor.close()

    def test_delete(self):
        self.session.create(self.uri, self.session_create_config())

        cursor = self.session.open_cursor(self.uri, None, None)
        value1 = "a"

        for i in range(self.nitems):
            self.session.begin_transaction()
            cursor[str(i)] = value1
            self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(5)}')

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(5))

        self.session.checkpoint()

        self.session.begin_transaction()
        cursor.set_key(str(5))
        cursor.remove()
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(10)}')

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))

        # We should build a delta
        self.session.checkpoint()
        stat_cursor = self.session.open_cursor('statistics:' + self.uri)
        self.assertEqual(stat_cursor[stat.dsrc.rec_page_delta_leaf][2], 1)
        stat_cursor.close()

        session2 = self.conn.open_session()
        # Do an uncommitted update
        session2.begin_transaction()
        cursor2 = session2.open_cursor(self.uri, None, None)
        cursor2[str(2)] = value1

        # We should build an empty delta
        self.session.checkpoint()

        stat_cursor = self.session.open_cursor('statistics:' + self.uri)
        self.assertEqual(stat_cursor[stat.dsrc.rec_page_delta_leaf][2], 1)
        stat_cursor.close()

        session2.rollback_transaction()

        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(20)},oldest_timestamp={self.timestamp_str(20)}')

        # We should build a delta with delete
        self.session.checkpoint()

        stat_cursor = self.session.open_cursor('statistics:' + self.uri)
        self.assertEqual(stat_cursor[stat.dsrc.rec_page_delta_leaf][2], 2)
        stat_cursor.close()

        session2 = self.conn.open_session()
        # Do an uncommitted update
        session2.begin_transaction()
        cursor2 = session2.open_cursor(self.uri, None, None)
        cursor2[str(2)] = value1

        # We should build an empty delta
        self.session.checkpoint()

        stat_cursor = self.session.open_cursor('statistics:' + self.uri)
        self.assertEqual(stat_cursor[stat.dsrc.rec_page_delta_leaf][2], 2)
        stat_cursor.close()

    def test_prepare_update(self):
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.session.create(self.uri, self.session_create_config())

        cursor = self.session.open_cursor(self.uri, None, None)
        value1 = "a"
        value2 = "b"

        for i in range(self.nitems):
            self.session.begin_transaction()
            cursor[str(i)] = value1
            self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(5)}')

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(5))

        self.session.checkpoint()

        self.session.begin_transaction()
        cursor[str(5)] = value2
        self.session.prepare_transaction(f'prepare_timestamp={self.timestamp_str(10)},prepared_id=' + self.prepared_id_str(1))
        cursor.reset()

        session2 = self.conn.open_session()

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))

        # We should build a delta
        session2.checkpoint()

        stat_cursor = session2.open_cursor('statistics:' + self.uri)
        self.assertEqual(stat_cursor[stat.dsrc.rec_page_delta_leaf][2], 1)
        stat_cursor.close()

        session3 = self.conn.open_session()
        # Do an uncommitted update
        session3.begin_transaction()
        cursor2 = session3.open_cursor(self.uri, None, None)
        cursor2[str(2)] = value1
        cursor2.reset()

        # We should build an empty delta
        session2.checkpoint()

        stat_cursor = session2.open_cursor('statistics:' + self.uri)
        self.assertEqual(stat_cursor[stat.dsrc.rec_page_delta_leaf][2], 1)
        stat_cursor.close()

        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(20)},durable_timestamp={self.timestamp_str(30)}')
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))

        # We should build a delta
        session2.checkpoint()

        stat_cursor = session2.open_cursor('statistics:' + self.uri)
        self.assertEqual(stat_cursor[stat.dsrc.rec_page_delta_leaf][2], 2)
        stat_cursor.close()

        # We should build an empty delta
        session2.checkpoint()

        stat_cursor = session2.open_cursor('statistics:' + self.uri)
        self.assertEqual(stat_cursor[stat.dsrc.rec_page_delta_leaf][2], 2)
        stat_cursor.close()

    def test_prepare_delete(self):
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.session.create(self.uri, self.session_create_config())

        cursor = self.session.open_cursor(self.uri, None, None)
        value1 = "a"
        value2 = "b"

        for i in range(self.nitems):
            self.session.begin_transaction()
            cursor[str(i)] = value1
            self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(5)}')

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(5))

        self.session.checkpoint()

        self.session.begin_transaction()
        cursor.set_key(str(5))
        cursor.remove()
        self.session.prepare_transaction(f'prepare_timestamp={self.timestamp_str(10)},prepared_id=' + self.prepared_id_str(1))
        cursor.reset()

        session2 = self.conn.open_session()

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))

        # We should build a delta
        session2.checkpoint()

        stat_cursor = session2.open_cursor('statistics:' + self.uri)
        self.assertEqual(stat_cursor[stat.dsrc.rec_page_delta_leaf][2], 1)
        stat_cursor.close()

        session3 = self.conn.open_session()
        # Do an uncommitted update
        session3.begin_transaction()
        cursor2 = session3.open_cursor(self.uri, None, None)
        cursor2[str(2)] = value1
        cursor2.reset()

        # We should build an empty delta
        session2.checkpoint()

        stat_cursor = session2.open_cursor('statistics:' + self.uri)
        self.assertEqual(stat_cursor[stat.dsrc.rec_page_delta_leaf][2], 1)
        stat_cursor.close()

        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(20)},durable_timestamp={self.timestamp_str(30)}')
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))

        # We should build a delta
        session2.checkpoint()

        stat_cursor = session2.open_cursor('statistics:' + self.uri)
        self.assertEqual(stat_cursor[stat.dsrc.rec_page_delta_leaf][2], 2)
        stat_cursor.close()

        # We should build an empty delta
        session2.checkpoint()

        stat_cursor = session2.open_cursor('statistics:' + self.uri)
        self.assertEqual(stat_cursor[stat.dsrc.rec_page_delta_leaf][2], 2)
        stat_cursor.close()

    def test_prepare_update_delete(self):
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.session.create(self.uri, self.session_create_config())

        cursor = self.session.open_cursor(self.uri, None, None)
        value1 = "a"
        value2 = "b"

        for i in range(self.nitems):
            self.session.begin_transaction()
            cursor[str(i)] = value1
            self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(5)}')

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(5))

        self.session.checkpoint()

        self.session.begin_transaction()
        cursor[str(5)] = value2
        cursor.set_key(str(5))
        cursor.remove()
        self.session.prepare_transaction(f'prepare_timestamp={self.timestamp_str(10)},prepared_id=' + self.prepared_id_str(1))
        cursor.reset()

        session2 = self.conn.open_session()

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))

        # We should build a delta
        session2.checkpoint()

        stat_cursor = session2.open_cursor('statistics:' + self.uri)
        self.assertEqual(stat_cursor[stat.dsrc.rec_page_delta_leaf][2], 1)
        stat_cursor.close()

        session3 = self.conn.open_session()
        # Do an uncommitted update
        session3.begin_transaction()
        cursor2 = session3.open_cursor(self.uri, None, None)
        cursor2[str(2)] = value1
        cursor2.reset()

        # We should build an empty delta
        session2.checkpoint()

        stat_cursor = session2.open_cursor('statistics:' + self.uri)
        self.assertEqual(stat_cursor[stat.dsrc.rec_page_delta_leaf][2], 1)
        stat_cursor.close()

        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(20)},durable_timestamp={self.timestamp_str(30)}')
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))

        # We should build a delta
        session2.checkpoint()

        stat_cursor = session2.open_cursor('statistics:' + self.uri)
        self.assertEqual(stat_cursor[stat.dsrc.rec_page_delta_leaf][2], 2)
        stat_cursor.close()

        # We should build an empty delta
        session2.checkpoint()

        stat_cursor = session2.open_cursor('statistics:' + self.uri)
        self.assertEqual(stat_cursor[stat.dsrc.rec_page_delta_leaf][2], 2)
        stat_cursor.close()
