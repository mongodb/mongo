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

# Dump/decode a PALM database for a particular file/page combination.

# TODO: The code would be clearer if we converted all the ASCII hex
#       strings to byte arrays early on.  Currently, we pass hex
#       strings everywhere, as this program evolved from a shell script.
#
# TODO: integrate with wt_binary_decode.py so that the payload is shown
#       as a WT btree page.

import copy, io, os, re, subprocess, sys
from py_common import binary_data, binary_data
import wt_ckpt_decode
binary_to_pretty_string = binary_data.binary_to_pretty_string  # a convenient function name

decode_version = "2024-11-15.0"

_python3 = (sys.version_info >= (3, 0, 0))
if not _python3:
    raise Exception('This script requires Python 3')

# Find the 'mdb_dump' utility.
def setup_lmdb_dump():
    # Assuming we're somewhere in a WT git directory, walk the tree up
    # looking for the lmdb sources or dump program.
    curdir = os.getcwd()
    wttop = curdir
    found = False
    while wttop != '/':
        if os.path.isdir(os.path.join(wttop, 'third_party', 'openldap_liblmdb')):
            found = True
            break
        wttop = os.path.dirname(d)
    if not found:
        print('Cannot find wt, must run this from a build directory')
        sys.exit(1)

    dumppath = os.path.join(wttop, 'third_party', 'openldap_liblmdb', 'mdb_dump')
    dumpdir = os.path.join(wttop, 'third_party', 'openldap_liblmdb')

    if not os.path.isfile(dumppath):
        os.system(f'echo ==== building mdb_dump in {dumpdir} ====; cd {dumpdir}; make mdb_dump; echo ====; echo')
    if not os.path.isfile(dumppath):
        print(f'Cannot build mdb_dump in {dumpdir}')
        sys.exit(1)

    return dumppath

def encode_bytes(f):
    lines = f.readlines()
    allbytes = bytearray()
    for line in lines:
        if ':' in line:
            (_, _, line) = line.rpartition(':')
        nospace = line.replace(' ', '').replace('\n', '')
        print('LINE (len={}): {}'.format(len(nospace), nospace))
        if len(nospace) > 0:
            print('first={}, last={}'.format(nospace[0], nospace[-1]))
            b = codecs.decode(nospace, 'hex')
            #b = bytearray.fromhex(line).decode()
            allbytes += b
    return allbytes

def dehex(hstr):
    return int(hstr, 16)

def swapstr(hstr):
    if len(hstr) % 2 == 0:
        r = ''
        n = len(hstr)
        while n > 0:
            n = n - 2
            r += hstr[n:]
            hstr = hstr[0:n]
        return r
    else:
        return hstr

class ColumnParser:
    def __init__(self, columns, match):
        self.columns = columns
        self.match = match

    def get(self, n):
        if n == len(self.columns) - 1:
            trail = ''
        else:
            trail = ', '
        nm = self.columns[n]['name']
        swap = False
        if 'swap' in self.columns[n]:
            swap = self.columns[n]['swap']
        if swap:
            val = swapstr(self.match.group(n))
        else:
            val = self.match.group(n)
        if 'hex' not in self.columns[n]:
            val = dehex(val)
        return f'{nm}={val}{trail}'

# Patterns to match and group ints of specified lengths
u64 = '([0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f])'
u32 = '([0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f])'
u16 = '([0-9a-f][0-9a-f][0-9a-f][0-9a-f])'
u8 = '([0-9a-f][0-9a-f])'

