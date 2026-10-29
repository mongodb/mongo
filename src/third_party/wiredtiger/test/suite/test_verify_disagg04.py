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
from wtscenario import make_scenarios

# test_verify_disagg04.py
#    Run WT_SESSION::verify against a populated shared history store on a disaggregated
#    follower. This drives __wt_hs_verify_one, which must read the history store at the
#    checkpoint pinned by the stable btree rather than through a live handle.

@disagg_test_class
class test_verify_disagg04(wttest.WiredTigerTestCase):
    test_name = __qualname__
    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    conn_config = 'disaggregated=(role="leader")'
    conn_config_follower = 'disaggregated=(role="follower")'

    table_cfg = 'key_format=S,value_format=S,block_manager=disagg'
    uri = f'layered:{test_name}'
    nitems = 200

    def test_verify_follower_populated_hs(self):
        self.session.create(self.uri, self.table_cfg)
        cursor = self.session.open_cursor(self.uri, None, None)

        # Write every key at ts=5 and checkpoint.
        for i in range(self.nitems):
            self.session.begin_transaction()
            cursor[str(i)] = 'v1_' + str(i)
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(5))
        self.session.checkpoint()

        # Overwrite every key at ts=10. The ts=5 versions move into the history store.
        for i in range(self.nitems):
            self.session.begin_transaction()
            cursor[str(i)] = 'v2_' + str(i)
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()

        # Delete every other key at ts=15, giving those history store records a stop timestamp.
        for i in range(0, self.nitems, 2):
            self.session.begin_transaction()
            cursor.set_key(str(i))
            cursor.remove()
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(15))
        self.session.checkpoint()
        cursor.close()

        self.verifyUntilSuccess(self.session)

        conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + ',create,' +
                                           self.conn_config_follower)
        session_follow = conn_follow.open_session('')
        self.disagg_advance_checkpoint(conn_follow)

        # The stable btree is opened from its picked-up checkpoint, so the history store must
        # be read from the checkpoint pinned alongside it.
        self.verifyUntilSuccess(session_follow)

        session_follow.close()
        conn_follow.close()
