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

import io
import os
import sys
import unittest
from types import SimpleNamespace

# Add tools directory to sys.path so we can import py_common
sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import py_common.btree_format as btree_format
import py_common.binary_data as binary_data
import py_common.mdb_log_parse as log_parse

class Test(unittest.TestCase):
    """Unit tests for decoding a single WT pages using only."""

    def make_opts(self):
        """Mock an opts object used by WTPage/Printer/PageStats."""
        return SimpleNamespace(
            disagg=False,
            skip_data=True,  # only check the headers
            cont=False,
            debug=False,
            # Printer options
            split=False,
            verbose=False,
            ext=False,
            output=None,
        )

    def load_page_bytes(self, opts):
        """Read WiredTiger01.txt and convert its hex dump into raw bytes."""
        cur_dir = os.path.dirname(os.path.abspath(__file__))
        file_path = os.path.join(cur_dir, "binary_files", "WiredTiger01.txt")
        with open(file_path, "r", encoding="utf-8") as f:
            return log_parse.encode_bytes(f, opts)

    def test_wtpage_headers_from_wiredtiger01(self):
        """Decode WiredTiger01.txt and verify page and block header fields."""
        opts = self.make_opts()
        
        page_bytes = self.load_page_bytes(opts)
        self.assertGreater(len(page_bytes), 0, "Encoded page bytes should not be empty")

        b = binary_data.BinaryFile(io.BytesIO(page_bytes))

        page = btree_format.WTPage.parse(b, len(page_bytes), opts)
        self.assertTrue(page.success, "WTPage parsing failed")

        # Validate Page Header fields
        p = page.page_header
        self.assertIsNotNone(p)
        self.assertEqual(p.recno, 0)
        self.assertEqual(p.write_gen, 11)
        self.assertEqual(p.mem_size, 3702)
        self.assertEqual(p.entries, 16)
        self.assertEqual(p.type, btree_format.PageType.WT_PAGE_ROW_LEAF)
        self.assertEqual(p.flags, btree_format.PageFlags.WT_PAGE_EMPTY_V_NONE)
        self.assertEqual(p.version, 1)

        # Validate Block Header fields
        b = page.block_header
        self.assertIsNotNone(b)
        self.assertEqual(b.disk_size, 4096)
        self.assertEqual(b.checksum, 414598985)
        self.assertEqual(b.flags, btree_format.BlockFlags.WT_BLOCK_DATA_CKSUM)


if __name__ == "__main__":
    unittest.main()