# e.g. 0000000000000003 00000000000000d2 0000000000000001 000000000000007e 00000000 00000000 0100000000000000 0100000000000000 0000000000000000 4223dc35bd260600
def palm_dump_key(opts, kstr):
    keypat = '^' + u64 + u64 + u64 + u64 + u32 + u32 + u64 + u64 + u64 + u64 + u64 + u64 + '$'
    columns = [ None,
                { 'name' : 'table_id' },
                { 'name' : 'page_id' },
                { 'name' : 'lsn' },
                { 'name' : 'checkpoint_id' },
                { 'name' : 'is_delta' },
                { 'name' : 'PADDING' },
                { 'name' : 'backlink_lsn', 'swap' : True },
                { 'name' : 'base_lsn', 'swap' : True },
                { 'name' : 'backlink_checkpoint_id', 'swap' : True },
                { 'name' : 'base_checkpoint_id', 'swap' : True },
                { 'name' : 'flags', 'swap' : True },
                { 'name' : 'timestamp_materialized_us', 'swap' : True }
               ]
    m = re.search(keypat, kstr)
    cp = ColumnParser(columns, m)
    if not m:
        raise Exception('bad key: ' + kstr)
    table_id = dehex(m.group(1))
    page_id = dehex(m.group(2))
    is_delta = dehex(m.group(5))

    print(f'PALM KEY: {cp.get(1)}{cp.get(2)}{cp.get(3)}{cp.get(4)}{cp.get(5)}{cp.get(7)}{cp.get(8)}{cp.get(9)}{cp.get(10)}{cp.get(11)}{cp.get(12)}')
    if m.group(6) != '00000000':
        print('  WARNING: gap between is_Delta and backlink is not zero')
    if is_delta < 0 or is_delta > 1:
        print('  WARNING: is_delta is not boolean')
    return [ is_delta, table_id, page_id ]

# e.g. 0000000000000000f80c000000000000ee080000de00000007040001
def palm_dump_page_header(vstr):
    keypat = '^' + u64 + u64 + u32 + u32 + u8 + u8 + u8 + u8 + '$'
    columns = [ None,
                { 'name' : 'recno', 'swap' : True  },
                { 'name' : 'write_gen', 'swap' : True  },
                { 'name' : 'mem_size', 'swap' : True  },
                { 'name' : 'entries', 'swap' : True  },
                { 'name' : 'type' },
                { 'name' : 'flags' },
                { 'name' : 'PADDING' },
                { 'name' : 'version' }
               ]
    m = re.search(keypat, vstr)
    cp = ColumnParser(columns, m)
    if not m:
        raise Exception('bad key: ' + vstr)
    print(f'  page_header: {cp.get(1)}{cp.get(2)}{cp.get(3)}{cp.get(4)}{cp.get(5)}{cp.get(6)}{cp.get(8)}')
    if m.group(7) != '00':
        print('  WARNING: padding byte is not zero')

# e.g.  db01012c7a1499df59d4831f02010000
def palm_dump_block_header(vstr):
    keypat = '^' + u8 + u8 + u8 + u8 + u32 + u32 + u8 + u8 + u16 + '$'
    columns = [ None,
                { 'name' : 'magic' },
                { 'name' : 'version' },
                { 'name' : 'compatible_version' },
                { 'name' : 'header_size' },
                { 'name' : 'checksum', 'hex' : True, 'swap' : True },
                { 'name' : 'previous_checksum', 'hex' : True, 'swap' : True  },
                { 'name' : 'reconciliation_id' },
                { 'name' : 'flags' },
                { 'name' : 'PADDING' },
               ]
    m = re.search(keypat, vstr)
    cp = ColumnParser(columns, m)
    if not m:
        raise Exception('bad key: ' + vstr)
    print(f'  block_header: {cp.get(1)}{cp.get(2)}{cp.get(3)}{cp.get(4)}{cp.get(5)}{cp.get(6)}{cp.get(7)}{cp.get(8)}')
    if m.group(9) != '0000':
        print('  WARNING: padding bytes are not zero')

def palm_dump_payload(opts, vstr):
    b = bytes.fromhex(vstr)
    continuation = ''
    binlen = len(vstr) // 2
    if opts.length != -1 and binlen > opts.length:
        vstr = vstr[0:opts.length * 2]
        continuation = '\n  ...'
    print(f'  payload:\n{binary_to_pretty_string(b)}{continuation}')

# Look for special strings like checkpoint address cookies
def advanced_checkpoint(b):
    # latin-1 decoding won't fail on whatever binary we have, and it's
    # easy to look at.
    lstr = b.decode("latin-1")
    pattern = 'addr="([0-9a-f]*)"'
    m = re.search(pattern, lstr)
    if m:
        # Show a decoded cookie
        print(f'Checkpoint cookie ', end='')
        wt_ckpt_decode.decode_arg(m.group(1), 4096)

