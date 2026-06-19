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

import re, wttest, wiredtiger
from helper_disagg import disagg_test_class
from helper import simulate_crash_restart

# Test the database-level size stored in the checkpoint completion record.
@disagg_test_class
class test_disagg_checkpoint_size02(wttest.WiredTigerTestCase):
    conn_config = 'disaggregated=(role="leader",lose_all_my_data=true)'
    disagg_size_buffer = 1024 * 1024

    def get_database_size(self):
        match = re.search(r'database_size=(\d+)', self.disagg_get_complete_checkpoint_meta())
        assert(match)
        return int(match.group(1))

    # Test that a brand new database cannot have its checkpoint metadata read. Create a table and
    # checkpoint and validate that the size goes up.
    def test_new_database(self):
        # With an empty database attempt to read the checkpoint metadata. This will fail as no
        # checkpoint has completed yet.
        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: self.disagg_get_complete_checkpoint_meta())

        # Create a table to ensure metadata gets written
        uri = "layered:test_table"
        self.session.create(uri, 'key_format=i,value_format=S')
        # Take a checkpoint first - this will write the initial checkpoint metadata to palite
        self.session.checkpoint()
        # With no actual data, the size should be minimal. The only data that has actually been
        # written to SLS at this stage is the root page, and leaf page of the shared metadata file.
        # It should also include the size of the 1MB buffer for new databases.
        self.assertGreater(self.get_database_size(), self.disagg_size_buffer, "Database size should be non-zero")

    # Test that the database size increases as data is added and checkpoints are taken.
    def test_database_size_increases(self):
        uri = "layered:test_table"
        self.session.create(uri, 'key_format=i,value_format=S')

        # Take initial checkpoint.
        self.session.checkpoint()
        initial_size = self.get_database_size()

        # Insert data.
        cursor = self.session.open_cursor(uri)
        for i in range(1000):
            cursor[i] = 'a' * 100
        cursor.close()

        # Take checkpoint
        self.session.checkpoint()
        next_size = self.get_database_size()

        # Size should have increased
        self.assertGreater(next_size, initial_size,
            f"Database size should increase after insert: {initial_size} -> {next_size}")

        # Insert more data
        cursor = self.session.open_cursor(uri)
        for i in range(1000, 3000):
            cursor[i] = 'b' * 100
        cursor.close()

        # Take another checkpoint
        self.session.checkpoint()
        even_more = self.get_database_size()
        # Size should have increased again
        self.assertGreater(even_more, next_size,
            f"Database size should increase after more inserts: {next_size} -> {even_more}")

    # Test that the database size decreases as data is removed and checkpoints are taken.
    def test_database_size_decreases(self):
        uri = "layered:test_table"
        self.session.create(uri, 'key_format=i,value_format=S')

        # Take initial checkpoint.
        self.session.checkpoint()
        initial_size = self.get_database_size()

        # Insert data.
        cursor = self.session.open_cursor(uri)
        for i in range(1000):
            cursor[i] = 'a' * 100
        cursor.close()

        self.session.checkpoint()
        size_with_data = self.get_database_size()

        # Size should have increased
        self.assertGreater(size_with_data, initial_size,
            f"Database size should increase after insert: {initial_size} -> {size_with_data}")

        # Truncate most data
        self.session.begin_transaction()
        cursor = self.session.open_cursor(uri)
        for i in range(100, 1000):
            cursor.set_key(i)
            cursor.remove()
        cursor.close()
        self.session.commit_transaction()

        # Checkpoint after truncation
        self.session.checkpoint()
        size_after_truncate = self.get_database_size()

        # Size should have decreased but still include buffer
        self.assertLess(size_after_truncate, size_with_data,
            f"Database size should decrease after truncate: {size_with_data} -> {size_after_truncate}")
        self.assertGreaterEqual(size_after_truncate, self.disagg_size_buffer,
            f"Size should still include buffer: {size_after_truncate} >= {self.disagg_size_buffer}")

    def test_database_size_multiple_btrees(self):
        uri1 = "layered:test1"
        uri2 = "layered:test2"
        uri3 = "layered:test3"

        self.session.create(uri1, 'key_format=i,value_format=S')
        self.session.create(uri2, 'key_format=i,value_format=S')
        self.session.create(uri3, 'key_format=i,value_format=S')

        # Take initial checkpoint.
        self.session.checkpoint()
        initial_size = self.get_database_size()

        # Insert data into first table.
        cursor = self.session.open_cursor(uri1)
        for i in range(500):
            cursor[i] = 'a' * 100
        cursor.close()

        self.session.checkpoint()
        size_after_t1 = self.get_database_size()
        delta1 = size_after_t1 - initial_size

        # Insert data into second table.
        cursor = self.session.open_cursor(uri2)
        for i in range(500):
            cursor[i] = 'b' * 100
        cursor.close()

        self.session.checkpoint()
        size_after_t2 = self.get_database_size()
        delta2 = size_after_t2 - size_after_t1

        # Insert data into third table.
        cursor = self.session.open_cursor(uri3)
        for i in range(500):
            cursor[i] = 'c' * 100
        cursor.close()

        self.session.checkpoint()
        size_after_t3 = self.get_database_size()
        delta3 = size_after_t3 - size_after_t2

        # Each table insertion should have increased the database size.
        self.assertGreater(delta1, 0)
        self.assertGreater(delta2, 0)
        self.assertGreater(delta3, 0)

        # The deltas should be similar since we inserted similar amounts, allow 10% variance.
        avg_delta = (delta1 + delta2 + delta3) / 3
        for delta in [delta1, delta2, delta3]:
            self.assertGreater(delta, avg_delta * 0.9)
            self.assertLess(delta, avg_delta * 1.1)

    def test_database_size_persists_across_restart(self):
        uri = "layered:test_table"
        self.session.create(uri, 'key_format=i,value_format=S')

        # Insert data.
        cursor = self.session.open_cursor(uri)
        for i in range(1000):
            cursor[i] = 'a' * 100
        cursor.close()

        self.session.checkpoint()
        size_before_restart = self.get_database_size()

        # Reopen the connection
        with self.expectedStdoutPattern("Removing local file"):
            self.reopen_conn()

        # Check the size after restart
        size_after_restart = self.get_database_size()

        # The size should be preserved but isn't guaranteed to be equal due to the checkpoint on
        # shutdown and restart, allow for 10% variance.
        self.assertAlmostEqual(size_before_restart, size_after_restart, delta=size_before_restart * 0.1)

    def test_failed_checkpoint_no_size_change(self):
        uri = "layered:test_table"
        self.session.create(uri, 'key_format=i,value_format=S')

        # Insert data.
        cursor = self.session.open_cursor(uri)
        for i in range(1000):
            cursor[i] = 'a' * 100
        cursor.close()

        self.session.checkpoint()
        size_before_crash = self.get_database_size()

        # Insert more data
        cursor = self.session.open_cursor(uri)
        for i in range(100, 200):
            cursor[i] = 'b' * 100
        cursor.close()

        with self.expectedStdoutPattern("Removing local file"):
            simulate_crash_restart(self, ".", "RESTART")

        # Check size after crash. It should be the same since checkpoint didn't complete.
        size_after_crash = self.get_database_size()
        self.assertEqual(size_after_crash, size_before_crash)
