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

import re, wttest
from wiredtiger import stat
from helper_disagg import DisaggConfigMixin, disagg_test_class

# test_disagg_checkpoint_size03.py
#   Test that the checkpoint size does not grow excessively due to a bytes_total
#   leak in the disaggregated checkpoint code.
@disagg_test_class
class test_disagg_checkpoint_size03(wttest.WiredTigerTestCase):

    uri_base = "test_disagg_ckpt_size03"
    conn_config = 'disaggregated=(role="leader",lose_all_my_data=true), page_delta=(delta_pct=90,internal_page_delta=true,leaf_page_delta=true,max_consecutive_delta=5)'
    uri = "layered:" + uri_base

    def conn_extensions(self, extlist):
        extlist.skip_if_missing = True
        DisaggConfigMixin.conn_extensions(self, extlist)

    def get_checkpoint_size(self):
        stable_uri = f'file:{self.uri_base}.wt_stable'
        mc = self.session.open_cursor('metadata:')
        mc.set_key(stable_uri)
        mc.search()
        size = int(re.findall(r',size=(\d+),', mc.get_value())[-1])
        mc.close()
        return size

    # Reconfigure to the default delta_pct (20%) for this test. The class-level
    # conn_config uses delta_pct=90 for the delta tests.
    def test_bytes_total_leak(self):
        self.conn.reconfigure('page_delta=(delta_pct=20)')
        self.session.create(self.uri, 'key_format=S,value_format=S')
        nrows = 1
        val_size = 10

        # Insert data and take the baseline checkpoint.
        c = self.session.open_cursor(self.uri)
        for i in range(nrows):
            c[f'key{i:06d}'] = 'a' * val_size
        c.close()
        self.session.checkpoint()
        baseline = self.get_checkpoint_size()

        # Rewrite every row three times, checkpointing each time.
        # Each cycle rewrites all leaf, internal, and root pages.
        for cycle, ch in enumerate(['b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm'], start=1):
            c = self.session.open_cursor(self.uri)
            for i in range(nrows):
                c[f'key{i:06d}'] = ch * val_size
            c.close()
            self.session.checkpoint()

        final = self.get_checkpoint_size()

        # With delta_pct=20 and this workload (rewriting every row each cycle),
        # no deltas should ever be emitted, only full page images.
        stat_cursor = self.session.open_cursor('statistics:' + self.uri)
        delta_count = stat_cursor[stat.dsrc.rec_page_delta_leaf][2]
        stat_cursor.close()
        self.assertEqual(delta_count, 0,
            f"Expected no deltas with delta_pct=20, but got {delta_count}")

        # The data volume hasn't changed, same nrows, same val_size.
        # The checkpoint size should stay near the baseline, not grow to ~3x.
        self.pr(f"Final: {final}, Baseline: {baseline}, multiple of baseline: {final/baseline:.1f}x")
        self.assertLess(final, baseline * 2,
            f"bytes_total is leaking: baseline={baseline}, after 3 rewrite cycles={final} "
            f"({final/baseline:.1f}x). Old disagg page blocks are not being freed during "
            f"single-page reconciliation  see disagg_page_free_required in rec_write.c "
            f"and disagg_free_block in __wt_ref_block_free().")


    # Uses the class-level delta_pct=90 -- no reconfigure needed.
    def test_bytes_total_leak_delta(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')

        # Write initial page with multiple keys
        c = self.session.open_cursor(self.uri)
        for i in range(10):
            c[f'key{i:02d}'] = f'value{i}'
        c.close()
        self.session.checkpoint()

        # The size of the first page we wrote + its root page.
        baseline = self.get_checkpoint_size()

        # Multiple iterations: first create deltas, then force full page writes
        for cycle, ch in enumerate(['b', 'c', 'd', 'e', 'f', 'g', 'h'], start=1):#, 'h', 'i', 'j', 'k', 'l', 'm'], start=1):
            c = self.session.open_cursor(self.uri)
            # Update existing keys to create deltas
            for i in range(0, 10, 2):
                c[f'key{i:02d}'] = f'newvalue{cycle}{i}'
            c.close()
            self.session.checkpoint()

        final = self.get_checkpoint_size()

        # Size should not grow excessively even with delta operations
        self.assertLess(final, baseline * 2,
            f"Size leak detected: baseline={baseline}, final={final} ({final/baseline:.1f}x). "
            f"Check delta chain termination handling in rec_write.c")

        # Verify we actually created deltas during the test
        stat_cursor = self.session.open_cursor('statistics:' + self.uri)
        total_deltas = stat_cursor[stat.dsrc.rec_page_delta_leaf][2]
        stat_cursor.close()
        self.assertGreater(total_deltas, 0, "No deltas were created during test")

    # Uses the class-level delta_pct=90 -- no reconfigure needed.
    def test_bytes_total_leak_delta_normal_ops(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')

        # Write initial page with multiple keys
        c = self.session.open_cursor(self.uri)
        for i in range(10):
            c[f'key{i:02d}'] = f'value{i}'
        c.close()
        self.session.checkpoint()
        baseline = self.get_checkpoint_size()

        # Multiple iterations updating existing keys to create deltas
        for cycle, ch in enumerate(['b', 'c', 'd', 'e', 'f'], start=1):
            c = self.session.open_cursor(self.uri)
            # Update existing keys instead of inserting new ones
            for i in range(0, 10, 2):  # Update every other key
                c[f'key{i:02d}'] = f'newvalue{cycle}{i}'
            c.close()
            self.session.checkpoint()

            # Verify deltas were created
            stat_cursor = self.session.open_cursor('statistics:' + self.uri)
            delta_count = stat_cursor[stat.dsrc.rec_page_delta_leaf][2]
            stat_cursor.close()
            self.assertGreater(delta_count, 0,
                f"Cycle {cycle}: Expected leaf page deltas but got {delta_count}")

    # Regression test for the size leak after rec_result is set to WT_PAGE_CLEAN.
    def test_size_leak_after_rec_result_page_clean(self):
        nrows = 20
        val_size = 200
        ncycles = 1

        self.session.create(self.uri, 'key_format=S,value_format=S')

        c = self.session.open_cursor(self.uri)
        # Insert 20 keys, roughly 4KB.
        self.session.begin_transaction()
        for i in range(nrows):
            c['key' + str(i)] = 'A' * val_size
        c.close()
        self.session.commit_transaction()
        self.session.checkpoint()
        baseline = self.get_checkpoint_size()
        self.pr(f"Baseline checkpoint size: {baseline}")

        # Generate a delta.
        c = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(0, nrows, 5):
            c['key' + str(i)] = 'x' * val_size
        c.close()
        self.session.commit_transaction()
        self.session.checkpoint()

        size_after_delta = self.get_checkpoint_size()

        # Evict the leaf page.
        self.session.breakpoint()
        evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        evict_cursor.set_key(f'key{0:04d}')
        evict_cursor.search()
        evict_cursor.reset()
        evict_cursor.close()
        self.session.rollback_transaction()

        # Step 3: Read in the evicted page and force a full page rewrite, this triggers our previous
        # leak.
        self.conn.reconfigure('page_delta=(delta_pct=1)')
        c = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(nrows):
            c['key' + str(i)] = 'y' * val_size
        c.close()
        self.session.commit_transaction()
        self.session.checkpoint()

        final = self.get_checkpoint_size()
        self.pr(f"Final: {final}, Baseline: {baseline}, ratio: {final/baseline:.2f}x")

        # If we leak the full page size then final is around 8K, in this case we expect it to be
        # less than 4.8K
        self.assertLess(final, baseline * 1.2)

    # Regression test for a bug in block_disagg_read.c where cumulative_size was set to only the
    # most recent block's raw size instead of the true cumulative total of base + all deltas.
    def test_cumulative_size_leak_after_eviction(self):
        nrows = 20
        val_size = 200
        ncycles = 10

        self.session.create(self.uri, 'key_format=S,value_format=S')

        # Write initial data. Small enough for a single leaf page 4K.
        c = self.session.open_cursor(self.uri)
        for i in range(nrows):
            c['key' + str(i)] = 'A' * val_size
        c.close()
        self.session.checkpoint()
        baseline = self.get_checkpoint_size()
        self.pr(f"Baseline checkpoint size: {baseline}")

        prev_delta_count = 0
        for cycle in range(ncycles):
            # Step 1: Create a delta on top of the current page.
            c = self.session.open_cursor(self.uri)
            for i in range(0, nrows, 5):
                c['key' + str(i)] = 'x' * val_size
            c.close()
            self.session.checkpoint()

            size_after_delta = self.get_checkpoint_size()

            # Verify a new delta was actually created this cycle.
            stat_cursor = self.session.open_cursor('statistics:' + self.uri)
            delta_count = stat_cursor[stat.dsrc.rec_page_delta_leaf][2]
            stat_cursor.close()
            new_deltas = delta_count - prev_delta_count
            prev_delta_count = delta_count
            self.assertGreater(new_deltas, 0,
                f"Cycle {cycle}: expected new deltas but got {new_deltas}")

            # Step 2: Evict the leaf page. On next access it will be read from the page service,
            # prior to the fix this would set cumulative_size to the last delta's raw size instead
            # the true cumulative total.
            evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
            self.session.begin_transaction()
            evict_cursor.set_key(f'key{0:04d}')
            evict_cursor.search()
            evict_cursor.reset()
            evict_cursor.close()
            self.session.rollback_transaction()

            # Step 3: Force a full page rewrite to terminate the delta chain.
            self.conn.reconfigure('page_delta=(delta_pct=1)')
            c = self.session.open_cursor(self.uri)
            for i in range(nrows):
                c['key' + str(i)] = 'y' * val_size
            c.close()
            self.session.checkpoint()

        final = self.get_checkpoint_size()
        self.pr(f"Final: {final}, Baseline: {baseline}, ratio: {final/baseline:.2f}x")

        # The data volume is constant across cycles (same nrows, same val_size).
        # Without the bug, the size stays near baseline.
        self.assertLess(final, baseline * 2)
