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

# Decode a checkpoint 'addr'

import os, sys, getopt
from py_common.binary_data import unpack_int

def usage():
    print('Usage:\n\
  $ python .../tools/wt_ckpt_decode.py [ -a allocsize ] addr...\n\
\n\
addr is a hex string\n\
')

def err_usage(msg):
        print('wt_ckpt_decode.py: ERROR: ' + msg)
        usage()
        sys.exit(False)

def show_one(label, value):
    l = 20 - len(label)
    l = l if l > 1 else 1
    print('    {0}{1}{2:10d}  (0x{2:x})'.format(label, (' ' * l), value, value))
    
def show_ref(ref, name, allocsize):
    if len(ref) == 4:
        show_one(name + ' object', ref[0])
        ref = ref[1:]
    off = ref[0]
    size = ref[1]
    csum = ref[2]
    if size == 0:
        off = -1
        csum = 0
    show_one(name + ' offset', (off + 1) * allocsize)
    show_one(name + ' size', (size) * allocsize)
    show_one(name + ' cksum', csum)
    print('')

def decode_arg(arg, allocsize):
    disagg = False
    addr = bytearray.fromhex(arg)
    version = addr[0]
    print(arg + ': ')
    if version != 1:
        print('disagg version assumed')
        disagg = True
        addr = bytes(addr)
    else:
        addr = bytes(addr[1:])

    # The number of values in a checkpoint may be 14 or 18. In the latter case, the checkpoint is
    # for a tiered Btree, and contains object ids for each of the four references in the checkpoint.
    # In the former case, the checkpoint is for a regular (local, single file) Btree, and there are
    # no objects. Based on what is present, we show them slightly appropriately.

    # First, we get the largest number of ints that can be decoded.
    result = []
    result_len = 0
    while True:
        try:
            i, addr = unpack_int(addr)
            result.append(i)
            result_len += 1
        except:
            break

    # Then we check the number of results against what we expect.
    if result_len == 14:
        ref_cnt = 3   # no object ids: each address cookie has three entries
    elif result_len == 18:
        ref_cnt = 4   # has object ids: each address cookie has four entries
    elif not disagg or (disagg and result_len != 5 and result_len != 6):
        if result_len == 0:
            result_len = 'unknown'
        print(f'**** ERROR: number of integers to decode ({result_len}) ' +
              'does not match expected checkpoint format')
        return

    if not disagg:
        # A 'regular' WT checkpoint cookie is the concatenation of four address cookies:
        # one for the root page and three for extent lists.
        # Now that we know whether each address cookie contains an object id, we can show them.
        pos = 0
        for refname in [ 'root', 'alloc', 'avail', 'discard' ]:
            show_ref(result[pos : pos + ref_cnt], refname, allocsize)
            pos += ref_cnt
        file_size = result[pos]
        ckpt_size = result[pos+1]
        show_one('file size', file_size)
        show_one('checkpoint size', ckpt_size)
    else:
        # A disaggregated storage checkpoint cookie is simply an address cookie for the root page.
        if result_len == 5:
            # Old style address cookie
            show_one('root page id', result[0])
            show_one('root checkpoint id', result[1])
            show_one('root rec id', result[2])
            show_one('root size', result[3])
            show_one('root checksum', result[4])
        elif result_len == 6:
            # New style address cookie
            show_one('root page id', result[0])
            show_one('root lsn', result[1])
            show_one('root checkpoint id', result[2])
            show_one('root rec id', result[3])
            show_one('root size', result[4])
            show_one('root checksum', result[5])

#decode_arg('018281e420f2fa4a8381e40c5855ca808080808080e22fc0e20fc0', 4096)  # regular Btree
#decode_arg('01818181e412e4fd01818281e41546bd16818381e4f2dbec3980808080e22fc0cfc0', 4096) # tiered
#decode_arg('c0268280bfe4ef31a1e1', 4096) # disagg, 5 entries
#decode_arg('c026e4123436388280bfe4ef31a1e1', 4096) # disagg, 6 entries
#decode_arg('01818181e412e4fd01818281e41546bd16818381e4f2dbec39808080e22fc0cfc0', 4096) # bad

# Only run the main code if this file is not imported.
if __name__ == '__main__':
    allocsize = 4096
    try:
        opts, args = getopt.getopt(sys.argv[1:], "a:", ["allocsize"])
    except getopt.GetoptError as err:
        err_usage(str(err))
    for o, a in opts:
        if o == '-a':
            allocsize = int(a)
    
    for arg in args:
        decode_arg(arg, allocsize)
