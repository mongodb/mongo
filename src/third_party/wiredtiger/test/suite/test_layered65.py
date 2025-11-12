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

import os, os.path, shutil, threading, time, wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios
from wiredtiger import stat

# test_layered65.py
#    Test garbage collection ensures that prepared updates and aborted
#    prepared updates are not removed if the rollback timestamps are newer than
#    the checkpoint timestamp of the stable table.
@disagg_test_class
class test_layered65(wttest.WiredTigerTestCase):
    base_config = 'statistics=(all),precise_checkpoint=true,preserve_prepared=true,'
    conn_config = base_config + 'disaggregated=(role="leader")'
    conn_config_follower = base_config + 'disaggregated=(role="follower")'

    create_session_config = 'key_format=i,value_format=S'

    uri = "layered:test_layered65"

    disagg_storages = gen_disagg_storages('test_layered65', disagg_only = True)
    scenarios = make_scenarios(disagg_storages)

    session_follow = None
    conn_follow = None

    def create_follower(self):
        self.conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + ',create,' +
                                                self.conn_config_follower)
        self.session_follow = self.conn_follow.open_session()

    def test_prepared_insert(self):
        self.create_follower()
        self.session.create(self.uri, self.create_session_config)
        self.session_follow.create(self.uri, self.create_session_config)

        # Insert a committed update.
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri)
        cursor[1] = "value1"
        self.session.commit_transaction(f"commit_timestamp={self.timestamp_str(10)}")

        # Insert the committed update on follower.
        self.session_follow.begin_transaction()
        cursor_follow = self.session_follow.open_cursor(self.uri)
        cursor_follow[1] = "value1"
        self.session_follow.commit_transaction(f"commit_timestamp={self.timestamp_str(10)}")

        # Insert a prepared update.
        self.session.begin_transaction()
        cursor[2] = "value1"
        self.session.prepare_transaction(f"prepare_timestamp={self.timestamp_str(20)},prepared_id={self.prepared_id_str(1)}")

        # Insert a prepared update on follower.
        self.session_follow.begin_transaction()
        cursor_follow[2] = "value1"
        self.session_follow.prepare_transaction(f"prepare_timestamp={self.timestamp_str(20)},prepared_id={self.prepared_id_str(1)}")

        cursor_follow.close()

        # Make the prepred update stable.
        self.conn.set_timestamp(f"stable_timestamp={self.timestamp_str(20)}")
        self.conn_follow.set_timestamp(f"stable_timestamp={self.timestamp_str(20)}")

        session = self.conn.open_session()
        session.checkpoint()

        # Advance the checkpoint on the follower.
        self.disagg_advance_checkpoint(self.conn_follow)

        # Evict the data.
        session_follow2 = self.conn_follow.open_session("debug=(release_evict_page)")
        evict_cursor = session_follow2.open_cursor(self.uri)
        evict_cursor.set_key(1)
        evict_cursor.search()
        evict_cursor.close()

        stat_cursor = session_follow2.open_cursor('statistics:' + self.uri)
        garbage_collected = stat_cursor[stat.dsrc.rec_ingest_garbage_collection_keys][2]
        # Only the committed update can be garbage collected.
        self.assertEqual(garbage_collected, 1)

    def test_prepared_insert_rollback(self):
        self.create_follower()
        self.session.create(self.uri, self.create_session_config)
        self.session_follow.create(self.uri, self.create_session_config)

        # Insert a committed update.
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri)
        cursor[1] = "value1"
        self.session.commit_transaction(f"commit_timestamp={self.timestamp_str(10)}")

        # Insert the committed update on follower.
        self.session_follow.begin_transaction()
        cursor_follow = self.session_follow.open_cursor(self.uri)
        cursor_follow[1] = "value1"
        self.session_follow.commit_transaction(f"commit_timestamp={self.timestamp_str(10)}")

        # Insert a prepared update.
        self.session.begin_transaction()
        cursor[2] = "value1"
        self.session.prepare_transaction(f"prepare_timestamp={self.timestamp_str(20)},prepared_id={self.prepared_id_str(1)}")

        # Rollback the prepared update.
        self.session.rollback_transaction(f"rollback_timestamp={self.timestamp_str(30)}")

        # Insert a prepared update on follower.
        self.session_follow.begin_transaction()
        cursor_follow[2] = "value1"
        self.session_follow.prepare_transaction(f"prepare_timestamp={self.timestamp_str(20)},prepared_id={self.prepared_id_str(1)}")

        # Rollback the prepared update on follower.
        self.session_follow.rollback_transaction(f"rollback_timestamp={self.timestamp_str(30)}")
        cursor_follow.close()

        # Make the prepred update stable.
        self.conn.set_timestamp(f"stable_timestamp={self.timestamp_str(20)}")
        self.conn_follow.set_timestamp(f"stable_timestamp={self.timestamp_str(20)}")

        self.session.checkpoint()

        # Advance the checkpoint on the follower.
        self.disagg_advance_checkpoint(self.conn_follow)

        # Evict the data.
        session_follow2 = self.conn_follow.open_session("debug=(release_evict_page)")
        evict_cursor = session_follow2.open_cursor(self.uri)
        evict_cursor.set_key(1)
        evict_cursor.search()
        evict_cursor.close()

        stat_cursor = self.session_follow.open_cursor('statistics:' + self.uri)
        garbage_collected = stat_cursor[stat.dsrc.rec_ingest_garbage_collection_keys][2]
        # Only the committed update can be garbage collected.
        self.assertEqual(garbage_collected, 1)
        stat_cursor.close()

        # Insert another committed update.
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri)
        cursor[3] = "value1"
        self.session.commit_transaction(f"commit_timestamp={self.timestamp_str(40)}")

        # Insert the committed update on follower.
        self.session_follow.begin_transaction()
        cursor_follow = self.session_follow.open_cursor(self.uri)
        cursor_follow[3] = "value1"
        self.session_follow.commit_transaction(f"commit_timestamp={self.timestamp_str(40)}")

        # Make the rollback stable.
        self.conn.set_timestamp(f"stable_timestamp={self.timestamp_str(30)}")
        self.conn_follow.set_timestamp(f"stable_timestamp={self.timestamp_str(30)}")

        self.session.checkpoint()

        # Advance the checkpoint on the follower.
        self.disagg_advance_checkpoint(self.conn_follow)

        # Evict the data.
        session_follow2 = self.conn_follow.open_session("debug=(release_evict_page)")
        evict_cursor = session_follow2.open_cursor(self.uri)
        evict_cursor.set_key(3)
        evict_cursor.search()
        evict_cursor.close()

        stat_cursor = self.session_follow.open_cursor('statistics:' + self.uri)
        garbage_collected = stat_cursor[stat.dsrc.rec_ingest_garbage_collection_keys][2]
        # The aborted prepared update is garbage collected.
        self.assertEqual(garbage_collected, 2)
        stat_cursor.close()

    def test_prepared_update(self):
        self.create_follower()
        self.session.create(self.uri, self.create_session_config)
        self.session_follow.create(self.uri, self.create_session_config)

        # Insert a committed update.
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri)
        cursor[1] = "value1"
        self.session.commit_transaction(f"commit_timestamp={self.timestamp_str(10)}")

        # Insert the committed update on follower.
        self.session_follow.begin_transaction()
        cursor_follow = self.session_follow.open_cursor(self.uri)
        cursor_follow[1] = "value1"
        self.session_follow.commit_transaction(f"commit_timestamp={self.timestamp_str(10)}")

        # Insert another committed update.
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri)
        cursor[2] = "value1"
        self.session.commit_transaction(f"commit_timestamp={self.timestamp_str(10)}")

        # Insert the committed update on follower.
        self.session_follow.begin_transaction()
        cursor_follow = self.session_follow.open_cursor(self.uri)
        cursor_follow[2] = "value1"
        self.session_follow.commit_transaction(f"commit_timestamp={self.timestamp_str(10)}")

        # Make the prepred update stable.
        self.conn.set_timestamp(f"stable_timestamp={self.timestamp_str(10)}")
        self.conn_follow.set_timestamp(f"stable_timestamp={self.timestamp_str(10)}")

        # Evict the data.
        session_follow2 = self.conn_follow.open_session("debug=(release_evict_page)")
        evict_cursor = session_follow2.open_cursor(self.uri)
        evict_cursor.set_key(1)
        evict_cursor.search()
        evict_cursor.close()

        # Update a key with prepared transaction.
        self.session.begin_transaction()
        cursor[2] = "value1"
        self.session.prepare_transaction(f"prepare_timestamp={self.timestamp_str(20)},prepared_id={self.prepared_id_str(1)}")

        # Update the same key on follower.
        self.session_follow.begin_transaction()
        cursor_follow[2] = "value1"
        self.session_follow.prepare_transaction(f"prepare_timestamp={self.timestamp_str(20)},prepared_id={self.prepared_id_str(1)}")

        cursor_follow.close()

        # Make the prepred update stable.
        self.conn.set_timestamp(f"stable_timestamp={self.timestamp_str(20)}")
        self.conn_follow.set_timestamp(f"stable_timestamp={self.timestamp_str(20)}")

        session = self.conn.open_session()
        session.checkpoint()

        # Advance the checkpoint on the follower.
        self.disagg_advance_checkpoint(self.conn_follow)

        # Evict the data.
        session_follow2 = self.conn_follow.open_session("debug=(release_evict_page)")
        evict_cursor = session_follow2.open_cursor(self.uri)
        evict_cursor.set_key(1)
        evict_cursor.search()
        evict_cursor.close()

        stat_cursor = session_follow2.open_cursor('statistics:' + self.uri)
        garbage_collected = stat_cursor[stat.dsrc.rec_ingest_garbage_collection_keys][2]
        # Only the non-prepared key can be garbage collected.
        self.assertEqual(garbage_collected, 1)

    def test_prepared_update_rollback(self):
        self.create_follower()
        self.session.create(self.uri, self.create_session_config)
        self.session_follow.create(self.uri, self.create_session_config)

        # Insert a committed update.
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri)
        cursor[1] = "value1"
        self.session.commit_transaction(f"commit_timestamp={self.timestamp_str(10)}")

        # Insert the committed update on follower.
        self.session_follow.begin_transaction()
        cursor_follow = self.session_follow.open_cursor(self.uri)
        cursor_follow[1] = "value1"
        self.session_follow.commit_transaction(f"commit_timestamp={self.timestamp_str(10)}")

        # Insert another committed update.
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri)
        cursor[2] = "value1"
        self.session.commit_transaction(f"commit_timestamp={self.timestamp_str(10)}")

        # Insert the committed update on follower.
        self.session_follow.begin_transaction()
        cursor_follow = self.session_follow.open_cursor(self.uri)
        cursor_follow[2] = "value1"
        self.session_follow.commit_transaction(f"commit_timestamp={self.timestamp_str(10)}")

        # Make the prepred update stable.
        self.conn.set_timestamp(f"stable_timestamp={self.timestamp_str(10)}")
        self.conn_follow.set_timestamp(f"stable_timestamp={self.timestamp_str(10)}")

        # Evict the data.
        session_follow2 = self.conn_follow.open_session("debug=(release_evict_page)")
        evict_cursor = session_follow2.open_cursor(self.uri)
        evict_cursor.set_key(1)
        evict_cursor.search()
        evict_cursor.close()

        # Update a key with prepared transaction.
        self.session.begin_transaction()
        cursor[2] = "value1"
        self.session.prepare_transaction(f"prepare_timestamp={self.timestamp_str(20)},prepared_id={self.prepared_id_str(1)}")

        # Rollback the prepared update.
        self.session.rollback_transaction(f"rollback_timestamp={self.timestamp_str(30)}")

        # Update the same key on follower.
        self.session_follow.begin_transaction()
        cursor_follow[2] = "value1"
        self.session_follow.prepare_transaction(f"prepare_timestamp={self.timestamp_str(20)},prepared_id={self.prepared_id_str(1)}")

        # Rollback the prepared update on follower.
        self.session_follow.rollback_transaction(f"rollback_timestamp={self.timestamp_str(30)}")
        cursor_follow.close()

        # Make the prepred update stable.
        self.conn.set_timestamp(f"stable_timestamp={self.timestamp_str(20)}")
        self.conn_follow.set_timestamp(f"stable_timestamp={self.timestamp_str(20)}")

        self.session.checkpoint()

        # Advance the checkpoint on the follower.
        self.disagg_advance_checkpoint(self.conn_follow)

        # Evict the data.
        session_follow2 = self.conn_follow.open_session("debug=(release_evict_page)")
        evict_cursor = session_follow2.open_cursor(self.uri)
        evict_cursor.set_key(1)
        evict_cursor.search()
        evict_cursor.close()

        stat_cursor = self.session_follow.open_cursor('statistics:' + self.uri)
        garbage_collected = stat_cursor[stat.dsrc.rec_ingest_garbage_collection_keys][2]
        # Only the committed update can be garbage collected.
        self.assertEqual(garbage_collected, 1)
        stat_cursor.close()

        # FIXME-WT-15489: enable this after we stop writing prepared update to disk for in-memory btrees

        # Insert another committed update.
        # self.session.begin_transaction()
        # cursor = self.session.open_cursor(self.uri)
        # cursor[3] = "value1"
        # self.session.commit_transaction(f"commit_timestamp={self.timestamp_str(40)}")

        # Insert the committed update on follower.
        # self.session_follow.begin_transaction()
        # cursor_follow = self.session_follow.open_cursor(self.uri)
        # cursor_follow[3] = "value1"
        # self.session_follow.commit_transaction(f"commit_timestamp={self.timestamp_str(40)}")

        # Make the rollback stable.
        # self.conn.set_timestamp(f"stable_timestamp={self.timestamp_str(30)}")
        # self.conn_follow.set_timestamp(f"stable_timestamp={self.timestamp_str(30)}")

        # self.session.checkpoint()

        # Advance the checkpoint on the follower.
        # self.disagg_advance_checkpoint(self.conn_follow)

        # Evict the data.
        # session_follow2 = self.conn_follow.open_session("debug=(release_evict_page)")
        # evict_cursor = session_follow2.open_cursor(self.uri)
        # evict_cursor.set_key(3)
        # evict_cursor.search()
        # evict_cursor.close()

        # stat_cursor = self.session_follow.open_cursor('statistics:' + self.uri)
        # garbage_collected = stat_cursor[stat.dsrc.rec_ingest_garbage_collection_keys][2]
        # # The aborted prepared update is garbage collected.
        # self.assertEqual(garbage_collected, 2)
        # stat_cursor.close()
