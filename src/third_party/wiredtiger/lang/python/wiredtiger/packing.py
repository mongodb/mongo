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
# WiredTiger variable-length packing and unpacking functions

"""Packing and unpacking functions
The format string uses the following conversions:
Format  Python  Notes
  x     N/A     pad byte, no associated value
  b     int     signed byte
  B     int     unsigned byte
  h     int     signed 16-bit
  H     int     unsigned 16-bit
  i     int     signed 32-bit
  I     int     unsigned 32-bit
  l     int     signed 32-bit
  L     int     unsigned 32-bit
  q     int     signed 64-bit
  Q     int     unsigned 64-bit
  r     int     record number
  s     str     fixed-length string
  S     str     NUL-terminated string
  t     int     fixed-length bit field
  u     str     raw byte array
"""

from intpacking import pack_int, unpack_int

def __get_type(fmt):
    if not fmt:
        return None, fmt
    # Variable-sized encoding is the default (and only supported format in v1)
    if fmt[0] in '.@<>':
        tfmt = fmt[0]
        fmt = fmt[1:]
    else:
        tfmt = '.'
    return tfmt, fmt

def __unpack_iter_fmt(fmt):
    size = 0
    havesize = 0
    for offset, char in enumerate(fmt):
        if char.isdigit():
            size = (size * 10) + int(char)
            havesize = 1
        else:
            if not havesize:
                size = 1
            yield offset, havesize, size, char
            size = 0
            havesize = 0

def unpack(fmt, s):
    tfmt, fmt = __get_type(fmt)
    if not fmt:
        return ()
    if tfmt != '.':
        raise ValueError('Only variable-length encoding is currently supported')
    result = []
    for offset, havesize, size, f in __unpack_iter_fmt(fmt):
        if f == 'x':
            s = s[size:]
            # Note: no value, don't increment i
        elif f in 'SsUu':
            if not havesize:
                if f == 's':
                    pass
                elif f == 'S':
                    size = s.find('\0')
                elif f == 'u' and offset == len(fmt) - 1:
                    size = len(s)
                else:
                    # Note: 'U' is used internally, and may be exposed to us.
                    # It indicates that the size is always stored unless there
                    # is a size in the format.
                    size, s = unpack_int(s)
            result.append(s[:size])
            if f == 'S' and not havesize:
                size += 1
            s = s[size:]
        elif f in 't':
            # bit type, size is number of bits
            result.append(ord(s[0:1]))
            s = s[1:]
        elif f in 'Bb':
            # byte type
            for i in xrange(size):
                v = ord(s[0:1])
                if f != 'B':
                    v -= 0x80
                result.append(v)
                s = s[1:]
        else:
            # integral type
            for j in xrange(size):
                v, s = unpack_int(s)
                result.append(v)
    return result

def __pack_iter_fmt(fmt, values):
    index = 0
    for offset, havesize, size, char in __unpack_iter_fmt(fmt):
        if char == 'x':  # padding no value
            yield offset, havesize, size, char, None
        elif char in 'SsUut':
            yield offset, havesize, size, char, values[index]
            index += 1
        else:            # integral type
            size = size if havesize else 1
            for i in xrange(size):
                value = values[index]
                yield offset, havesize, 1, char, value
                index = index + 1

def pack(fmt, *values):
    tfmt, fmt = __get_type(fmt)
    if not fmt:
        return ()
    if tfmt != '.':
        raise ValueError('Only variable-length encoding is currently supported')
    result = ''
    i = 0
    for offset, havesize, size, f, val in __pack_iter_fmt(fmt, values):
        if f == 'x':
            if not havesize:
                result += '\0'
            else:
                result += '\0' * size
            # Note: no value, don't increment i
        elif f in 'SsUu':
            if f == 'S' and '\0' in val:
                l = val.find('\0')
            else:
                l = len(val)
            if havesize or f == 's':
                if l > size:
                    l = size
            elif (f == 'u' and offset != len(fmt) - 1) or f == 'U':
                result += pack_int(l)
            if type(val) is unicode and f in 'Ss':
                result += str(val[:l])
            else:
                result += val[:l]
            if f == 'S' and not havesize:
                result += '\0'
            elif size > l:
                result += '\0' * (size - l)
        elif f in 't':
            # bit type, size is number of bits
            if not havesize:
                size = 1
            if size > 8:
                raise ValueError("bit count cannot be greater than 8 for 't' encoding")
            mask = (1 << size) - 1
            if (mask & val) != val:
                raise ValueError("value out of range for 't' encoding")
            result += chr(val)
        elif f in 'Bb':
            # byte type
            if not havesize:
                size = 1
            for i in xrange(size):
                if f == 'B':
                    v = val
                else:
                    # Translate to maintain ordering with the sign bit.
                    v = val + 0x80
                if v > 255 or v < 0:
                    raise ValueError("value out of range for 'B' encoding")
                result += chr(v)
        else:
            # integral type
            result += pack_int(val)
    return result
