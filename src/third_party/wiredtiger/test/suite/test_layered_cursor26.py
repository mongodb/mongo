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

# A table created while the step-down timestamp is set has no stable constituent until a later
# step-up creates one. A statistics cursor on such a table must report the ingest constituent alone
# rather than trying to open the missing one, whichever URI names the table and whichever
# statistics it is asked for.

from helper_disagg import DisaggSchemaEpochMixin, DisaggSizeTestMixin, disagg_test_class
from helper_layered_stepdown import LayeredStepdownMixin
from wiredtiger import stat
import wttest


@disagg_test_class
class test_layered_cursor26(
  DisaggSizeTestMixin, LayeredStepdownMixin, wttest.WiredTigerTestCase, DisaggSchemaEpochMixin):
    conn_config = ('statistics=(all),precise_checkpoint=true,'
                   'disaggregated=(role="leader",lose_all_my_data=true)')

    table_config = 'key_format=S,value_format=S,type=layered'

    # The step-down timestamp every test creates its table under.
    cutoff = 5

    table_uri = f'table:{__qualname__}'
    layered_uri = f'layered:{__qualname__}'

    def create_with_step_down_ts_set(self):
        """Set the step-down timestamp, then create and populate a table under it."""
        self.set_global_ts(1, 1)
        self.set_step_down_ts(self.cutoff)

        self.session.create(self.table_uri, self.table_config)
        # Rows written once the timestamp is set commit above the cutoff and belong to the follower
        # era.
        self.write_at(self.table_uri, {'k1': 'v1', 'k2': 'v2'}, self.cutoff + 1)

        # Assert the premise of every test below, so a create that started behaving differently
        # cannot pass as a correct statistics result.
        self.assertFalse(self.stable_constituent_exists(self.conn, self.layered_uri))

    def block_size(self, uri, config='statistics=(size)'):
        """Return the block size reported by a statistics cursor on uri."""
        with wttest.open_cursor(self.session, f'statistics:{uri}', config=config) as cursor:
            return cursor[stat.dsrc.block_size][2]

    def assert_size_is_ingest_only(self, uri, config):
        """The table has no stable constituent, so its size is the ingest constituent's alone."""
        ingest = self.block_size(self.ingest_uri(self.layered_uri), config)
        # A zero on both sides would satisfy the comparison without proving anything.
        self.assertGreater(ingest, 0)
        self.assertEqual(self.block_size(uri, config), ingest)

    def test_size_stats_on_table_uri(self):
        """The table URI declines the size fast path and falls back to the slow path."""
        self.create_with_step_down_ts_set()
        self.assert_size_is_ingest_only(self.table_uri, 'statistics=(size)')

    def test_size_stats_on_layered_uri(self):
        """The layered URI has no size fast path, so it reaches the slow path directly."""
        self.create_with_step_down_ts_set()
        self.assert_size_is_ingest_only(self.layered_uri, 'statistics=(size)')

    def test_all_stats_on_table_uri(self):
        """Full statistics walk the tree, which looks the checkpoint up a different way."""
        self.create_with_step_down_ts_set()
        self.assert_size_is_ingest_only(self.table_uri, 'statistics=(all)')

    def test_all_stats_on_layered_uri(self):
        """Full statistics on the layered URI take the tree-walk branch without the fast path."""
        self.create_with_step_down_ts_set()
        self.assert_size_is_ingest_only(self.layered_uri, 'statistics=(all)')

    def test_size_reported_once_step_up_creates_the_constituent(self):
        """A table created under the timestamp reports its real size once it has a constituent."""
        self.create_with_step_down_ts_set()

        self.complete_step_down(self.cutoff)
        self.step_up()
        self.leader_checkpoint(self.cutoff + 2)

        self.assertTrue(self.stable_constituent_exists(self.conn, self.layered_uri))
        expected = self.get_checkpoint_size(self.stable_uri(self.layered_uri))
        self.assertGreater(expected, 0)
        self.assertEqual(self.block_size(self.table_uri, 'statistics=(size)'), expected)


if __name__ == '__main__':
    wttest.run()
