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

import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wiredtiger import stat
from wtscenario import make_scenarios

# Eviction must preserve reads after a prepared rollback when older values become obsolete.
@disagg_test_class
class test_layered_prepare12(wttest.WiredTigerTestCase):
    uri = 'layered:test_layered_prepare12'
    scenarios = make_scenarios(gen_disagg_storages(disagg_only=True), [
        ('preserve', dict(preserve_prepared=True)),
        ('no_preserve', dict(preserve_prepared=False)),
    ])

    def base_config(self):
        return 'statistics=(all),precise_checkpoint=true,' + \
            f'preserve_prepared={str(self.preserve_prepared).lower()},'

    def conn_config(self):
        return self.base_config() + 'disaggregated=(role="leader")'

    def test_prepared_rollback_after_prunable_update(self):
        conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + ',create,' +
            self.base_config() + 'disaggregated=(role="follower")')
        session_follow = conn_follow.open_session()
        for session in (self.session, session_follow):
            session.create(self.uri, 'key_format=i,value_format=S')
            cursor = session.open_cursor(self.uri)
            session.begin_transaction()
            cursor[1] = "value1"
            session.commit_transaction(f"commit_timestamp={self.timestamp_str(10)}")
            cursor.close()

        session_evict = conn_follow.open_session("debug=(release_evict_page)")

        def evict_follower(expected):
            cursor = session_evict.open_cursor(self.uri)
            cursor.set_key(1)
            self.assertEqual(cursor.search(), 0)
            self.assertEqual(cursor.get_value(), expected)
            cursor.close()

        # Evict before publishing a checkpoint so the follower retains the first value.
        conn_follow.set_timestamp(f"stable_timestamp={self.timestamp_str(10)}")
        evict_follower("value1")

        for session in (self.session, session_follow):
            cursor = session.open_cursor(self.uri)
            session.begin_transaction()
            cursor[1] = "value2"
            session.commit_transaction(f"commit_timestamp={self.timestamp_str(20)}")

            session.begin_transaction()
            cursor[1] = "prepared"
            session.prepare_transaction(
                f"prepare_timestamp={self.timestamp_str(40)},"
                f"prepared_id={self.prepared_id_str(1)}")
            session.rollback_transaction(f"rollback_timestamp={self.timestamp_str(50)}")
            cursor.close()

        # Publish the committed value without making the prepared rollback obsolete.
        self.conn.set_timestamp(f"stable_timestamp={self.timestamp_str(30)}")
        self.session.checkpoint()
        conn_follow.set_timestamp(
            f"stable_timestamp={self.timestamp_str(30)},"
            f"oldest_timestamp={self.timestamp_str(30)}")
        self.disagg_advance_checkpoint_and_wait(conn_follow)

        restored_before = self.get_stat(
            stat.conn.cache_write_restore_invisible, session=session_follow)
        evict_follower("value2")
        restored_after = self.get_stat(
            stat.conn.cache_write_restore_invisible, session=session_follow)
        if self.preserve_prepared:
            self.assertGreater(restored_after, restored_before)
        else:
            self.assertEqual(restored_after, restored_before)

        cursor = session_follow.open_cursor(self.uri)
        self.assertEqual(cursor[1], "value2")
        cursor.close()
        session_evict.close()
        session_follow.close()
        conn_follow.close()
        self.conn.set_timestamp(f"stable_timestamp={self.timestamp_str(50)}")
