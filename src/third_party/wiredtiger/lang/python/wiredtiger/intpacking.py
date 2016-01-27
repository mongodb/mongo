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
#

import math, struct

# Variable-length integer packing
# need: up to 64 bits, both signed and unsigned
#
# Try hard for small values (up to ~2 bytes), after that, just encode the
# length in the first byte.
#
#  First byte | Next |                        |
#  byte       | bytes| Min Value              | Max Value
# ------------+------+------------------------+--------------------------------
# [00 00xxxx] | free | N/A                    | N/A
# [00 01llll] | 8-l  | -2^64                  | -2^13 - 2^6
# [00 1xxxxx] | 1    | -2^13 - 2^6            | -2^6 - 1 
# [01 xxxxxx] | 0    | -2^6                   | -1
# [10 xxxxxx] | 0    | 0                      | 2^6 - 1 
# [11 0xxxxx] | 1    | 2^6                    | 2^13 + 2^6 - 1
# [11 10llll] | l    | 2^14 + 2^7             | 2^64 - 1
# [11 11xxxx] | free | N/A                    | N/A

NEG_MULTI_MARKER = 0x10
NEG_2BYTE_MARKER = 0x20
NEG_1BYTE_MARKER = 0x40
POS_1BYTE_MARKER = 0x80
POS_2BYTE_MARKER = 0xc0
POS_MULTI_MARKER = 0xe0

NEG_1BYTE_MIN = -2**6
NEG_2BYTE_MIN = -2**13 + NEG_1BYTE_MIN
POS_1BYTE_MAX = 2**6 - 1
POS_2BYTE_MAX = 2**13 + POS_1BYTE_MAX

MINUS_BIT = -1 << 64
UINT64_MASK = 0xffffffffffffffff

def getbits(x, start, end=0):
    '''return the least significant bits of x, from start to end'''
    return (x & ((1 << start) - 1)) >> (end)

def get_int(b, size):
    r = 0
    for i in xrange(size):
        r = (r << 8) | ord(b[i])
    return r

def pack_int(x):
    if x < NEG_2BYTE_MIN:
        packed = struct.pack('>Q', x & UINT64_MASK)
        while packed and packed[0] == '\xff':
            packed = packed[1:]
        return chr(NEG_MULTI_MARKER | getbits(8 - len(packed), 4)) + packed
    elif x < NEG_1BYTE_MIN:
        x -= NEG_2BYTE_MIN
        return chr(NEG_2BYTE_MARKER | getbits(x, 13, 8)) + chr(getbits(x, 8))
    elif x < 0:
        x -= NEG_1BYTE_MIN
        return chr(NEG_1BYTE_MARKER | getbits(x, 6))
    elif x <= POS_1BYTE_MAX:
        return chr(POS_1BYTE_MARKER | getbits(x, 6))
    elif x <= POS_2BYTE_MAX:
        x -= (POS_1BYTE_MAX + 1)
        return chr(POS_2BYTE_MARKER | getbits(x, 13, 8)) + chr(getbits(x, 8))
    elif x == POS_2BYTE_MAX + 1:
        # This is a special case where we could store the value with
        # just a single byte, but we append a zero byte so that the
        # encoding doesn't get shorter for this one value.
        return chr(POS_MULTI_MARKER | 0x1) + chr(0)
    else:
        packed = struct.pack('>Q', x - (POS_2BYTE_MAX + 1))
        while packed and packed[0] == '\x00':
            packed = packed[1:]
        return chr(POS_MULTI_MARKER | getbits(len(packed), 4)) + packed

def unpack_int(b):
    marker = ord(b[0])
    if marker < NEG_2BYTE_MARKER:
        sz = 8 - getbits(marker, 4)
        return ((-1 << (sz << 3)) | get_int(b[1:], sz), b[sz+1:])
    elif marker < NEG_1BYTE_MARKER:
        return (NEG_2BYTE_MIN + ((getbits(marker, 5) << 8) | ord(b[1])), b[2:])
    elif marker < POS_1BYTE_MARKER:
        return (NEG_1BYTE_MIN + getbits(marker, 6), b[1:])
    elif marker < POS_2BYTE_MARKER:
        return (getbits(marker, 6), b[1:])
    elif marker < POS_MULTI_MARKER:
        return (POS_1BYTE_MAX + 1 +
               ((getbits(marker, 5) << 8) | ord(b[1])), b[2:])
    else:
        sz = getbits(marker, 4)
        return (POS_2BYTE_MAX + 1 + get_int(b[1:], sz), b[sz+1:])

# Sanity testing
if __name__ == '__main__':
    import random

    for big in (100, 10000, 1 << 40, 1 << 64):
        for i in xrange(1000):
            r = random.randint(-big, big)
            print "\rChecking %d" % r,
            if unpack_int(pack_int(r))[0] != r:
                print "\nFound a problem with %d" % r
                break

        print

        for i in xrange(1000):
            r1 = random.randint(-big, big)
            r2  = random.randint(-big, big)
            print "\rChecking %d, %d" % (r1, r2),
            if cmp(r1, r2) != cmp(pack_int(r1), pack_int(r2)):
                print "\nFound a problem with %d, %d" % (r1, r2)
                break

        print
