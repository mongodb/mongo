#!/usr/bin/env python
#
# Public Domain 2014-2016 MongoDB, Inc.
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
from suite_subprocess import suite_subprocess
from wtscenario import multiply_scenarios, number_scenarios
import wiredtiger, wttest

# test_huffman02.py
#    Huffman key and value configurations test.
class test_huffman02(wttest.WiredTigerTestCase, suite_subprocess):
    huffkey = [
        ('bad', dict(keybad=1,huffkey=',huffman_key=bad')),
        ('english', dict(keybad=0,huffkey=',huffman_key=english')),
        ('none', dict(keybad=0,huffkey=',huffman_key=none')),
    ]
    huffval = [
        ('bad', dict(valbad=1,huffval=',huffman_value=bad')),
        ('english', dict(valbad=0,huffval=',huffman_value=english')),
        ('none', dict(valbad=0,huffval=',huffman_value=english')),
    ]
    type = [
        ('file', dict(uri='file:huff')),
        ('table', dict(uri='table:huff')),
    ]
    scenarios = number_scenarios(multiply_scenarios('.',type,huffkey, huffval))

    def test_huffman(self):
        if self.keybad or self.valbad:
            msg = '/Invalid argument/'
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
                self.session.create(self.uri, self.huffkey + self.huffval), msg)
        else:
            self.session.create(self.uri, self.huffkey + self.huffval)

if __name__ == '__main__':
    wttest.run()
