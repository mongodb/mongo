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

from time import sleep
import wttest
from wiredtiger import stat

# test_compact08.py
# Test background compaction interruption. The background compaction server is considered as
# interrupted if is it disabled while it is compacting a table. If it is disable between two tables,
# it is not considered as an interruption.
class test_compact08(wttest.WiredTigerTestCase):
    # Make compact slow to increase the chances of interruption. 
    conn_config = 'timing_stress_for_test=[compact_slow]'
    create_params = 'key_format=i,value_format=S,allocation_size=4KB,leaf_page_max=32KB,'
    # Have enough tables to give the server something to work on.
    num_tables = 5
    table_numkv = 100 * 1000
    table_uri='table:test_compact08'

    delete_range_len = 10 * 1000
    delete_ranges_count = 4
    value_size = 1024 # The value should be small enough so that we don't create overflow pages.

    def get_bg_compaction_interrupted(self):
        c = self.session.open_cursor('statistics:', None, 'statistics=(all)')
        running = c[stat.conn.background_compact_interrupted][2]
        c.close()
        return running

    def get_bg_compaction_running(self):
        c = self.session.open_cursor('statistics:', None, 'statistics=(all)')
        running = c[stat.conn.background_compact_running][2]
        c.close()
        return running

    # Create the table in a way that it creates a mostly empty file.
    def test_compact08(self):

        # FIXME-WT-11399
        if self.runningHook('tiered'):
            self.skipTest("this test does not yet work with tiered storage")

        # Create the tables and populate them.
        for i in range(0, self.num_tables):
            uri = f'{self.table_uri}_{i}'
            self.session.create(uri, self.create_params)
            c = self.session.open_cursor(uri, None)
            for k in range(self.table_numkv):
                c[k] = ('%07d' % k) + '_' + 'abcd' * ((self.value_size // 4) - 2)
            c.close()
        self.session.checkpoint()

        # Now let's delete a lot of data ranges. Create enough space so that compact runs in more
        # than one iteration.
        for i in range(0, self.num_tables):
            uri = f'{self.table_uri}_{i}'
            c = self.session.open_cursor(uri, None)
            for r in range(self.delete_ranges_count):
                start = r * self.table_numkv // self.delete_ranges_count
                for i in range(self.delete_range_len):
                    c.set_key(start + i)
                    c.remove()
            c.close()

        num_interruption = 0
        # Expect a message indication the interruption.
        with self.expectedStderrPattern('background compact interrupted by application'):
            # It is possible that the server was disabled between two tables which is not considered
            # as an interruption, retry a few times if needed.
            for i in range(0, 10):

                # Background compaction should not be running yet.
                self.assertEqual(self.get_bg_compaction_running(), 0)

                # Start the background compaction server with a low threshold to make sure it starts
                # working.
                self.session.compact(None, 'background=true,free_space_target=1MB')

                # Wait for the server to wake up.
                running = self.get_bg_compaction_running()
                while not running:
                    sleep(1)
                    running = self.get_bg_compaction_running()
                self.assertEqual(running, 1)

                # Disable the server which may interrupt compaction.
                self.session.compact(None, 'background=false')

                # Wait for the server to stop.
                while running:
                    sleep(1)
                    running = self.get_bg_compaction_running()
                self.assertEqual(running, 0)

                # Check the server has been interrupted.
                num_interruption = self.get_bg_compaction_interrupted()
                if num_interruption > 0:
                    break

        self.assertEqual(num_interruption, 1)

if __name__ == '__main__':
    wttest.run()
