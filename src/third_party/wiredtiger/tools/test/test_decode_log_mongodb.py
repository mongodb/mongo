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
import contextlib
import io
import logging
import os
import sys
import unittest

# Add tools directory to sys.path so we can import wt_binary_decode
sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import wt_binary_decode
from py_common.decode_opts import DecodeOptions


class TestDecodeMongoDBLog(unittest.TestCase):
    """Unit tests for decoding hex dumps from MongoDB logs."""

    def setUp(self):
        self.cur_dir = os.path.dirname(os.path.abspath(__file__))
        self.binary_files_dir = os.path.join(self.cur_dir, "binary_files")

    def run_decode(self, filename):
        log_path = os.path.join(self.binary_files_dir, filename)
        self.assertTrue(os.path.exists(log_path), f"Missing log file at {log_path}")

        buffer = io.StringIO()
        with self.assertLogs("py_common.mdb_log_parse", level=logging.INFO) as logs:
            with contextlib.redirect_stdout(buffer):
                wt_binary_decode.wtdecode(log_path, DecodeOptions(dumpin=True))
        return buffer.getvalue(), "\n".join(logs.output)

    def test_decode_log_mongodb_valid(self):
        self.skipTest("FIXME-WT-16726 Test for valid MongoDB log")

    def test_decode_log_mongodb_multi_chunk_valid(self):
        self.skipTest("FIXME-WT-16726 Test for valid multi-chunk MongoDB log")

    def test_decode_log_mongodb_incomplete_chunks(self):
        self.skipTest("FIXME-WT-16726 Test for incomplete chunks in MongoDB log")

    def test_decode_log_mongodb_no_checksum_mismatch(self):
        self.skipTest("FIXME-WT-16726 Test for no checksum mismatch in MongoDB log")

    def test_decode_log_mongodb_non_hex_characters(self):
        _, logged_output = self.run_decode("mongodb_non_hex.log")

        # Should print error message about corrupt hex dump
        self.assertIn("Hex dump is corrupt", logged_output)
        self.assertIn("Non-hex characters found", logged_output)
        self.assertIn("No valid byte dump found in MongoDB log", logged_output)

    def test_decode_log_mongodb_odd_length_hex(self):
        _, logged_output = self.run_decode("mongodb_odd_length.log")

        # Should print error message about odd length
        self.assertIn("Hex dump is corrupt", logged_output)
        self.assertIn("Hex data chunk length is not even", logged_output)
        self.assertIn("No valid byte dump found in MongoDB log", logged_output)

    def test_decode_log_mongodb_block_size_mismatch(self):
        _, logged_output = self.run_decode("mongodb_size_mismatch.log")

        # Should print error message about size mismatch
        self.assertIn("Hex dump is corrupt", logged_output)
        self.assertIn("Block size mismatch", logged_output)
        self.assertIn("No valid byte dump found in MongoDB log", logged_output)

if __name__ == "__main__":
    unittest.main()
