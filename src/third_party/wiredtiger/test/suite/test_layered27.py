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

import platform, wttest
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages
from test_layered23 import Oplog
from wtscenario import make_scenarios

# test_layered27.py
# Test draining the ingest table
@wttest.skip_for_hook("tiered", "FIXME-WT-14938: crashing with tiered hook.")
@disagg_test_class
class test_layered27(wttest.WiredTigerTestCase, DisaggConfigMixin):
    conn_base_config = ',create,statistics=(all),statistics_log=(wait=1,json=true,on_close=true),' \
                 + 'disaggregated=(page_log=palm),'

    sizes = [
        ('small', dict(multiplier=1)),
        ('large', dict(multiplier=100)),
    ]

    disagg_storages = gen_disagg_storages('test_layered27', disagg_only = True)

    scenarios = make_scenarios(disagg_storages, sizes)

    uri = 'layered:test_layered27'

    def conn_config(self):
        return self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="leader")'

    def test_drain_insert_update(self):
        if platform.processor() == 's390x':
            self.skipTest("FIXME-WT-15000: not working on zSeries")

        # Create the oplog
        oplog = Oplog()

        # Create the table on leader and tell oplog about it
        self.session.create(self.uri, "key_format=S,value_format=S")
        t = oplog.add_uri(self.uri)

        # Create the follower and create its table
        # To keep this test relatively easy, we're only using a single URI.
        conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="follower")')
        session_follow = conn_follow.open_session('')
        session_follow.create(self.uri, "key_format=S,value_format=S")

        # Create some oplog traffic
        oplog.insert(t, 100 * self.multiplier)

        # Apply them to leader WT and checkpoint.
        oplog.apply(self, self.session, 0, 100 * self.multiplier)
        oplog.check(self, self.session, 0, 100 * self.multiplier)

        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(oplog.last_timestamp())}')

        self.session.checkpoint()     # checkpoint 1

        # Add some more traffic
        oplog.insert(t, 100 * self.multiplier)
        oplog.update(t, 200 * self.multiplier)
        oplog.apply(self, self.session, 100 * self.multiplier, 300 * self.multiplier)
        oplog.check(self, self.session, 0, 400 * self.multiplier)

        # On the follower -
        # Apply all the entries to follower
        oplog.apply(self, session_follow, 0, 400 * self.multiplier)

        self.pr(f'OPLOG: {oplog}')
        oplog.check(self, session_follow, 0, 400 * self.multiplier)

        # Then advance the checkpoint and make sure everything is still good
        self.pr('advance checkpoint')
        self.disagg_advance_checkpoint(conn_follow)
        oplog.check(self, session_follow, 0, 400 * self.multiplier)

        self.conn.close()
        conn_follow.reconfigure('disaggregated=(role="leader")')

        # Checkpoint after draining the ingest table
        conn_follow.set_timestamp(f'stable_timestamp={self.timestamp_str(oplog.last_timestamp())}')
        session_follow.checkpoint()

        # Reopen the new leader as follower to get rid of the content in the ingest table
        conn_follow.close()
        conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="follower")')
        session_follow = conn_follow.open_session('')

        # Ensure everything is in the new checkpoint
        oplog.check(self, session_follow, 0, 400 * self.multiplier)

    def test_drain_remove(self):
        if platform.processor() == 's390x':
            self.skipTest("FIXME-WT-15000: not working on zSeries")

        # Create the oplog
        oplog = Oplog()

        # Create the table on leader and tell oplog about it
        self.session.create(self.uri, "key_format=S,value_format=S")
        t = oplog.add_uri(self.uri)

        # Create the follower and create its table
        # To keep this test relatively easy, we're only using a single URI.
        conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="follower")')
        session_follow = conn_follow.open_session('')
        session_follow.create(self.uri, "key_format=S,value_format=S")

        # Create some oplog traffic
        oplog.insert(t, 100 * self.multiplier)

        # Apply them to leader WT and checkpoint.
        oplog.apply(self, self.session, 0, 100 * self.multiplier)
        oplog.check(self, self.session, 0, 100 * self.multiplier)

        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(oplog.last_timestamp())}')

        self.session.checkpoint()     # checkpoint 1

        # Delete some updates
        oplog.remove(t, 100 * self.multiplier)
        oplog.apply(self, self.session, 100 * self.multiplier, 100 * self.multiplier)
        oplog.check(self, self.session, 0, 200 * self.multiplier)

        # On the follower -
        # Apply all the entries to follower
        oplog.apply(self, session_follow, 0, 200 * self.multiplier)

        self.pr(f'OPLOG: {oplog}')
        oplog.check(self, session_follow, 0, 200 * self.multiplier)

        # Then advance the checkpoint and make sure everything is still good
        self.pr('advance checkpoint')
        self.disagg_advance_checkpoint(conn_follow)
        oplog.check(self, session_follow, 0, 200 * self.multiplier)

        self.conn.close()
        conn_follow.reconfigure('disaggregated=(role="leader")')

        # Checkpoint after draining the ingest table
        conn_follow.set_timestamp(f'stable_timestamp={self.timestamp_str(oplog.last_timestamp())}')
        session_follow.checkpoint()

        # Reopen the new leader as follower to get rid of the content in the ingest table
        conn_follow.close()
        conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="follower")')
        session_follow = conn_follow.open_session('')

        # Ensure everything is in the new checkpoint
        oplog.check(self, session_follow, 0, 200 * self.multiplier)

    def test_drain_remove_insert(self):
        # Create the oplog
        oplog = Oplog()

        # Create the table on leader and tell oplog about it
        self.session.create(self.uri, "key_format=S,value_format=S")
        t = oplog.add_uri(self.uri)

        # Create the follower and create its table
        # To keep this test relatively easy, we're only using a single URI.
        conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="follower")')
        session_follow = conn_follow.open_session('')
        session_follow.create(self.uri, "key_format=S,value_format=S")

        # Create some oplog traffic
        oplog.insert(t, 100 * self.multiplier)

        # Apply them to leader WT and checkpoint.
        oplog.apply(self, self.session, 0, 100 * self.multiplier)
        oplog.check(self, self.session, 0, 100 * self.multiplier)

        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(oplog.last_timestamp())}')

        self.session.checkpoint()     # checkpoint 1

        # Delete some updates
        oplog.remove(t, 100 * self.multiplier)
        oplog.insert(t, 100 * self.multiplier, 0)
        oplog.apply(self, self.session, 100 * self.multiplier, 200 * self.multiplier)
        oplog.check(self, self.session, 0, 300 * self.multiplier)

        # On the follower -
        # Apply all the entries to follower
        oplog.apply(self, session_follow, 0, 300 * self.multiplier)

        self.pr(f'OPLOG: {oplog}')
        oplog.check(self, session_follow, 0, 300 * self.multiplier)

        # Then advance the checkpoint and make sure everything is still good
        self.pr('advance checkpoint')
        self.disagg_advance_checkpoint(conn_follow)
        oplog.check(self, session_follow, 0, 300 * self.multiplier)

        self.conn.close()
        conn_follow.reconfigure('disaggregated=(role="leader")')

        # Checkpoint after draining the ingest table
        conn_follow.set_timestamp(f'stable_timestamp={self.timestamp_str(oplog.last_timestamp())}')
        session_follow.checkpoint()

        # Reopen the new leader as follower to get rid of the content in the ingest table
        conn_follow.close()
        conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="follower")')
        session_follow = conn_follow.open_session('')

        # Ensure everything is in the new checkpoint
        oplog.check(self, session_follow, 0, 300 * self.multiplier)
