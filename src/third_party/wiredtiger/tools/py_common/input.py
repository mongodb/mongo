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

import codecs
import re


def encode_bytes(f, opts):
    """
    Encode a text hex dump into raw bytes.

    This mirrors the logic used by the standalone decoding tools: for each line,
    strip off any prefix up to and including the last ':', then keep only
    hexadecimal characters and decode them into bytes. All decoded chunks are
    concatenated and returned as a single ``bytearray``.
    """

    lines = f.readlines()
    allbytes = bytearray()
    for line in lines:
        if ':' in line:
            (_, _, line) = line.rpartition(':')
        # Keep anything that looks like it could be hexadecimal,
        # remove everything else.
        nospace = re.sub(r"[^a-fA-F\d]", "", line)
        if opts.debug:
            print("LINE (len={}): {}".format(len(nospace), nospace))
        if len(nospace) > 0:
            if opts.debug:
                print("first={}, last={}".format(nospace[0], nospace[-1]))
            b = codecs.decode(nospace, "hex")
            # b = bytearray.fromhex(line).decode()
            allbytes += b
    return allbytes
