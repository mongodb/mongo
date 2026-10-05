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

from py_common import btree_format


class TestDecodeHSKey(unittest.TestCase):
    """Decoding history store keys (WT_HS_KEY_FORMAT)."""

    # btree_id=16, key="key0001\0", hs_start_ts=1, hs_counter=0, from a real WiredTigerHS.wt.
    HS_KEY_BYTES = bytes.fromhex('90 88 6b 65 79 30 30 30 31 00 81 80'.replace(' ', ''))

    def test_parse(self):
        """The key and its byte-aligned rendering decode from the encoded bytes."""
        hs_key = btree_format.HSKey.parse(self.HS_KEY_BYTES)
        self.assertEqual((hs_key.btree_id, hs_key.key, hs_key.hs_start_ts, hs_key.hs_counter),
                         (16, b'key0001\x00', 1, 0))
        width = len('88 6b 65 79 30 30 30 31 00')
        self.assertEqual(hs_key.lines(), [
            'hs key:',
            f'{"90":<{width}}  btree_id=16',
            f'{"88 6b 65 79 30 30 30 31 00":<{width}}  key="key0001"',
            f'{"81":<{width}}  hs_start_ts=0x1',
            f'{"80":<{width}}  hs_counter=0',
        ])

    def test_rejects_non_key_bytes(self):
        """Data store keys and truncated keys do not decode."""
        self.assertIsNone(btree_format.HSKey.parse(b'key0001 '))
        self.assertIsNone(btree_format.HSKey.parse(b''))
        self.assertIsNone(btree_format.HSKey.parse(self.HS_KEY_BYTES[:-2]))

    def test_non_printable_key_uses_hex(self):
        """A key with non-printable bytes falls back to hex."""
        self.assertEqual(btree_format.HSKey(4, b'\x00\xff\x10', 5, 1).key_string(), '00 ff 10')


if __name__ == "__main__":
    unittest.main()
