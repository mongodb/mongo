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

# Decode a checkpoint 'addr'

import os, sys, getopt

def usage():
    print 'Usage:\n\
  $ python .../tools/wt_ckpt_decode.py [ -a allocsize ] addr...\n\
\n\
addr is a hex string\n\
'

def err_usage(msg):
        print 'wt_ckpt_decode.py: ERROR: ' + msg
        usage()
        sys.exit(False)

# Set paths
wt_disttop = sys.path[0]
while not os.path.isdir(wt_disttop + '/build_posix'):
    if wt_disttop == '/':
        err_usage('current dir not in wiredtiger development directory')
    wt_disttop = os.path.dirname(wt_disttop)
sys.path.insert(1, os.path.join(wt_disttop, 'lang', 'python', 'wiredtiger'))

from packing import pack, unpack

def show_one(label, value):
    l = 16 - len(label)
    l = l if l > 1 else 1
    print '    {0}{1}{2:10d}  (0x{2:x})'.format(label, (' ' * l), value, value)
    
def show_triple(triple, name, allocsize):
    off = triple[0]
    size = triple[1]
    csum = triple[2]
    if size == 0:
        off = -1
        csum = 0
    show_one(name + ' offset', (off + 1) * allocsize)
    show_one(name + ' size', (size) * allocsize)
    show_one(name + ' cksum', csum)
    print ''

def decode_arg(arg, allocsize):
    addr = arg.decode("hex")
    version = ord(addr[0])
    print arg + ': '
    if version != 1:
        print '**** ERROR: unknown version ' + str(version)
    addr = addr[1:]
    result = unpack('iiiiiiiiiiiiii',addr)
    if len(result) != 14:
        print '**** ERROR: result len unexpected: ' + str(len(result))
    show_triple(result[0:3], 'root', allocsize)
    show_triple(result[3:6], 'alloc', allocsize)
    show_triple(result[6:9], 'avail', allocsize)
    show_triple(result[9:12], 'discard', allocsize)
    file_size = result[12]
    ckpt_size = result[13]
    show_one('file size', file_size)
    show_one('checkpoint size', ckpt_size)

#decode_arg('018281e420f2fa4a8381e40c5855ca808080808080e22fc0e20fc0', 4096)
    
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
