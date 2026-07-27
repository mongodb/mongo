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

import re, sys, time
import wttest, wiredtiger
from helper_disagg import disagg_test_class

# test_layered_stepup13.py
#
# A node that steps down and steps back up must treat the checkpoints from
# its previous stint as leader as belonging to an older run: their
# transaction ids are meaningless after the role change and must be
# considered cleared. Since a node never picks its own checkpoints back up,
# the step-up itself has to move the base write generation past everything
# the node has persisted; the trees it reopens for the new role then start
# a new run.
#
# Verify the new stint's first checkpoint records a newer run write
# generation than the previous stint's, and that verification, whose page
# walk compares every page against the aggregate its parent reports, is
# clean over the mix of both stints' pages.
@disagg_test_class
class test_layered_stepup13(wttest.WiredTigerTestCase):
    test_name = __qualname__
    uri = f'layered:{test_name}'

    conn_base_config = 'statistics=(all),disaggregated=(lose_all_my_data=true),' \
                     + 'page_delta=(delta_pct=100,internal_page_delta=true,leaf_page_delta=true),'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'

    # Small pages build a tree with several levels above the leaves.
    create_session_config = 'key_format=S,value_format=S,allocation_size=512,' \
                          + 'leaf_page_max=512,internal_page_max=512'

    nitems = 5000

    def parse_run_write_gen(self, uri):
        meta_cursor = self.session.open_cursor('metadata:')
        config = meta_cursor[uri]
        meta_cursor.close()
        # The search string will look like: 'run_write_gen=<num>'.
        run_write_gen = re.search('run_write_gen=(\d+)', config)
        self.assertTrue(run_write_gen is not None)
        return int(run_write_gen.group(1))

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

    def test_step_up_starts_new_run(self):
        if sys.platform.startswith('darwin'):
            return

        self.ignoreStdoutPattern('Picking up the same checkpoint again')

        self.session.create(self.uri, self.create_session_config)

        # First stint as leader: populate under an open transaction so the
        # populating transactions' ids are persisted rather than cleared.
        pin_session = self.conn.open_session('')
        pin_session.begin_transaction()

        value = 'v' * 20
        base_ts = 10
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(1, self.nitems + 1):
            self.session.begin_transaction()
            cursor[str(i)] = value
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(base_ts))
        cursor.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(base_ts)
            + ',oldest_timestamp=' + self.timestamp_str(base_ts))
        self.session.checkpoint()

        pin_session.rollback_transaction()
        pin_session.close()

        first_stint_gen = self.parse_run_write_gen(f'file:{self.test_name}.wt_stable')

        # Step down and step back up. The latest checkpoint is the node's
        # own, so nothing new is picked up across the role changes.
        self.conn.reconfigure('disaggregated=(role="follower")')
        self.conn.reconfigure('disaggregated=(role="leader")')

        # Second stint: update every key in one narrow range while another
        # transaction is running, so the rewritten pages carry this stint's
        # transaction ids while the untouched pages keep their first-stint
        # images, and the internal pages above are written as deltas on the
        # first stint's base images.
        pin_session = self.conn.open_session('')
        pin_cursor = pin_session.open_cursor(self.uri, None, None)
        pin_session.begin_transaction()
        pin_cursor.set_key('1')
        pin_cursor.search()

        update_ts = base_ts + 10
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(2001, 2401):
            self.session.begin_transaction()
            cursor[str(i)] = value + 'x'
            self.session.commit_transaction(
                'commit_timestamp=' + self.timestamp_str(update_ts))
        cursor.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(update_ts)
            + ',oldest_timestamp=' + self.timestamp_str(update_ts))
        self.session.checkpoint()

        pin_cursor.close()
        pin_session.rollback_transaction()
        pin_session.close()

        # The second stint is a new run: its checkpoint must record a newer
        # run write generation, marking the first stint's transaction ids
        # as belonging to an older run.
        second_stint_gen = self.parse_run_write_gen(f'file:{self.test_name}.wt_stable')
        self.assertGreater(second_stint_gen, first_stint_gen)

        self.verify_retry(self.session)
