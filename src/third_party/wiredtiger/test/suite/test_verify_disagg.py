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

import errno, os, wiredtiger, wttest
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_verify_disagg.py
#    SESSION::verify() testing for disagg storage

# FIXME-WT-15047: Implement tests for populated ingest tables verification
#    (we already have an OpLog imitation in some tests for layered tables)

@disagg_test_class
class test_verify_disagg(wttest.WiredTigerTestCase, DisaggConfigMixin):
    hs = [
        ('empty', dict(fill_hs=False)),
        ('populated', dict(fill_hs=True)),
    ]
    disagg_storages = gen_disagg_storages('test_verify_disagg', disagg_only = True)
    scenarios = make_scenarios(hs, disagg_storages)

    nitems = 10000
    timestamp = 2

    conn_base_config = 'disaggregated=(page_log=palm),'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'
    conn_config_follower = conn_base_config + 'disaggregated=(role="follower")'

    table_cfg = 'key_format=S,value_format=S,block_manager=disagg'

    session_follow = None
    conn_follow = None

    uri = 'layered:test_verify_disagg'

    def leader_put_data(self, value_prefix = '', low = 1, high = nitems):
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(low, high):
            self.session.begin_transaction()
            cursor[str(i)] = value_prefix + str(i)
            self.timestamp += 1
            # Setting the commit timestamp to fill the history store if required
            ts_cfg = "commit_timestamp=" + self.timestamp_str(self.timestamp) if self.fill_hs else None
            self.session.commit_transaction(ts_cfg)
        cursor.close()

    def verify(self, sessions, expected_error = None):
        for session in sessions:
            if expected_error:
                self.assertRaisesException(wiredtiger.WiredTigerError, \
                    lambda: session.verify(self.uri), os.strerror(expected_error))
            else:
                session.verify(self.uri)

    def create_follower(self):
        self.conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + ',create,' +
                                                self.conn_config_follower)
        self.session_follow = self.conn_follow.open_session('')

    def test_verify_disagg(self):
        if self.fill_hs:
            self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(self.timestamp))

        # Create a table in the leader
        self.session.create(self.uri, self.table_cfg)
        # Verify the empty leader's table
        self.verify([self.session])

        # Create a follower
        self.create_follower()
        # The leader's table stays empty, the follower creation doesn't mean loading tables from the leader (it requires reconfiguration)
        self.verify([self.session])
        self.verify([self.session_follow], errno.ENOENT)

        # Create an empty checkpoint
        self.session.checkpoint()
        self.verify([self.session])

        self.disagg_advance_checkpoint(self.conn_follow)
        # Now both connections should have empty tables
        self.verify([self.session, self.session_follow])

        # Put some data to the leader
        self.leader_put_data()
        # Perform update operations to fill HS
        self.leader_put_data(value_prefix = 'aaa')
        self.leader_put_data(value_prefix = 'bbb')
        # That's not allowed to perform verification if there is some dirty data
        self.verify([self.session], errno.EBUSY)

        # Checkpoint the data on the leader
        self.session.checkpoint()
        # Verify the leader's populated table
        self.verify([self.session])

        # Load the latest checkpoint to the follower
        self.disagg_advance_checkpoint(self.conn_follow)
        # Verify both the leader's and the follower's populated tables
        self.verify([self.session, self.session_follow])

        self.session_follow.close()
        self.conn_follow.close()

        # The leader is still alive, verify it.
        self.verify([self.session])
