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

# Decode a disagg address cookie from a hex string.

import argparse, json
from py_common import btree_format

# Example:
#   Input: 
#     00c09880e869252cb0ffffdfc5e869252cb0ffffdfc5c00a5d4a4c25
# 
#   Output: 
#     {
#       "page_id": 216,
#       "flags": 0,
#       "lsn": 7576511086841561093,
#       "base_lsn": 7576511086841561093,
#       "size": 74,
#       "checksum": 625756765,
#       "version": 0
#     }

def decode_addr(addr_raw):
    addr_bytes = bytes.fromhex(addr_raw)

    addr = btree_format.DisaggAddr.parse(addr_bytes)

    print(json.dumps(addr.__dict__, indent=2))

if __name__ == '__main__':
    parser = argparse.ArgumentParser(
        description='Decode a WiredTiger disaggregated address cookie as defined in block.h. \
            A disaggregated address cookie contains the page metadata required to fetch the page \
            from the page and log service.'
    )
    parser.add_argument(
        'hex_address',
        help='Hex address string to decode'
    )
    
    args = parser.parse_args()
    
    decode_addr(args.hex_address)
