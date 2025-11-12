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

import string
import wiredtiger

from enum import Enum

class OpType(Enum):
    ADD = 1
    REMOVE = 2
    REPLACE = 3

def create_value(r, size, repeat_size, valuefmt):
    choices = string.ascii_letters + string.digits
    if valuefmt == 'S':
        pattern = ''.join(r.choice(choices) for _ in range(repeat_size))
    elif valuefmt == 'u':
        pattern = b''.join(bytes([r.choice(choices.encode())]) for _ in range(repeat_size))
    else:
        raise ValueError(f"unsupported value fmt {valuefmt}")
    return (pattern * ((size + repeat_size - 1) // repeat_size))[:size]

def create_mods(rand, oldsz, repeatsz, nmod, maxdiff, valuefmt, oldv=None):
    if oldv == None:
        oldv = create_value(rand, oldsz, repeatsz, valuefmt)

    offsets = sorted(rand.sample(range(oldsz), nmod))
    modsizes = sorted(rand.sample(range(maxdiff), nmod + 1))
    lengths = [modsizes[i+1] - modsizes[i] for i in range(nmod)]
    modtypes = [rand.choice((OpType.ADD, OpType.REMOVE, OpType.REPLACE)) for _ in range(nmod)]

    orig = oldv
    newv = '' if valuefmt == 'S' else b''
    for i in range(1, nmod):
        if offsets[i] - offsets[i - 1] < maxdiff:
            continue
        newv += orig[:(offsets[i]-offsets[i-1])]
        orig = orig[(offsets[i]-offsets[i-1]):]
        if modtypes[i] == OpType.ADD:
            newv += create_value(rand, lengths[i], rand.randint(1, lengths[i]), valuefmt)
        elif modtypes[i] == OpType.REMOVE:
            orig = orig[lengths[i]:]
        elif modtypes[i] == OpType.REPLACE:
            newv += create_value(rand, lengths[i], rand.randint(1, lengths[i]), valuefmt)
            orig = orig[lengths[i]:]
    newv += orig

    try:
        mods = wiredtiger.wiredtiger_calc_modify(None, oldv, newv, max(maxdiff, nmod * 64), nmod)
    except wiredtiger.WiredTigerError:
        # When the data repeats, the algorithm can register the "wrong" repeated sequence.  Retry...
        mods = wiredtiger.wiredtiger_calc_modify(None, oldv, newv, nmod * (64 + repeatsz), nmod)

    return (oldv, mods, newv)
