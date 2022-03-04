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

import re
from wiredtiger import stat
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios
from helper import simulate_crash_restart
from test_rollback_to_stable01 import test_rollback_to_stable_base

# test_rollback_to_stable28.py
# Test the debug mode setting for update_restore_evict during recovery.
# Force update restore eviction, whenever we evict a page. We want to
# perform this in recovery to ensure that all the in-memory images have
# the proper write generation number and we don't end up reading stale
# transaction ID's stored on the page.
class test_rollback_to_stable28(test_rollback_to_stable_base):
    conn_config = 'log=(enabled=true),statistics=(all)'
    # Recovery connection config: The debug mode is only effective on high cache pressure as WiredTiger can potentially decide
    # to do an update restore evict on a page when the cache pressure requirements are not met.
    # This means setting eviction target low and cache size low.
    conn_recon = ',eviction_updates_trigger=10,eviction_dirty_trigger=5,eviction_dirty_target=1,' \
            'cache_size=1MB,debug_mode=(update_restore_evict=true),log=(recover=on)'

    # In principle this test should be run on VLCS and FLCS; but it doesn't run reliably, in that
    # while it always works in the sense of producing the right values, it doesn't always trigger
    # update restore eviction, and then the assertions about that fail. For FLCS, using small
    # pages and twice as many rows makes it work most of the time, but not always (especially on
    # the test machines...) and it also apparently fails some of the time on VLCS. For the moment
    # we've concluded that the marginal benefit of running on VLCS and particularly FLCS is small
    # so disabling these scenarios seems like the best strategy.
    format_values = [
        #('column', dict(key_format='r', value_format='S', extraconfig='')),
        #('column_fix', dict(key_format='r', value_format='8t', 
        #    extraconfig=',allocation_size=512,leaf_page_max=512')),
        ('row_integer', dict(key_format='i', value_format='S', extraconfig='')),
    ]

    scenarios = make_scenarios(format_values)

    def parse_write_gen(self, uri):
        meta_cursor = self.session.open_cursor('metadata:')
        config = meta_cursor[uri]
        meta_cursor.close()
        # The search string will look like: 'run_write_gen=<num>'.
        # Just reverse the string and take the digits from the back until we hit '='.
        write_gen = re.search('write_gen=\d+', config)
        run_write_gen = re.search('run_write_gen=\d+', config)
        self.assertTrue(write_gen is not None)
        write_gen_str = str()
        run_write_gen_str = str()
        for c in reversed(write_gen.group(0)):
            if not c.isdigit():
                self.assertEqual(c, '=')
                break
            write_gen_str = c + write_gen_str
        for c in reversed(run_write_gen.group(0)):
            if not c.isdigit():
                self.assertEqual(c, '=')
                break
            run_write_gen_str = c + run_write_gen_str

        return int(write_gen_str), int(run_write_gen_str)

    def test_update_restore_evict_recovery(self):
        uri = 'table:test_debug_mode10'
        nrows = 10000

        # Create our table.
        ds = SimpleDataSet(self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config='log=(enabled=false)' + self.extraconfig)
        ds.populate()

        if self.value_format == '8t':
            nrows *= 2
            value_a = 97
            value_b = 98
            value_c = 99
            value_d = 100
        else:
            value_a = 'a' * 500
            value_b = 'b' * 500
            value_c = 'c' * 500
            value_d = 'd' * 500

        # Perform several updates.
        self.large_updates(uri, value_a, ds, nrows, False, 20)
        self.large_updates(uri, value_b, ds, nrows, False, 30)
        self.large_updates(uri, value_c, ds, nrows, False, 40)
        # Verify data is visible and correct.
        self.check(value_a, uri, nrows, None, 20)
        self.check(value_b, uri, nrows, None, 30)
        self.check(value_c, uri, nrows, None, 40)

        # Pin the stable timestamp to 40. We will be validating the state of the data post-stable timestamp
        # after we perform a recovery.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))

        # Perform additional updates post-stable timestamp.
        self.large_updates(uri, value_d, ds, nrows, False, 50)
        self.large_updates(uri, value_a, ds, nrows, False, 60)
        self.large_updates(uri, value_b, ds, nrows, False, 70)

        # Verify additional updated data is visible and correct.
        self.check(value_d, uri, nrows, None, 50)
        self.check(value_a, uri, nrows, None, 60)
        self.check(value_b, uri, nrows, None, 70)

        # Checkpoint to ensure the data is flushed to disk.
        self.session.checkpoint()

        # Extract the most recent checkpoints write gen & run write gen. As we are still on a new DB connection,
        # the run write gen should be 1 at this point, equal to the connection-wide base write gen.
        # Since we checkpointed after a series of large writes/updates, the write gen of the pages should
        # definitely be greater than 1.
        checkpoint_write_gen, checkpoint_run_write_gen = self.parse_write_gen("file:test_debug_mode10.wt")
        self.assertEqual(checkpoint_run_write_gen, 1)
        self.assertGreater(checkpoint_write_gen, checkpoint_run_write_gen)

        # Simulate a crash/restart, opening our new DB in recovery. As we open in recovery we want to additionally
        # use the 'update_restore_evict' debug option to trigger update restore eviction.
        self.conn_config = self.conn_config + self.conn_recon
        simulate_crash_restart(self, ".", "RESTART")

        # As we've created a new DB connection post-shutdown, the connection-wide
        # base write gen should eventually initialise from the previous checkpoint's base 'write_gen' during the recovery process
        # ('write_gen'+1). This should be reflected in the initialisation of the 'run_write_gen' field of the newest
        # checkpoint post-recovery. As the recovery/rts process updates our pages, we'd also expect the latest checkpoint's
        # 'write_gen' to again be greater than its 'run_write_gen'.
        recovery_write_gen, recovery_run_write_gen = self.parse_write_gen("file:test_debug_mode10.wt")
        self.assertGreater(recovery_run_write_gen, checkpoint_write_gen)
        self.assertGreater(recovery_write_gen, recovery_run_write_gen)

        # Read the statistics of pages that have been update restored (to check the mechanism was used).
        stat_cursor = self.session.open_cursor('statistics:')
        pages_update_restored = stat_cursor[stat.conn.cache_write_restore][2]
        stat_cursor.close()
        self.assertGreater(pages_update_restored, 0)

        # Check that after recovery, we see the correct data with respect to our previous stable timestamp (40).
        self.check(value_c, uri, nrows, None, 40)
        self.check(value_c, uri, nrows, None, 50)
        self.check(value_c, uri, nrows, None, 60)
        self.check(value_c, uri, nrows, None, 70)
        self.check(value_b, uri, nrows, None, 30)
        self.check(value_a, uri, nrows, None, 20)
        # Passing 0 results in opening a transaction with no read timestamp.
        self.check(value_c, uri, nrows, None, 0)
