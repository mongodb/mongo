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

# A quick sanity test of an installation via 'pip install wiredtiger'.
# This program only uses the packing API.

import sys
from wiredtiger.packing import unpack, pack

testval = '8281e420f2fa4a8381e40c5855ca808080808080e22fc0e20fc0'
# Jump through hoops to make code work for Py2 + Py3
x = bytes(bytearray.fromhex(testval))

unpacked = unpack('iiiiiiiiiiiiii',x)
unexpect = [2, 1, 552802954, 3, 1, 207123978, 0, 0, 0, 0, 0, 0, 20480, 12288]
#print(str(unpacked)))
if unpacked != unexpect:
    raise Exception('BAD RESULT FOR UNPACK')

packed = pack('iiii', 1, 2, 3, 4)
expect = b'\x81\x82\x83\x84'
#print(str(packed)))
if packed != expect:
    raise Exception('BAD RESULT FOR PACK')

print('testpack success.')
