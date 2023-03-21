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
# wt_util
# [END_TAGS]

import wttest

from suite_subprocess import suite_subprocess

# test_pretty_hex_dump
#    Utilities: wt dump
# Test the dump utility with pretty hex flag
class test_pretty_hex_dump(wttest.WiredTigerTestCase, suite_subprocess):
    table_format = 'key_format=i,value_format=u'
    uri = 'table:test_dump'

    pretty_dump_file = 'pretty_dump.out'
    pretty_hex_dump_file = 'pretty_hex_dump.out'
    hex_dump_file = 'hex.out'

    pretty_hex_format = 'Format=print hex\n'
    data_header = 'Data\n'

    def get_bytes(self, i, len):
        """
        Return a pseudo-random, but predictable string that uses all characters
        """
        ret = b''
        for j in range(0, len // 3):
            k = i + j
            ret += bytes([k%255 + 1, (k*3)%255 + 1, (k*7)%255 + 1])
        return ret + bytes([0])   # Add a final null byte

    def populate_table(self, uri):
        """
        Populate test_dump table
        """
        cursor = self.session.open_cursor(uri, None, None)
        for i in range(2, 15):
            val_bytes = self.get_bytes(i*i, i*i)
            cursor[i] = (val_bytes)
        cursor.close()

    def test_dump(self):
        """
        Generates test table with byte array values. After that dumps the table in hex, pretty and
        pretty hex formats. And then validates pretty hex dump.
        """
        # Create three dumps: hex, pretty and pretty_hex
        self.session.create(self.uri, self.table_format)
        self.populate_table(self.uri)

        self.runWt(['dump', '-x', self.uri], outfilename=self.hex_dump_file)
        self.runWt(['dump', '-p', self.uri], outfilename=self.pretty_dump_file)
        self.runWt(['dump', '-px', self.uri], outfilename=self.pretty_hex_dump_file)

        # Validate pretty hex
        hex = open(self.hex_dump_file).readlines()
        pretty = open(self.pretty_dump_file).readlines()
        pretty_hex = open(self.pretty_hex_dump_file).readlines()

        # First validate number of lines the dumps
        self.assertEqual(True, len(pretty) == len(pretty_hex),
            'Pretty and pretty_hex output must have the same number of lines.')
        self.assertEqual(True, len(hex) == len(pretty_hex),
            'Hex and pretty_hex output must have the same number of lines.')

        # Next analyse the pretty hex dump line by line
        data_started = False
        value_line = False
        for h, p, px in zip(hex, pretty, pretty_hex):
            if data_started:
                # Data section started
                if value_line:
                    # Test values
                    self.assertEqual(True, h == px,
                        'Hex and pretty_hex values must match!\n' + 'Hex: ' + h + 'Pretty_hex: ' + px)
                else:
                    # Test keys
                    self.assertEqual(True, p == px,
                        'Pretty and pretty_hex keys must match!\n' + 'Pretty: ' + p + 'Pretty_hex: ' + px)

                value_line = not value_line
            else:
                if p.startswith('Format='):
                    # The two dumps must have appropriate format string
                    self.assertEqual(True, px == self.pretty_hex_format,
                        'Format for pretty_hex dump is: ' + px + ' Expected: ' + self.pretty_hex_format)
                else:
                    # The two dump lines must match
                    self.assertEqual(True, p == px,
                        'Dump lines differ!\n' + 'Pretty: ' + p + ', Pretty_hex: ' + px)

            # Test if data section has started
            if not data_started and p == self.data_header:
                self.assertEqual(True, p == px,
                    'Data section starts at different lines.\n' + 'Pretty: ' + p + 'Pretty_hex: ' + px)
                self.assertEqual(True, h == px,
                    'Data section starts at different lines.\n' + 'Hex: ' + h + 'Pretty_hex: ' + px)
                data_started = True

if __name__ == '__main__':
    wttest.run()
