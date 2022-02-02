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
#
# This script uses WiredTiger's integer unpacking library.  To load the
# WiredTiger library built in a development tree, you may have to set
# LD_LIBRARY_PATH or the equivalent for your system.  For example:
#   $ export LD_LIBRARY_PATH=`pwd`/../build

import os, sys, getopt

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

# Set paths
wt_disttop = sys.path[0]
env_builddir = os.getenv('WT_BUILDDIR')
curdir = os.getcwd()
if env_builddir and os.path.isfile(os.path.join(env_builddir, 'wt')):
    wt_builddir = env_builddir
elif os.path.isfile(os.path.join(curdir, 'wt')):
    wt_builddir = curdir
else:
    err_usage('Unable to find useable WiredTiger build.'
            'Call the script from either the build directory root or set the \'WT_BUILDDIR\' environment variable')

sys.path.insert(1, os.path.join(wt_builddir, 'lang', 'python'))

from wiredtiger.packing import unpack

def show_one(label, value):
    l = 16 - len(label)
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
    addr = bytearray.fromhex(arg)
    version = addr[0]
    print(arg + ': ')
    if version != 1:
        print('**** ERROR: unknown version ' + str(version))
    addr = bytes(addr[1:])

    # The number of values in a checkpoint may be 14 or 18. In the latter case, the checkpoint is
    # for a tiered Btree, and contains object ids for each of the four references in the checkpoint.
    # In the former case, the checkpoint is for a regular (local, single file) Btree, and there are
    # no objects. Based on what is present, we show them slightly appropriately.

    # First, we get the largest number of ints that can be decoded.
    result = []
    iformat = 'iiiiiiiiiiiiii'
    result_len = 0
    while True:
        try:
            result = unpack(iformat, addr)
            result_len = len(result)
        except:
            break
        iformat += 'i'

    # Then we check the number of results against what we expect.
    if result_len == 14:
        ref_cnt = 3   # no object ids
    elif result_len == 18:
        ref_cnt = 4   # has object ids
    else:
        if result_len == 0:
            result_len = 'unknown'
        print('**** ERROR: number of integers to decode ({}) '.format(result_len) +
              'does not match expected checkpoint format')
        return
    pos = 0

    # Now that we know whether the references have object ids, we can show them.
    for refname in [ 'root', 'alloc', 'avail', 'discard' ]:
        show_ref(result[pos : pos + ref_cnt], refname, allocsize)
        pos += ref_cnt
    file_size = result[pos]
    ckpt_size = result[pos+1]
    show_one('file size', file_size)
    show_one('checkpoint size', ckpt_size)

#decode_arg('018281e420f2fa4a8381e40c5855ca808080808080e22fc0e20fc0', 4096)  # regular Btree
#decode_arg('01818181e412e4fd01818281e41546bd16818381e4f2dbec3980808080e22fc0cfc0', 4096) # tiered
#decode_arg('01818181e412e4fd01818281e41546bd16818381e4f2dbec39808080e22fc0cfc0', 4096) # bad
    
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
