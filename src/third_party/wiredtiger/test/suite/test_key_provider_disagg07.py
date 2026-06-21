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

from helper_disagg import disagg_test_class, gen_disagg_storages
from helper_key_provider import KeyProviderBase
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# Leader/follower push-mode key provider scenarios:
# - follower's queued key persists after step-up
# - timestamps stay monotonic across role switches
# - follower prunes its queue on pickup
# - a queued rotation completes once stable reaches it
@disagg_test_class
class test_key_provider_disagg07(KeyProviderBase):
    test_name = __qualname__
    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    uri = f"layered:{test_name}"
    table_config = 'key_format=S,value_format=S'
    follower_conn = None

    def populate_table(self):
        # Give checkpoints real work to flush.
        self.ds = SimpleDataSet(self, self.uri, 10)
        self.ds.populate()
        self.row = 100

    def setup_follower(self):
        self.follower_dir = 'follower'
        config = self.extensionsConfig() + ',create,disaggregated=(role="follower")'
        self.follower_conn = self.wiredtiger_open(self.follower_dir, config)
        sess = self.follower_conn.open_session()
        sess.create(self.uri, self.table_config)
        sess.close()

    def write_and_checkpoint(self, conn=None):
        # A checkpoint only runs the key provider when there is dirty data to flush.
        if conn is None:
            conn = self.conn
        session = conn.open_session()
        cursor = session.open_cursor(self.uri)
        self.row += 1
        cursor[self.ds.key(self.row)] = self.ds.value(self.row)
        cursor.close()
        session.checkpoint()
        session.close()

    def baseline_kek(self, timestamp):
        # Persist an initial KEK; a checkpoint with none aborts.
        self.push_crypt_key(timestamp)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(timestamp))
        self.write_and_checkpoint()
        self.validate_latest_kek(timestamp)

    def step_up_follower(self):
        # Promote the follower to leader as self.conn.
        self.disagg_switch_follower_and_leader(self.follower_conn, self.conn)
        self.session.close()
        self.conn, self.follower_conn = self.follower_conn, self.conn
        self.session = self.conn.open_session()

    def test_step_up_keeps_timestamps_monotonic(self):
        self.populate_table()
        self.setup_follower()
        self.baseline_kek(10)

        # Promote the follower, checkpoint at 20.
        self.push_crypt_key(20, conn=self.follower_conn)
        self.step_up_follower()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        self.write_and_checkpoint()
        self.validate_latest_kek(20)

        # Old leader steps back up, checkpoint at 30.
        self.push_crypt_key(30, conn=self.follower_conn)
        self.step_up_follower()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))
        self.write_and_checkpoint()
        self.validate_latest_kek(30)

        # Keys advanced 10, 20, 30 across the switches.
        keys = [p['key'] for p in self.fetch_key_provider_pages()]
        self.assertEqual(keys, [self.generate_crypt_key(10), self.generate_crypt_key(20), self.generate_crypt_key(30)])

        self.follower_conn.close()

    def test_follower_prunes_on_pickup(self):
        self.populate_table()
        self.setup_follower()
        self.baseline_kek(10)

        # Queue two keys only on the follower.
        self.push_crypt_key(20, conn=self.follower_conn)
        self.push_crypt_key(50, conn=self.follower_conn)

        # Leader checkpoints between the two queued keys.
        self.push_crypt_key(30)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))
        self.write_and_checkpoint()
        self.validate_latest_kek(30)
        durable_pages = self.key_provider_page_count()

        # Step-up picks up the checkpoint, pruning 20, keeping 50, writing nothing.
        self.step_up_follower()
        self.assertEqual(self.key_provider_page_count(), durable_pages)

        # Stable reaches 50, so it is persisted.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))
        self.write_and_checkpoint()
        self.validate_latest_kek(50)

        self.follower_conn.close()

    def test_step_up_rotation_waits_for_stable(self):
        self.populate_table()
        self.setup_follower()
        self.baseline_kek(10)

        # Queue a future key on the follower, then promote it.
        self.push_crypt_key(30, conn=self.follower_conn)
        self.step_up_follower()

        # Stable below the key persists nothing.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        self.write_and_checkpoint()
        self.validate_latest_kek(10)

        # Stable reaches 30, completing the rotation.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))
        self.write_and_checkpoint()
        self.validate_latest_kek(30)

        self.follower_conn.close()
