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
from types import SimpleNamespace

# Add tools directory to sys.path so we can import py_common
sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from py_common import page_service

class TestDecodeDisaggTable(unittest.TestCase):
    """Unit tests for decoding a table from disaggregated storage."""

    def make_opts(self) -> SimpleNamespace:
        """Create an opts object required for decoding a disagg table."""
        keyfile = os.environ.get("DISAGG_KEYFILE")
        if not keyfile:
            self.skipTest(
                f"Environment variable DISAGG_KEYFILE must be set to the encryption keyfile path"
            )

        return SimpleNamespace(
            # Disagg / decoding options
            disagg=True,
            disagg_table=True,
            bson=True,
            keyfile=keyfile,
            # General decode options
            skip_data=False,
            cont=False,
            debug=False,
            # Printer options
            split=False,
            verbose=True,
            ext=False,
            output=None,
        )

    def test_decode_disagg_table_bson(self):
        """Decode the disagg table in JSONL format."""
        opts = self.make_opts()
        cur_dir = os.path.dirname(os.path.abspath(__file__))
        table_path = os.path.join(cur_dir, "binary_files", "disagg_oplog.jsonl")

        self.assertTrue(
            os.path.exists(table_path),
            f"Disagg table not found: {table_path}",
        )

        with open(table_path, "r", encoding="utf-8") as disagg_file:
            table_summary = page_service.process_disagg_table(disagg_file, opts)
            
            self.assertEqual(table_summary.delta_pages, 2)
            self.assertEqual(table_summary.full_pages, 6)
            self.assertEqual(table_summary.total_pages, 8)
            print(table_summary)

if __name__ == "__main__":
    unittest.main()
D
