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

import sys, time
import wttest, wiredtiger
from helper_disagg import disagg_test_class

# test_layered_delta16.py
#
# Verify a tree whose internal pages carry deltas written by a different
# leader than their base images.
#
# The first leader's checkpoint provides the internal pages' base images.
# After a role switch, the new leader updates a spread of keys while another
# transaction is running, so the updated pages and the aggregates above them
# are persisted with their transaction ids. The internal pages above them are
# written as deltas on the first leader's base images.
#
# Rebuilding such a page merges the old base image with the new deltas: the
# decision to clear a delta cell's transaction ids must be made against the
# delta that carries the cell, not the base image beneath it. Clearing by the
# base image's age strips the ids from current cells, and verification then
# finds pages holding newer transactions than their parents report.
@disagg_test_class
class test_layered_delta16(wttest.WiredTigerTestCase):
    test_name = __qualname__
    uri = f'layered:{test_name}'

    conn_base_config = 'statistics=(all),disaggregated=(lose_all_my_data=true),' \
                     + 'page_delta=(delta_pct=100,internal_page_delta=true,leaf_page_delta=true),'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'

    # Small pages build a tree with several levels above the leaves.
    create_session_config = 'key_format=S,value_format=S,allocation_size=512,' \
                          + 'leaf_page_max=512,internal_page_max=512'

    nitems = 5000

    def verify_retry(self, session):
        # The stable table can be transiently busy; retry on EBUSY but let a
        # genuine verification failure through.
        for _ in range(60):
            try:
                session.verify(self.uri, None)
                return
            except wiredtiger.WiredTigerError as e:
                if 'resource busy' not in str(e):
                    raise
                time.sleep(1)

    def test_verify_internal_deltas_on_old_base(self):
        if sys.platform.startswith('darwin'):
            return

        self.ignoreStdoutPattern('Picking up the same checkpoint again')

        self.session.create(self.uri, self.create_session_config)

        self.conn_follow = self.wiredtiger_open('follower',
            self.extensionsConfig() + ',create,' + self.conn_base_config
            + 'disaggregated=(role="follower")')
        session_follow = self.conn_follow.open_session('')
        session_follow.create(self.uri, self.create_session_config)

        # First leader: populate the whole key space and checkpoint. This
        # provides the base images for the internal pages.
        value = 'v' * 20
        base_ts = 10
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(1, self.nitems + 1):
            self.session.begin_transaction()
            cursor[str(i)] = value
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(base_ts))
        cursor.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(base_ts))
        self.session.checkpoint()

        # The second node becomes the leader.
        self.disagg_switch_follower_and_leader(self.conn_follow, self.conn)

        # Hold a transaction open on the new leader so the update transactions
        # below are still needed when their pages are written: their ids are
        # persisted rather than cleared.
        pin_session = self.conn_follow.open_session('')
        pin_cursor = pin_session.open_cursor(self.uri, None, None)
        pin_session.begin_transaction()
        pin_cursor.set_key('1')
        pin_cursor.search()

        # Update every key in one narrow range: the leaves and the internal
        # pages directly above them are rewritten as full images carrying the
        # new leader's transaction ids, while the internal pages higher up --
        # with only a few changed children -- are written as deltas on the
        # first leader's base images.
        update_ts = base_ts + 10
        cursor = session_follow.open_cursor(self.uri, None, None)
        for i in range(2001, 2401):
            session_follow.begin_transaction()
            cursor[str(i)] = value + 'x'
            session_follow.commit_transaction(
                'commit_timestamp=' + self.timestamp_str(update_ts))
        cursor.close()
        self.conn_follow.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(update_ts))
        session_follow.checkpoint()

        pin_cursor.close()
        pin_session.rollback_transaction()
        pin_session.close()

        # Verify on the new leader: the walk rebuilds the delta-written
        # internal pages from the old base images and the new deltas, and
        # checks every page against the aggregate its parent reports for it.
        self.verify_retry(session_follow)
