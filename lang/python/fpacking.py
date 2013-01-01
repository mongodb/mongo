#!/usr/bin/env python
#
# Copyright (c) 2008-2013 WiredTiger, Inc.
#	All rights reserved.
#
# See the file LICENSE for redistribution information.
#
# WiredTiger fixed-size packing and unpacking functions, using the Python
# struct library.

import struct

def __wt2struct(fmt):
    if not fmt:
        return None, fmt
    # Big endian with no alignment is the default
    if fmt[0] in '@=<>!':
        tfmt = fmt[0]
        fmt = fmt[1:]
    else:
        tfmt = '>'
    return tfmt, fmt.replace('r', 'Q')

def unpack(fmt, s):
    tfmt, fmt = __wt2struct(fmt)
    if not fmt:
        return ()
    result = ()
    pfmt = tfmt
    sizebytes = 0
    for offset, f in enumerate(fmt):
        if f.isdigit():
            sizebytes += 1
        # With a fixed size, everything is encoded as a string
        if f in 'Su' and sizebytes > 0:
            f = 's'
        if f not in 'Su':
            pfmt += f
            sizebytes = 0
            continue

        # We've hit something that needs special handling, split any fixed-size
        # values we've already passed
        if len(pfmt) > 1:
            size = struct.calcsize(pfmt)
            result += struct.unpack_from(pfmt, s)
            s = s[size:]
        if f == 'S':
            l = s.find('\0')
            result += (s[:l],)
            s = s[l+1:]
        if f == 'u':
            if offset == len(fmt) - 1:
                result += (s,)
            else:
                l = struct.unpack_from(tfmt + 'l', s)[0]
                s = s[struct.calcsize(tfmt + 'l'):]
                result += (s[:l],)
                s = s[l:]
        pfmt = tfmt
        sizebytes = 0

    if len(pfmt) > 1:
        result += struct.unpack(pfmt, s)
    return result

def pack(fmt, *values):
    pfmt, fmt = __wt2struct(fmt)
    if not fmt:
        return ''
    i = sizebytes = 0
    for offset, f in enumerate(fmt):
        if f == 'S':
            # Note: this code is being careful about embedded NUL characters
            if sizebytes == 0:
                l = values[i].find('\0') + 1
                if not l:
                    l = len(values[i]) + 1
                pfmt += str(l)
                sizebytes = len(str(l))
            f = 's'
        elif f == 'u':
            if sizebytes == 0 and offset != len(fmt) - 1:
                l = len(values[i])
                pfmt += 'l' + str(l)
                values = values[:i] + (l,) + values[i:]
                sizebytes = len(str(l))
            f = 's'
        pfmt += f
        if f.isdigit():
            sizebytes += 1
            continue
        if f != 's' and sizebytes > 0:
            i += int(pfmt[-sizebytes:])
        else:
            i += 1
        sizebytes = 0
    return struct.pack(pfmt, *values)
