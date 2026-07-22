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

import os, re, sys
from helper import WiredTigerCursor

def extract_id(metadata_value):
    """
    Parse the file ID from the configuration string.
    """

    match = re.search(r',id=(\d+)', metadata_value)
    return int(match.group(1))

def get_table_id(session, uri):
    """
    Return the integer file ID for uri by reading the WT metadata cursor.
    """
    with WiredTigerCursor(session, 'metadata:') as c:
        c.set_key(uri)
        if c.search() != 0:
            raise KeyError(f"URI not found in metadata: {uri}")
        return extract_id(c.get_value())

# Byte offset of each address triple (offset, size, checksum) within a decoded checkpoint cookie.
_CKPT_COOKIE_TRIPLE = {'root': 0, 'alloc': 3, 'avail': 6, 'discard': 9}

def checkpoint_extent_list_blocks(session, uri, kinds=('alloc', 'avail', 'discard'), allocsize=4096):
    """
    Decode every checkpoint's address cookie in uri's metadata and return the on-disk
    (offset, size) of its extent-list blocks for the requested kinds ('alloc', 'avail', 'discard').

    A regular (non-tiered, non-disagg) checkpoint cookie is version 1 followed by 14 packed integers:
    the root, alloc, avail and discard address triples, then the file and checkpoint sizes. Only
    blocks with a non-zero size, i.e. those written to their own on-disk block, are returned.

    Corruption tests use this to target or avoid these blocks: a checkpoint that drops another reads
    its alloc and discard extent lists, and a failed read there is fatal.
    """
    # unpack_int lives with the wt tooling, which is not on the suite path by default.
    tools_dir = os.path.join(os.path.dirname(__file__), '..', '..', '..', 'tools')
    if tools_dir not in sys.path:
        sys.path.append(tools_dir)
    from py_common.binary_data import unpack_int

    with WiredTigerCursor(session, 'metadata:') as c:
        c.set_key(uri)
        if c.search() != 0:
            raise KeyError(f"URI not found in metadata: {uri}")
        config = c.get_value()

    blocks = []
    for cookie in re.findall(r'addr="([0-9a-fA-F]+)"', config):
        addr = bytes(bytearray.fromhex(cookie))
        if addr[0] != 1:
            continue
        addr = addr[1:]
        values = []
        while True:
            try:
                v, addr = unpack_int(addr)
                values.append(v)
            except Exception:
                break
        if len(values) != 14:
            continue
        for kind in kinds:
            off, size, _ = values[_CKPT_COOKIE_TRIPLE[kind]:_CKPT_COOKIE_TRIPLE[kind] + 3]
            if size != 0:
                blocks.append(((off + 1) * allocsize, size * allocsize))
    return blocks
