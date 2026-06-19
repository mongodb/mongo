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
from helper_disagg import DisaggConfigMixin, disagg_test_class

# Test that the checkpoint size field is stored to the metadata for stable tables.
@disagg_test_class
class test_disagg_checkpoint_size(wttest.WiredTigerTestCase):

    test_name = __qualname__
    uri_base = test_name
    conn_config = 'disaggregated=(role="leader"),disaggregated=(lose_all_my_data=true)'
    uri = "layered:" + uri_base

    def conn_extensions(self, extlist):
        extlist.skip_if_missing = True
        extlist.extension('compressors', 'zstd')
        DisaggConfigMixin.conn_extensions(self, extlist)  # Load disagg extensions

    # Find all the size values in the checkpoint metadata string. Return the latest.
    def find_checkpoint_size(self, metadata_value):
        sizes = re.findall(r',size=(\d+),', metadata_value)
        assert(len(sizes) > 0)
        return int(sizes[-1])

    # Insert data without compression and validate that the checkpoint size is greater than the
    # amount of data inserted.
    def test_checkpoint_size_populated_non_compressed(self):
        # Create a layered table.
        self.session.create(self.uri, 'key_format=S,value_format=S')

        # Insert some data.
        cursor = self.session.open_cursor(self.uri)
        nrows = 1000
        value_size = 100  # Each value is 100 bytes.
        for i in range(nrows):
            value = 'x' * value_size
            cursor[str(i)] = value
        cursor.close()

        # Take a checkpoint to persist the data.
        self.session.checkpoint()

        # Open metadata cursor to read the stable btree's metadata.
        stable_uri = f'file:{self.uri_base}.wt_stable'
        meta_cursor = self.session.open_cursor('metadata:')
        meta_cursor.set_key(stable_uri)
        ret = meta_cursor.search()
        self.assertEqual(ret, 0, f"Failed to find metadata for {stable_uri}")

        # The last checkpoint should have a size >100000 (nrows * value_size) as compression is not
        # enabled.
        size = self.find_checkpoint_size(meta_cursor.get_value())
        self.assertGreater(size, 100000,
            f"Checkpoint size should be greater than 100000, got {size}")
        meta_cursor.close()

    # Insert data with compression enabled and validate that the checkpoint size is smaller than the
    # above test case's size.
    def test_checkpoint_size_populated_compressed(self):
        # Create a layered table.
        self.session.create(self.uri, 'key_format=S,value_format=S,block_compressor=zstd')


        # Insert some data.
        cursor = self.session.open_cursor(self.uri)
        nrows = 1000
        value_size = 100  # Each value is 100 bytes.
        for i in range(nrows):
            value = 'x' * value_size
            cursor[str(i)] = value
        cursor.close()

        # Take a checkpoint to persist the data.
        self.session.checkpoint()

        # Open metadata cursor to read the stable btree's metadata.
        stable_uri = f'file:{self.uri_base}.wt_stable'
        meta_cursor = self.session.open_cursor('metadata:')
        meta_cursor.set_key(stable_uri)
        ret = meta_cursor.search()
        self.assertEqual(ret, 0, f"Failed to find metadata for {stable_uri}")

        # The last checkpoint should have a size < 100000 (nrows * value_size).
        size = self.find_checkpoint_size(meta_cursor.get_value())
        self.assertLess(size, 100000,
            f"Checkpoint size should be less than 100000, got {size}")
        meta_cursor.close()

    # Test that checkpoint size increases after adding data and taking another checkpoint.
    def test_checkpoint_size_increases(self):
        # Create a layered table.
        self.session.create(self.uri, 'key_format=S,value_format=S')

        # Insert some initial data.
        cursor = self.session.open_cursor(self.uri)
        for i in range(500):
            cursor[str(i)] = 'x' * 100
        cursor.close()

        # Take a checkpoint.
        self.session.checkpoint()

        # Get the checkpoint size after first batch.
        stable_uri = f'file:{self.uri_base}.wt_stable'
        meta_cursor = self.session.open_cursor('metadata:')
        meta_cursor.set_key(stable_uri)
        ret = meta_cursor.search()
        self.assertEqual(ret, 0, f"Failed to find metadata for {stable_uri}")
        first_size = self.find_checkpoint_size(meta_cursor.get_value())
        meta_cursor.close()

        # Insert more data
        cursor = self.session.open_cursor(self.uri)
        for i in range(500, 1500):
            cursor[str(i)] = 'y' * 100
        cursor.close()

        # Take another checkpoint.
        self.session.checkpoint()

        # Get the checkpoint size after second batch.
        meta_cursor = self.session.open_cursor('metadata:')
        meta_cursor.set_key(stable_uri)
        meta_cursor.search()
        second_size = self.find_checkpoint_size(meta_cursor.get_value())

        # The size should have increased.
        self.assertGreater(second_size, first_size,
            f"Checkpoint size should increase: {first_size} -> {second_size}")
        meta_cursor.close()

    # Test that the checkpoint size is preserved across database restart.
    def test_checkpoint_size_persists_across_restart(self):
        # Create a layered table and insert data.
        self.session.create(self.uri, 'key_format=S,value_format=S')

        cursor = self.session.open_cursor(self.uri)
        for i in range(1000):
            cursor[f'key{i:06d}'] = 'x' * 100
        cursor.close()

        # Take a checkpoint.
        self.session.checkpoint()

        # Get the checkpoint size before restart.
        stable_uri = f'file:{self.uri_base}.wt_stable'
        meta_cursor = self.session.open_cursor('metadata:')
        meta_cursor.set_key(stable_uri)
        ret = meta_cursor.search()
        self.assertEqual(ret, 0, f"Failed to find metadata for {stable_uri}")
        size_before = self.find_checkpoint_size(meta_cursor.get_value())
        meta_cursor.close()

        # Reopen the connection.
        with self.expectedStdoutPattern("Removing local file"):
            self.reopen_conn()

        # Get the checkpoint size after restart.
        meta_cursor = self.session.open_cursor('metadata:')
        meta_cursor.set_key(stable_uri)
        ret = meta_cursor.search()
        self.assertEqual(ret, 0, f"Failed to find metadata for {stable_uri}")
        size_after = self.find_checkpoint_size(meta_cursor.get_value())
        meta_cursor.close()

        # The sizes should match.
        self.assertEqual(size_before, size_after,
            f"Checkpoint size should persist: before={size_before}, after={size_after}")
