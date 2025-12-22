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

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from prepare_util import test_prepare_preserve_prepare_base
from wtscenario import make_scenarios

# Test reconciliation with prepared rollback. Ensure the old value is included
# in the write after the prepared update is rolled back.
@disagg_test_class
class test_layered69(test_prepare_preserve_prepare_base):
    conn_config_base = test_prepare_preserve_prepare_base.conn_config + ',disaggregated=(role="leader")'

    uri = "table:test_layered69"

    evict = [
        ('none', dict(evict=False)),
        ('evict', dict(evict=True)),
    ]

    delta = [
        ('disabled', dict(delta=False)),
        ('enabled', dict(evict=True)),
    ]

    disagg_storages = gen_disagg_storages('test_layered69', disagg_only = True)
    scenarios = make_scenarios(disagg_storages, evict, delta)

    def conn_config(self):
        if self.delta:
            return self.conn_config_base
        else:
            return self.conn_config_base + ',page_delta=(internal_page_delta=false,leaf_page_delta=false)'

    def test_rollback_prepared_update(self):
        if self.delta:
            stat = wiredtiger.stat.dsrc.rec_page_delta_leaf
        else:
            stat = wiredtiger.stat.dsrc.rec_page_full_image_leaf

        # Setup: Initialize the stable timestamp
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(20)}')

        create_params = 'key_format=i,value_format=S,type=layered'
        self.session.create(self.uri, create_params)

        # Insert a value and commit for keys 1-19
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(1, 20):
            cursor.set_key(i)
            cursor.set_value('commit_value')
            cursor.insert()
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(21)}')

        # Make the updates stable
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(21)}')

        # Verify checkpoint writes no prepared to disk
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: False,
            stat: not self.delta,
        }, self.uri)

        if self.evict:
            # Force the page to be evicted
            evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
            self.session.begin_transaction()
            for i in range(1, 19):
                evict_cursor.set_key(i)
                self.assertEqual(evict_cursor.search(), 0)
                evict_cursor.reset()
            self.session.rollback_transaction()
            evict_cursor.close()

        # Update key 19 with a prepared update prepared_id=1
        session_prepare = self.conn.open_session()
        cursor_prepare = session_prepare.open_cursor(self.uri)
        session_prepare.begin_transaction()
        cursor_prepare.set_key(19)
        cursor_prepare.set_value('prepared_value_20_3')
        cursor_prepare.insert()
        session_prepare.prepare_transaction(f'prepare_timestamp={self.timestamp_str(35)},prepared_id={self.prepared_id_str(1)}')

        # Rollback the prepared transaction
        session_prepare.rollback_transaction(f'rollback_timestamp={self.timestamp_str(45)}')
        session_prepare.close()

        # Verify checkpoint skips writing a page to disk
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: False,
            stat: False,
        }, self.uri)

        # Make stable timestamp equal to prepare timestamp - this should allow checkpoint to reconcile prepared update
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(35)}')

        # Verify checkpoint writes a page with prepared time window to disk
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: True,
            stat: True,
        }, self.uri)

        # Make stable timestamp equal to rollback timestamp
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(45)}')

        # Verify checkpoint writes a page with the committed update to disk
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: False,
            stat: True,
        }, self.uri)

        # Verify the key
        self.assertEqual(cursor[19], 'commit_value')

    def test_rollback_prepared_reinsert(self):
        if self.delta:
            stat = wiredtiger.stat.dsrc.rec_page_delta_leaf
        else:
            stat = wiredtiger.stat.dsrc.rec_page_full_image_leaf

        # Setup: Initialize the stable timestamp
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(20)}')

        create_params = 'key_format=i,value_format=S,type=layered'
        self.session.create(self.uri, create_params)

        # Insert a value and commit for keys 1-19
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(1, 20):
            cursor.set_key(i)
            cursor.set_value('commit_value')
            cursor.insert()
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(21)}')

        # Delete key 19
        self.session.begin_transaction()
        cursor.set_key(19)
        cursor.remove()
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(22)}')

        # Make the delete globally visible
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(30)},oldest_timestamp={self.timestamp_str(22)}')

        # Verify checkpoint writes no prepared to disk
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: False,
            stat: not self.delta,
        }, self.uri)

        if self.evict:
            # Force the page to be evicted
            evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
            self.session.begin_transaction()
            for i in range(1, 19):
                evict_cursor.set_key(i)
                self.assertEqual(evict_cursor.search(), 0)
                evict_cursor.reset()
            self.session.rollback_transaction()
            evict_cursor.close()

        # Update key 19 with a prepared update prepared_id=1
        session_prepare = self.conn.open_session()
        cursor_prepare = session_prepare.open_cursor(self.uri)
        session_prepare.begin_transaction()
        cursor_prepare.set_key(19)
        cursor_prepare.set_value('prepared_value_20_3')
        cursor_prepare.insert()
        session_prepare.prepare_transaction(f'prepare_timestamp={self.timestamp_str(35)},prepared_id={self.prepared_id_str(1)}')

        # Rollback the prepared transaction
        session_prepare.rollback_transaction(f'rollback_timestamp={self.timestamp_str(45)}')
        session_prepare.close()

        # Verify checkpoint skips writing a page to disk
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: False,
            stat: False,
        }, self.uri)

        # Make stable timestamp equal to prepare timestamp - this should allow checkpoint to reconcile prepared update
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(35)}')

        # Verify checkpoint writes a page with prepared time window to disk
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: True,
            stat: True,
        }, self.uri)

        # Make stable timestamp equal to rollback timestamp
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(45)}')

        # Verify checkpoint writes a page with the committed update to disk
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: False,
            stat: True,
        }, self.uri)

        # Verify the key
        cursor.set_key(19)
        self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)

    def test_rollback_prepared_remove(self):
        if self.delta:
            stat = wiredtiger.stat.dsrc.rec_page_delta_leaf
        else:
            stat = wiredtiger.stat.dsrc.rec_page_full_image_leaf

        # Setup: Initialize the stable timestamp
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(20)}')

        create_params = 'key_format=i,value_format=S,type=layered'
        self.session.create(self.uri, create_params)

        # Insert a value and commit for keys 1-19
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(1, 20):
            cursor.set_key(i)
            cursor.set_value('commit_value')
            cursor.insert()
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(21)}')

        # Make the updates stable
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(21)}')

        # Verify checkpoint writes no prepared to disk
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: False,
            stat: not self.delta,
        }, self.uri)

        if self.evict:
            # Force the page to be evicted
            evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
            self.session.begin_transaction()
            for i in range(1, 19):
                evict_cursor.set_key(i)
                self.assertEqual(evict_cursor.search(), 0)
                evict_cursor.reset()
            self.session.rollback_transaction()
            evict_cursor.close()

        # Delete key 19 with a prepared update prepared_id=1
        session_prepare = self.conn.open_session()
        cursor_prepare = session_prepare.open_cursor(self.uri)
        session_prepare.begin_transaction()
        cursor_prepare.set_key(19)
        cursor_prepare.remove()
        session_prepare.prepare_transaction(f'prepare_timestamp={self.timestamp_str(35)},prepared_id={self.prepared_id_str(1)}')

        # Rollback the prepared transaction
        session_prepare.rollback_transaction(f'rollback_timestamp={self.timestamp_str(45)}')
        session_prepare.close()

        # Verify checkpoint skips writing a page to disk
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: False,
            stat: False,
        }, self.uri)

        # Make stable timestamp equal to prepare timestamp - this should allow checkpoint to reconcile prepared update
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(35)}')

        # Verify checkpoint writes a page with prepared time window to disk
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: True,
            stat: True,
        }, self.uri)

        # Make stable timestamp equal to rollback timestamp
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(45)}')

        # Verify checkpoint writes a page with the committed update to disk
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: False,
            stat: True,
        }, self.uri)

        # Verify the key
        self.assertEqual(cursor[19], 'commit_value')