# Look for special strings like checkpoint address cookies
def advanced_page(opts, b):
    import wt_binary_decode
    parser = wt_binary_decode.get_arg_parser()
    args = []
    args.append('-fsv')    # fragment
    args.append('--disagg')
    if opts.debug:
        args.append('-D')
    args.append('-')     # no file name
    bin_opts = parser.parse_args(args)
    bf = binary_data.BinaryFile(io.BytesIO(b))
    wt_binary_decode.wtdecode_file_object(bf, bin_opts, len(b))
    print('------------------------------------------')

def palm_dump_value(opts, vstr, is_delta):
    # The advanced option calls the binary decoder, so it doesn't need extra verbiage here.
    if not opts.advanced:
        print('PALM VALUE:')
    b = bytes.fromhex(vstr)
    if opts.raw:
        print(binary_to_pretty_string(b))
        if opts.advanced:
            advanced_checkpoint(b)
        return
    # Length of the page header, delta header, block header
    # Multiplied by 2 because the input is ascii hex - two chars is one byte.
    ph = 28*2
    dh = 12*2
    bh = 16*2
    if is_delta:
        palm_dump_page_header(vstr[0:dh])
        palm_dump_block_header(vstr[dh:dh+bh])
        palm_dump_payload(opts, vstr[dh+bh:])
    else:
        if opts.advanced:
            advanced_page(opts, b)
        else:
            palm_dump_page_header(vstr[0:ph])
            palm_dump_block_header(vstr[ph:ph+bh])
            palm_dump_payload(opts, vstr[ph+bh:])

def palm_dump(opts, line):
    pattern = '^ *([0-9a-f]*)[^0-9a-f]+([0-9a-f]*)$'
    m = re.search(pattern, line)
    [ is_delta, table_id, page_id ] = palm_dump_key(opts, m.group(1))
    t_opts = opts

    # If this is the metadata "table", turn on the raw option.
    if table_id == 1 and page_id == 1:
        t_opts = copy.deepcopy(opts)
        t_opts.raw = True
        t_opts.raw_advanced = True
    palm_dump_value(t_opts, m.group(2), is_delta)
    print('')

def palm_command_line_args():
    def check_pos_int(s):
        val = int(s)
        if val <= 0:
            raise argparse.ArgumentTypeError(f"{s} is not a value int")
        return val

    def check_int(s):
        return int(s)

    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('-D', '--debug', help="debug this tool", action='store_true')
    parser.add_argument('-l', '--length', help="max length of value payload to show", type=check_int)
    parser.add_argument('-p', '--pageid', help="page id to decode", type=check_pos_int)
    parser.add_argument('-r', '--raw', help="show raw bytes for value", action='store_true')
    parser.add_argument('-t', '--tableid', help="table id to decode", type=check_pos_int)
    parser.add_argument('-V', '--version', help="print version number of this program", action='store_true')
    parser.add_argument('directory', help="home directory for LMDB kv-store (often <WT_HOME>/kv-store)")
    opts = parser.parse_args()

    # Add additional options, that are used internally.
    opts.advanced = (not opts.raw)

    return opts

opts = palm_command_line_args()
if opts.version:
    print('wt_palm_decode version "{}"'.format(decode_version))
    sys.exit(0)

if not opts.length:
    # Set the length to a reasonable amount given the arguments.
    if opts.tableid and opts.pageid:
        opts.length = 1000
    else:
        opts.length = 0

dump_program = setup_lmdb_dump()
if not opts.tableid:
    tmatch = '................'
else:
    tmatch = "%0.16x" % opts.tableid
if not opts.pageid:
    pmatch = '................'
else:
    pmatch = "%0.16x" % opts.pageid

# TODO: we could parse this all in Python
cmd = f"{dump_program} -s pages {opts.directory} | sed -e '/^ /!d' -e 's/^ *//' | paste -d' ' - - | sed -e '/^{tmatch}{pmatch}/!d' -e 's/ / => /'"
#print(f"CMD = {cmd}")

try:
    process = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, shell=True, text=True)
    while True:
        line = process.stdout.readline()
        #print(f'LINE: {line}')
        if not line:
            break
        palm_dump(opts, line.rstrip())
except KeyboardInterrupt:
    pass
except BrokenPipeError:
    pass

sys.exit(0)
