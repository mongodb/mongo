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
# WiredTiger packing and unpacking utility functions and constants

import sys

# In the Python3 world, we pack into the bytes type, which like a list of ints.
# In Python2, we pack into a string.  Create a set of constants and methods
# to hide the differences from the main code.

# all bits on or off, expressed as a bytes type
x00 = b'\x00'
xff = b'\xff'
x00_entry = x00[0]
xff_entry = xff[0]
empty_pack = b''

_python3 = (sys.version_info >= (3, 0, 0))
if _python3:
    def _ord(b):
        return b

    def _chr(x, y=None):
        a = [x]
        if y != None:
            a.append(y)
        return bytes(a)

    def _is_string(s):
        return type(s) is str

    def _string_result(s):
        return s.decode()

else:
    def _ord(b):
        return ord(b)

    def _chr(x, y=None):
        s = chr(x)
        if y != None:
            s += chr(y)
        return s

    def _is_string(s):
        return type(s) is unicode

    def _string_result(s):
        return s
