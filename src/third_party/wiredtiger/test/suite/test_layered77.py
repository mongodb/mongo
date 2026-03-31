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

import time, wttest
from eviction_util import eviction_util
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios


# Test that leader-to-follower role transition doesn't crash when eviction
# processes pages that were split during a prior checkpoint.
@disagg_test_class
class test_layered77(eviction_util, wttest.WiredTigerTestCase):
    conn_base_config = 'cache_size=10MB,'

    disagg_storages = gen_disagg_storages('test_layered77', disagg_only = True)
    scenarios = make_scenarios(disagg_storages)
    uri='layered:test_layered77'

    def conn_config(self):
        return self.conn_base_config + 'disaggregated=(role="leader"),'

    def conn_config_follower(self):
        return self.conn_base_config + 'disaggregated=(role="follower"),'

    def test_step_down_dirty_eviction(self):
        """
        Test that transitioning from leader to follower role doesn't crash when
        eviction processes pages that were split during a prior checkpoint.

        During the role transition window, eviction threads may still be processing
        pages that have pending split state from checkpoint. This test verifies that
        the transition is handled safely without assertion failures.
        """
        create_params = 'key_format=i,value_format=S,block_manager=disagg'
        nrows = 10000
        value = 'k' * 1024  # 1KB per row, ~10MB total fills the 10MB cache

        self.session.create(self.uri, create_params)

        # Write data as leader.
        self.populate(self.uri, 0, nrows, value)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(nrows))

        # Checkpoint to create pages with pending split state.
        # After checkpoint, some pages may have been split into multiple blocks
        # but are marked clean. When eviction encounters these pages later, it
        # needs to handle the split state properly.
        self.session.checkpoint()

        # Make eviction aggressive to increase the chance of encountering
        # split pages during the role transition.
        self.conn.reconfigure(
            'eviction_dirty_target=1,eviction_dirty_trigger=5,'
            'eviction_updates_target=1,eviction_updates_trigger=5'
        )

        # Capture checkpoint metadata while still the leader.
        checkpoint_meta = self.disagg_get_complete_checkpoint_meta()

        # Transition from leader to follower role.
        # Eviction threads running concurrently should not crash when they
        # encounter pages with pending split state during this transition.
        self.conn.reconfigure('disaggregated=(role="follower")')

        # Allow time for eviction to process pages during the transition window.
        time.sleep(0.5)

        self.close_conn()

        # Reopen as follower with the captured checkpoint metadata.
        config = (self.conn_config_follower() +
                  f'disaggregated=(checkpoint_meta="{checkpoint_meta}"),')
        self.open_conn(".", config)

        # Verify all data is readable from the follower.
        cursor = self.session.open_cursor(self.uri, None, None)
        count = 0
        while cursor.next() == 0:
            count += 1
        cursor.close()
        self.assertEqual(count, nrows)
