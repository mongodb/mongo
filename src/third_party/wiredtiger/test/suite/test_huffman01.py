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

from suite_subprocess import suite_subprocess
from wtscenario import make_scenarios
import wiredtiger, wttest

# test_huffman01.py
#    Huffman key and value configurations
# Basic smoke-test of huffman key and value settings.
class test_huffman01(wttest.WiredTigerTestCase, suite_subprocess):
    """
    Test basic operations
    """
    table_name = 'table:test_huff'

    huffval = [
        ('none', dict(huffval=',huffman_value=none',vfile=None)),
        ('english', dict(huffval=',huffman_value=english',vfile=None)),
        ('utf8', dict(huffval=',huffman_value=utf8t8file',vfile='t8file')),
        ('utf16', dict(huffval=',huffman_value=utf16t16file',vfile='t16file')),
    ]
    scenarios = make_scenarios(huffval)

    def test_huffman(self):
        dir = self.conn.get_home()
        # if self.vfile != None and not os.path.exists(self.vfile):
        if self.vfile != None:
            f = open(dir + '/' + self.vfile, 'w')
            # For the UTF settings write some made-up frequency information.
            f.write('48 546233\n49 460946\n')
            f.write('0x4a 546233\n0x4b 460946\n')
            f.close()
        config= self.huffval
        self.session.create(self.table_name, config)

# Test Huffman encoding ranges.
class test_huffman_range(wttest.WiredTigerTestCase):
    table_name = 'table:test_huff'

    # Test UTF8 out-of-range symbol information.
    def test_huffman_range_symbol_utf8(self):
        dir = self.conn.get_home()
        f = open(dir + '/t8file', 'w')
        f.write('256 546233\n257 460946\n')
        f.close()
        config="huffman_value=utf8t8file"
        msg = '/not in range/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.create(self.table_name, config), msg)

    # Test UTF16 out-of-range symbol information.
    def test_huffman_range_symbol_utf16(self):
        dir = self.conn.get_home()
        f = open(dir + '/t16file', 'w')
        f.write('65536 546233\n65537 460946\n')
        f.close()
        config="huffman_value=utf16t16file"
        msg = '/not in range/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.create(self.table_name, config), msg)

    # Test out-of-range frequency information.
    def test_huffman_range_frequency(self):
        # Write out-of-range frequency information.
        dir = self.conn.get_home()
        f = open(dir + '/t8file', 'w')
        f.write('48 4294967296\n49 4294967297\n')
        f.close()
        config="huffman_value=utf8t8file"
        msg = '/not in range/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.create(self.table_name, config), msg)

    # Test duplicate symbol information.
    def test_huffman_range_symbol_dup(self):
        dir = self.conn.get_home()
        f = open(dir + '/t8file', 'w')
        f.write('100 546233\n101 460946\n')
        f.write('102 546233\n100 460946\n')
        f.close()
        config="huffman_value=utf8t8file"
        msg = '/duplicate symbol/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.create(self.table_name, config), msg)

if __name__ == '__main__':
    wttest.run()
