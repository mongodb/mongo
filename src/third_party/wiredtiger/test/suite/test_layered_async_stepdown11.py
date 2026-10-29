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

# test_layered_async_stepdown11.py
#    The step-down boundary in schema-epoch space: validation of the step-down disaggregated
#    schema epoch and the publish restrictions it imposes. Schema epochs order schema operations
#    independently of timestamps, so the boundary must be declared and enforced in both spaces.

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages, DisaggSchemaEpochMixin
from helper_layered_stepdown import LayeredStepdownMixin
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_async_stepdown11(
  LayeredStepdownMixin, wttest.WiredTigerTestCase, DisaggSchemaEpochMixin):
    test_name = __qualname__

    table_config = 'key_format=S,value_format=S'
    conn_config = 'statistics=(all),precise_checkpoint=true,' \
        'disaggregated=(role="leader",lose_all_my_data=true)'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def uri(self, name):
        return f'layered:{self.test_name}_{name}'

    # The epoch cannot be supplied without the timestamp: the boundary is one object with two
    # coordinates and must be set atomically.
    def test_epoch_without_ts_rejected(self):
        self.set_stable_epoch(10)
        self.set_global_ts(1, 1)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.conn.set_timestamp(
                'step_down_disaggregated_schema_epoch=' + self.timestamp_str(10)),
            '/requires the step down timestamp/')

    # The timestamp alone is accepted while the server adopts the new parameter, skipping the
    # epoch-space enforcement.
    def test_ts_without_epoch_allowed(self):
        self.set_stable_epoch(10)
        self.set_global_ts(1, 1)
        self.set_step_down_ts(20)
        self.assertEqual(self.step_down_ts_is_set(), 1)
        self.assertEqual(self.step_down_epoch_is_set(), 0)
        self.complete_step_down(20)
        self.assertEqual(self.step_down_ts_is_set(), 0)

    # Without schema epochs there is no epoch space to bound.
    def test_epoch_without_epochs_in_use_rejected(self):
        self.set_global_ts(1, 1)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.set_step_down_ts(20, 10),
            '/requires schema epochs to be in use/')

    # The stable epoch is monotonic and must be able to reach the boundary exactly, so the
    # boundary cannot sit below it. Equality is allowed.
    def test_epoch_below_stable_epoch_rejected(self):
        self.set_stable_epoch(10)
        self.set_global_ts(1, 1)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.set_step_down_ts(20, 5),
            '/must not be older than the stable disaggregated schema epoch/')
        self.set_step_down_ts(20, 10)
        self.assertEqual(self.step_down_epoch_is_set(), 1)

    # While the boundary is set the stable epoch may reach it exactly but never pass it.
    def test_stable_epoch_cannot_pass_boundary(self):
        self.set_stable_epoch(10)
        self.set_global_ts(1, 1)
        self.set_step_down_ts(20, 15)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.set_stable_epoch(16),
            '/must not advance past the step down disaggregated schema epoch/')
        self.set_stable_epoch(15)

    # Setting the boundary and advancing the stable epoch in one call obeys the same ordering.
    def test_boundary_and_stable_epoch_in_one_call(self):
        self.set_stable_epoch(10)
        self.set_global_ts(1, 1)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.conn.set_timestamp(
                'step_down_timestamp=' + self.timestamp_str(20) +
                ',step_down_disaggregated_schema_epoch=' + self.timestamp_str(12) +
                ',stable_disaggregated_schema_epoch=' + self.timestamp_str(15)),
            '/must not be older than the stable disaggregated schema epoch/')
        self.conn.set_timestamp(
            'step_down_timestamp=' + self.timestamp_str(20) +
            ',step_down_disaggregated_schema_epoch=' + self.timestamp_str(15) +
            ',stable_disaggregated_schema_epoch=' + self.timestamp_str(15))
        self.assertEqual(self.step_down_epoch_is_set(), 1)

    # Completing the step-down clears the boundary in both spaces.
    def test_step_down_clears_epoch(self):
        self.set_stable_epoch(10)
        self.set_global_ts(1, 1)
        uri = self.uri('cleared')
        self.session.create(uri, self.table_config)
        self.publish(uri, 15)
        self.set_stable_epoch(15)

        self.set_step_down_ts(20, 15)
        self.assertEqual(self.step_down_ts_is_set(), 1)
        self.assertEqual(self.step_down_epoch_is_set(), 1)

        self.complete_step_down(20)
        self.assertEqual(self.step_down_ts_is_set(), 0)
        self.assertEqual(self.step_down_epoch_is_set(), 0)

    # While the boundary is set an epoch is only assigned above it. A publish above the boundary
    # lands in the next leader era, deferred past every checkpoint of this one; a publish at or
    # below the boundary is rejected.
    def test_publish_only_above_boundary(self):
        self.set_stable_epoch(10)
        self.set_global_ts(1, 1)
        self.set_step_down_ts(20, 15)

        above = self.uri('above')
        below = self.uri('below')
        self.session.create(above, self.table_config)
        self.session.create(below, self.table_config)

        self.publish(above, 16)
        # The boundary epoch itself is this era's, which a window create cannot claim.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.publish(below, 15),
            '/at or below the step down boundary/')

        self.complete_step_down(20, epoch=15)
        self.assertFalse(self.uri_in_shared_metadata(self.conn, above))
        self.assertFalse(self.uri_in_shared_metadata(self.conn, below))

    # A table created before the boundary belongs to this era, so it can still be published once
    # the boundary is set, but only at an epoch the step-down checkpoint reaches.
    def test_pre_boundary_create_publishes_below_boundary(self):
        self.set_stable_epoch(10)
        self.set_global_ts(1, 1)
        uri = self.uri('pre_boundary')
        self.session.create(uri, self.table_config)

        self.set_step_down_ts(20, 15)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.publish(uri, 16),
            '/above the step down boundary/')
        # The boundary itself belongs to this era: the step-down checkpoint runs at its epoch.
        self.publish(uri, 15)

        self.complete_step_down(20, epoch=15)
        self.assertTrue(self.uri_in_shared_metadata(self.conn, uri))

    # A table created before the boundary is carried by the step-down checkpoint, which is what
    # publishing it at or below the boundary buys.
    def test_pre_boundary_create_is_covered(self):
        self.set_stable_epoch(10)
        self.set_global_ts(1, 1)
        uri = self.uri('covered')
        self.session.create(uri, self.table_config)
        self.publish(uri, 12)

        self.set_step_down_ts(20, 15)
        self.complete_step_down(20, epoch=15)
        self.assertTrue(self.uri_in_shared_metadata(self.conn, uri))

    # A create issued inside the window cannot be claimed by this era: its publish is rejected
    # below the boundary, and the next leader era publishes the whole history.
    def test_publish_window_create_below_boundary_rejected(self):
        self.set_stable_epoch(10)
        self.set_global_ts(1, 1)
        uri = self.uri('recreated')

        # Leave an unpublished create and drop behind, then recreate the table inside the window.
        self.session.create(uri, self.table_config)
        self.dropUntilSuccess(self.session, uri)

        self.set_step_down_ts(20, 15)
        self.session.create(uri, self.table_config)

        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.publish(uri, 12),
            '/at or below the step down boundary/')

        self.complete_step_down(20, epoch=15)
        self.step_up()
        self.publish(uri, 16)
        self.set_stable_epoch(16)
        self.leader_checkpoint(25)
        self.assertTrue(self.uri_in_shared_metadata(self.conn, uri))

    # A drop issued inside the window is bounded the same way as a create.
    def test_publish_window_drop_below_boundary_rejected(self):
        self.set_stable_epoch(10)
        self.set_global_ts(1, 1)
        uri = self.uri('window_dropped')
        self.session.create(uri, self.table_config)
        self.publish(uri, 12)

        self.set_step_down_ts(20, 15)
        self.dropUntilSuccess(self.session, uri)

        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.publish(uri, 12),
            '/at or below the step down boundary/')
        self.publish(uri, 16)

        self.complete_step_down(20, epoch=15)
        # The create was covered by the step-down checkpoint, so the table survives this era and
        # the drop lands in the next one.
        self.assertTrue(self.uri_in_shared_metadata(self.conn, uri))

    # A drop issued before the boundary belongs to this era: it can only be published at an epoch
    # the step-down checkpoint reaches, taking the table out of it.
    def test_pre_boundary_drop_publishes_below_boundary(self):
        self.set_stable_epoch(10)
        self.set_global_ts(1, 1)
        uri = self.uri('pre_dropped')
        self.session.create(uri, self.table_config)
        self.publish(uri, 11)
        self.dropUntilSuccess(self.session, uri)

        self.set_step_down_ts(20, 15)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.publish(uri, 16),
            '/above the step down boundary/')
        self.publish(uri, 12)

        self.complete_step_down(20, epoch=15)
        self.assertFalse(self.uri_in_shared_metadata(self.conn, uri))

    # Dropping a table whose create was never published cancels the create, so no pair is left to
    # publish and the whole history resolves inside this era without the table ever surfacing.
    def test_pre_boundary_create_drop_pair_cancels(self):
        self.set_stable_epoch(10)
        self.set_global_ts(1, 1)
        uri = self.uri('pair')
        self.session.create(uri, self.table_config)
        self.dropUntilSuccess(self.session, uri)

        self.set_step_down_ts(20, 15)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.publish(uri, 12),
            '/No pending schema operations to publish/')

        self.complete_step_down(20, epoch=15)
        self.assertFalse(self.uri_in_shared_metadata(self.conn, uri))
