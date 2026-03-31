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

import os
import sys
import unittest

# Add tools directory to sys.path so we can import py_common
sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from py_common import binary_data, btree_format

class TestDecodeDeltaPage(unittest.TestCase):
    """Unit tests for decoding a delta page from disaggregated storage."""

    def test_decode_disagg_delta_page(self):
        """Decode delta page from MongoDB oplog."""
        cur_dir = os.path.dirname(os.path.abspath(__file__))
        page_path = os.path.join(cur_dir, "binary_files", "disagg_delta_oplog.bin")

        self.assertTrue(
            os.path.exists(page_path),
            f"Page binary not found: {page_path}",
        )

        with open(page_path, "rb") as disagg_file:
            b = binary_data.BinaryFile(disagg_file)
            nbytes = os.path.getsize(page_path)

            page = btree_format.WTPage.parse(b, nbytes, disagg=True)

            page.print_page(decode_as_bson=True, disagg=True)

            # Verify the parse succeeded and we can inspect the page
            self.assertTrue(getattr(page, 'success', True), 'WTPage.parse failed')

            # Check page header fields (from expected output)
            page_header = page.page_header
            self.assertEqual(page_header.recno, 0)
            self.assertEqual(page_header.write_gen, 6)
            self.assertEqual(page_header.mem_size, 2268)
            self.assertEqual(page_header.entries, 10)
            self.assertEqual(page_header.type.name, 'WT_PAGE_ROW_LEAF')
            self.assertEqual(int(page_header.flags), 0)
            self.assertEqual(page_header.version, 0)

            # Check block disagg header
            block_header = page.block_header
            self.assertEqual(block_header.magic, btree_format.BlockDisaggHeader.WT_BLOCK_DISAGG_MAGIC_DELTA)
            self.assertEqual(block_header.version, 1)
            self.assertEqual(block_header.compatible_version, 1)
            self.assertEqual(block_header.header_size, 44)
            self.assertEqual(block_header.checksum, 2836552602)
            self.assertEqual(block_header.previous_checksum, 4000115340)
            self.assertTrue(block_header.flags & btree_format.BlockDisaggFlags.WT_BLOCK_DISAGG_DATA_CKSUM)

            # There should be ten cells
            self.assertEqual(len(page.cells), 10)

            # First cell: short key 9 bytes, packed 64-bit value which encodes timestamp
            c0 = page.cells[0]
            self.assertTrue(c0.is_key)

            # Second cell: value cell with timestamps and BSON payload
            c1 = page.cells[1]
            self.assertTrue(c1.is_value)
            self.assertEqual(len(c1.data), 419)
            # Check timestamps
            self.assertIsNotNone(c1.start_ts)
            self.assertEqual(c1.start_ts, 7613589195910545415)


if __name__ == "__main__":
    unittest.main()
