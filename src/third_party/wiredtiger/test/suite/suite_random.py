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

# suite_random.py
#    A quick and predictable pseudo random number generator.
import wttest

class suite_random:
    """
    Generate random 32 bit integers that are predictable,
    and use no global state.  We use the Multiply-with-carry
    method invented by George Marsaglia, because it is quick
    and easy to implement.
    """
    def __init__(self, *args):
        arglen = len(args)
        seedw, seedz = wttest.getseed()
        if arglen == 1:
            self.seedw = int(args[0]) & 0xffffffff
            self.seedz = int(args[0]) & 0xffffffff
        elif arglen == 2:
            self.seedw = int(args[0]) & 0xffffffff
            self.seedz = int(args[1]) & 0xffffffff
        else:
            self.seedw = int(seedw) & 0xffffffff
            self.seedz = int(seedz) & 0xffffffff

    def rand32(self):
        """
        returns a random 32 bit integer
        """
        w = self.seedw
        z = self.seedz
        if w == 0 or z == 0:
            seedw, seedz = wttest.getRandomSeed()
            self.seedw = int(seedw) & 0xffffffff
            self.seedz = int(seedz) & 0xffffffff

        self.seedz = (36969 * (z & 65535) + (z >> 16)) & 0xffffffff
        self.seedw = (18000 * (w & 65535) + (w >> 16)) & 0xffffffff
        return ((z << 16) + (w & 65535)) & 0xffffffff

    def rand_range(self, n, m):
        """
        returns a random integer in the range [N,M).
        """
        if m > 0xffffffff or n < 0:
            raise ValueError("rand32_range expects args between 0 , 2^32")
        if n >= m:
            raise ValueError("rand32_range(n,m) expects n < m")
        r = self.rand32()
        return (r % (m - n)) + n

    def rand_float(self):
        """
        returns a random floating point value between 0 and 1.0
        The number returned does not encompass all possibilities,
        only 2^32 values within the range.
        """
        return (self.rand32() + 0.0)/0x100000000
