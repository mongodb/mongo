#!/usr/bin/env python3
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

# Functions for reading and interpreting basic data types.

import typing


def get_bits(x: int, start: int, end: int = 0) -> int:
    '''
    Return the least significant bits of x, from start to end.
    '''
    return (x & ((1 << start) - 1)) >> (end)


def get_int(b: bytes, size: int) -> int:
    '''
    Convert a binary string to an int.
    '''
    r = 0
    for i in range(size):
        r = (r << 8) | b[i]
    return r


def unpack_int(b: bytes) -> tuple[int, bytes]:
    '''
    Unpack an encoded integer from the given binary string. Return the tuple (unpacked integer, the
    rest of the binary array).
    '''
    # Borrowed from intpacking.py, with small adjustments.
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
    # [11 10llll] | l    | 2^13 + 2^6             | 2^64 - 1
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

    marker = b[0]
    if marker < NEG_MULTI_MARKER or marker >= 0xf0:
        raise ValueError('Not a packed integer')
    elif marker < NEG_2BYTE_MARKER:
        sz = 8 - get_bits(marker, 4)
        if sz < 0:
            raise ValueError('Not a valid packed integer')
        part1 = (-1 << (sz << 3))
        part2 = get_int(b[1:], sz)
        part3 = b[sz+1:]
        return (part1 | part2, part3)
    elif marker < NEG_1BYTE_MARKER:
        return (NEG_2BYTE_MIN + ((get_bits(marker, 5) << 8) | b[1]), b[2:])
    elif marker < POS_1BYTE_MARKER:
        return (NEG_1BYTE_MIN + get_bits(marker, 6), b[1:])
    elif marker < POS_2BYTE_MARKER:
        return (get_bits(marker, 6), b[1:])
    elif marker < POS_MULTI_MARKER:
        return (POS_1BYTE_MAX + 1 +
               ((get_bits(marker, 5) << 8) | b[1]), b[2:])
    else:
        sz = get_bits(marker, 4)
        return (POS_2BYTE_MAX + 1 + get_int(b[1:], sz), b[sz+1:])


def decode_esc_hex(s: str) -> bytes:
    '''
    Decode a string produced by __wt_raw_to_esc_hex.
    '''
    b = bytearray()
    i = 0
    while i < len(s):
        if s[i] == '\\':
            if i + 3 > len(s):
                raise ValueError('Not a valid escaped hex byte')
            b.append(int(s[i + 1] + s[i + 2], 16))
            i += 3
        else:
            b.append(ord(s[i]))
            i += 1
    return bytes(b)


class FileAsArray(object):
    '''
    A wrapper class around a file-like object that allows it to be treated as an array.
    '''

    def __init__(self, fileobj):
        '''
        Initialize the class.
        '''
        self.fileobj = fileobj
        self.pos = 0
        self.slicepos = 0

    def __getitem__(self, key: typing.Union[int, slice]) -> bytes:
        '''
        Get the item based on the given key: an integer that specifies the offset, or a slice to
        read a range of bytes.
        '''
        if isinstance(key, slice):
            # We only handle limited slices, like this: x[n:]
            if key.stop != None and key.step != None:
                raise ValueError('slice error: ' + str(key))
            ret = FileAsArray(self.fileobj)
            ret.slicepos = key.start
            self.pos = -1
            return ret
        elif isinstance(key, int):
            if self.pos != key:
                self.fileobj.seek(key + self.slicepos)
                self.pos = key
            ret = self.fileobj.read(1)[0]
            self.pos += 1
            return ret
        else:
            raise ValueError('not implemented: ' + str(key))


class BinaryFile(object):
    '''
    An encapsulation around a file object that saves bytes read in increments so raw bytes can be
    shown.
    '''

    def __init__(self, fileobj):
        '''
        Initialize the instance of this binary stream.
        '''
        self.fileobj = fileobj
        self.saved = bytearray()

    def read(self, n: int) -> bytes:
        '''
        Read the next n bytes.
        '''
        if n < 0:
            raise ValueError('The read length must be >= 0')
        result = self.fileobj.read(n)
        self.saved += result
        return result

    def read_uint8(self) -> int:
        '''
        Read the next little-endian integer.
        '''
        return int.from_bytes(self.read(1), byteorder='little')

    def read_uint16(self) -> int:
        '''
        Read the next little-endian integer.
        '''
        return int.from_bytes(self.read(2), byteorder='little')

    def read_uint32(self) -> int:
        '''
        Read the next little-endian integer.
        '''
        return int.from_bytes(self.read(4), byteorder='little')

    def read_uint64(self) -> int:
        '''
        Read the next little-endian integer.
        '''
        return int.from_bytes(self.read(8), byteorder='little')

    def read_packed_uint64(self) -> int:
        '''
        Read the next packed integer.
        '''
        arr = FileAsArray(self)
        val, arr = unpack_int(arr)
        return val

    def read_packed_uint64_with_size(self) -> typing.Tuple[int, int]:
        '''
        Read the next packed integer; return (integer, number of bytes read).
        '''
        start = self.tell()
        arr = FileAsArray(self)
        val, arr = unpack_int(arr)
        sz = self.tell() - start
        return (val, sz)

    def read_long_length(self) -> int:
        '''
        Return a length as used in a cell that isn't a "short" cell. Lengths that are
        less or equal to 64 (WT_CELL_SIZE_ADJUST) are packed in a short cell, so if a
        non-short cell is used, the length numbering starts at 64.
        '''
        l = self.read_packed_uint64() + 64
        if l < 0:
            raise ValueError('Negative length: ' + str(l))
        return l

    def seek(self, n: int) -> int:
        '''
        Seek to a different position within the file.
        '''
        # Throw away previous saved when we seek
        self.saved = bytearray()
        return self.fileobj.seek(n)

    def tell(self) -> int:
        '''
        Tell the position within the file.
        '''
        return self.fileobj.tell()

    def saved_bytes(self) -> bytes:
        '''
        Return the bytes read since last call to this function.
        '''
        result = self.saved
        self.saved = bytearray()
        return bytes(result)

# Convert binary data to a multi-line string with hex and printable characters
def binary_to_pretty_string(b, per_line=16, line_prefix='  ', start_with_line_prefix=True):
    printable = ''
    result = ''
    if start_with_line_prefix:
        result += line_prefix
    if len(b) == 0:
        return result
    for i in range(0, len(b)):
        if i > 0:
            if i % per_line == 0:
                result += '  ' + printable + '\n' + line_prefix
                printable = ''
            else:
                result += ' '
        result += '%02x' % b[i]
        if b[i] >= ord(' ') and b[i] < 0x7f:
            printable += chr(b[i])
        else:
            printable += '.'
    if i % per_line != per_line - 1:
        for j in range(i % per_line + 1, per_line):
            result += '   '
    result += '  ' + printable
    return result
