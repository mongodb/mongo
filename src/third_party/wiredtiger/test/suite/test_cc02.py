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
#
# [TEST_TAGS]
# checkpoint:checkpoint_cleanup
# [END_TAGS]

from test_cc01 import test_cc_base
from wiredtiger import stat
from wtscenario import make_scenarios

# test_cc02.py
# Test that in-memory or on-disk obsolete content is removed from the HS.
class test_cc02(test_cc_base):
    # Useful for debugging:
    # conn_config = "verbose=[checkpoint_cleanup:4]"
    cleanup_flows = [
        # Keep everything in memory, obsolete cleanup should mark obsolete data dirty so it is evicted.
        ('eviction', dict(in_memory=True)),
        # Move everything to disk, obsolete content should mark the ref as obsolete.
        ('disk', dict(in_memory=False)),
    ]

    scenarios = make_scenarios(cleanup_flows)

    def test_cc(self):
        # Create a table.
        create_params = 'key_format=i,value_format=S'
        nrows = 1000
        uri = "table:cc02"

        # Create and populate a table.
        self.session.create(uri, create_params)

        old_value = "a"
        old_ts = 1
        self.populate(uri, 0, nrows, old_value, old_ts)

        # Pin oldest and stable to timestamp 1.
        self.conn.set_timestamp(f'oldest_timestamp={self.timestamp_str(old_ts)},stable_timestamp={self.timestamp_str(old_ts)}')

        # Update each record with a newer timestamp.
        new_value = "b"
        new_ts = 10
        self.populate(uri, 0, nrows, new_value, new_ts)

        # Make the updates stable and checkpoint to write everything in the DS and HS.
        # The most recent updates with the ts 10 should be in the DS while the ones with ts 1 should
        # be in the HS.
        self.conn.set_timestamp(f'stable_timestamp={new_ts}')
        self.session.checkpoint()

        if not self.in_memory:
            # Evict everything from the HS.
            session_evict = self.conn.open_session("debug=(release_evict_page=true)")
            session_evict.begin_transaction(f'read_timestamp={self.timestamp_str(old_ts)}')
            evict_cursor = session_evict.open_cursor(uri, None, None)
            for i in range(0, nrows):
                evict_cursor.set_key(i)
                evict_cursor.search()
                # Check we are getting the value from the HS.
                self.assertEqual(evict_cursor.get_value(), old_value)
                evict_cursor.reset()
            session_evict.rollback_transaction()
            evict_cursor.close()

        # Make the updates in the HS obsolete.
        self.conn.set_timestamp(f'oldest_timestamp={new_ts}')

        # Trigger obsolete cleanup.
        # Depending whether the obsolete pages are on disk or in-memory, they should be flagged and
        # discarded.
        self.wait_for_cc_to_run()
        c = self.session.open_cursor('statistics:')
        visited = c[stat.conn.checkpoint_cleanup_pages_visited][2]
        obsolete_evicted = c[stat.conn.checkpoint_cleanup_pages_evict][2]
        obsolete_on_disk = c[stat.conn.checkpoint_cleanup_pages_removed][2]
        c.close()

        # We should always visit pages for cleanup.
        self.assertGreater(visited, 0)

        # Depending on the scenario, cleanup will be triggered differently.
        if self.in_memory:
            self.assertGreater(obsolete_evicted, 0)
            self.assertEqual(obsolete_on_disk, 0)
        else:
            self.assertEqual(obsolete_evicted, 0)
            self.assertGreater(obsolete_on_disk, 0)
