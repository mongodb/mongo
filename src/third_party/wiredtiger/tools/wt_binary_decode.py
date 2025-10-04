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

# Dump/decode a WiredTiger .wt file.

# This is not a complete dump program and should not be depended upon for correctness --
# see "wt dump" for that.  But this is standalone (doesn't require linkage with any WT
# libraries), and may be useful as 1) a learning tool 2) quick way to hack/extend dumping.

import codecs, io, os, re, sys, traceback, pprint
from py_common import binary_data, btree_format
from dataclasses import dataclass
import typing
binary_to_pretty_string = binary_data.binary_to_pretty_string  # a convenient function nam

# Optional dependency: crc32c
have_crc32c = False
try:
    import crc32c
    have_crc32c = True
except:
    pass

# Optional dependency: python-snappy
have_snappy = False
try:
    import snappy
    have_snappy = True
except:
    pass

# Optional dependency: bson
have_bson = False
try:
    import bson
    have_bson = True
except:
    pass

decode_version = "2023-03-03.0"

# A container for page stats
@dataclass
class PageStats:
    num_keys: int = 0
    keys_sz: int = 0

    # prepared updates (not reported currently)
    num_d_start_ts: int = 0
    d_start_ts_sz: int = 0
    num_d_stop_ts: int = 0
    d_stop_ts_sz: int = 0

    # durable history
    num_start_ts: int = 0
    start_ts_sz: int = 0
    num_stop_ts: int = 0
    stop_ts_sz: int = 0
    num_start_txn: int = 0
    start_txn_sz: int = 0
    num_stop_txn: int = 0
    stop_txn_sz: int = 0

    @property
    def num_ts(self) -> int:
        return self.num_d_start_ts + self.num_d_stop_ts + self.num_start_ts + self.num_stop_ts

    @property
    def ts_sz(self) -> int:
        return self.d_start_ts_sz + self.d_stop_ts_sz + self.start_ts_sz + self.stop_ts_sz

    @property
    def num_txn(self) -> int:
        return self.num_start_txn + self.num_stop_txn

    @property
    def txn_sz(self) -> int:
        return self.start_txn_sz + self.stop_txn_sz

    @staticmethod
    def csv_cols() -> typing.List[str]:
        return [
            'num keys',
            'keys size',
            'num durable start ts',
            'durable start ts size',
            'num durable stop ts',
            'durable stop ts size',
            'num start ts',
            'start ts size',
            'num stop ts',
            'stop ts size',
            'num ts',
            'ts size',
            'num start txn',
            'start txn size',
            'num stop txn',
            'stop txn size',
            'num txn',
            'txn size'
        ]

    def to_csv_cols(self) -> typing.List[int]:
        return [
            self.num_keys,
            self.keys_sz,
            self.num_d_start_ts,
            self.d_start_ts_sz,
            self.num_d_stop_ts,
            self.d_stop_ts_sz,
            self.num_start_ts,
            self.start_ts_sz,
            self.num_stop_ts,
            self.stop_ts_sz,
            self.num_ts,
            self.ts_sz,
            self.num_start_txn,
            self.start_txn_sz,
            self.num_stop_txn,
            self.stop_txn_sz,
            self.num_txn,
            self.txn_sz,
        ]


_python3 = (sys.version_info >= (3, 0, 0))

if not _python3:
    raise Exception('This script requires Python 3')

################################################################

# Manages printing to output.
# We keep track of cells, the first line printed for a new cell
# shows the cell number, subsequent lines are indented a little.
# If the split option is on, we show any bytes that were used
# in decoding before the regular decoding output appears.
# Those 'input bytes' are shown shifted to the right.
class Printer(object):
    def __init__(self, binfile, opts):
        self.binfile = binfile
        self.issplit = opts.split
        self.verbose = opts.verbose
        self.ext = opts.ext
        self.cellpfx = ''
        self.in_cell = False

    def begin_cell(self, cell_number):
        self.cellpfx = f'{cell_number}: '
        self.in_cell = True
        ignore = self.binfile.saved_bytes()  # reset the saved position

    def end_cell(self):
        self.in_cell = False
        self.cellpfx = ''

    # This is the 'print' function, used as p.rint()
    def rint(self, s):
        if self.issplit:
            saved_bytes = self.binfile.saved_bytes()[:]
            # For the split view, we want to have the bytes related to
            # stuff to be normally printed to appear indented by 40 spaces,
            # with 10 more spaces to show a possibly abbreviated file position.
            # If we are beginning a cell, we want that to appear left justified,
            # within the 40 spaces of indentation.
            if len(saved_bytes) > 0:
                # create the 10 character file position
                # the current file position has actually advanced by
                # some number of bytes, so subtract that now.
                cur_pos = self.binfile.tell() - len(saved_bytes)
                file_pos = f'{cur_pos:x}'
                if len(file_pos) > 8:
                    file_pos = '...' + file_pos[-5:]
                elif len(file_pos) < 8:
                    file_pos = ' ' * (8 - len(file_pos)) + file_pos
                file_pos += ': '

                indentation = (self.cellpfx + ' ' * 40)[0:40]
                self.cellpfx = ''
                while len(saved_bytes) > 20:
                    print(indentation + file_pos + str(saved_bytes[:20].hex(' ')))
                    saved_bytes = saved_bytes[20:]
                    indentation = ' ' * 40
                    file_pos = ' ' * 10
                print(indentation + file_pos + str(saved_bytes.hex(' ')))

        pfx = self.cellpfx
        self.cellpfx = ''
        if pfx == '' and self.in_cell:
            pfx = '  '
        print(pfx + s)

    def rint_v(self, s):
        if self.verbose:
            self.rint(s)
            
    def rint_ext(self, s):
        if self.ext:
            self.rint(s)

def ts(uint64):
    return hex(uint64)

def txn(uint64):
    return hex(uint64)

# Print the bytes, which may be keys or values.
def raw_bytes(b):
    if type(b) != type(b''):
        # Not bytes, it's already a string.
        return b

    # If the high bit of the first byte is on, it's likely we have
    # a packed integer.  If the high bit is off, it's possible we have
    # a packed integer (it would be negative) but it's harder to guess,
    # we'll presume a string.  But if the byte is 0x7f, that's ASCII DEL,
    # very unlikely to be the beginning of a string, but it decodes as -1,
    # so seems more likely to be an int.  If the UTF-8 decoding of the
    # string fails, we probably just have binary data.

    # Try decoding as one or more packed ints
    result = ''
    s = b
    while len(s) > 0 and s[0] >= 0x7f:
        val, s = binary_data.unpack_int(s)
        if result != '':
            result += ' '
        result += f'<packed {d_and_h(val)}>'
    if len(s) == 0:
        return result

    # See if the rest of the bytes can be decoded as a string
    try:
        if result != '':
            result += ' '
        return f'"{result + s.decode()}"'
    except:
        pass

    # The earlier steps failed, so it must be binary data
    return binary_to_pretty_string(b, start_with_line_prefix=False)

# Show an integer as decimal and hex
def d_and_h(n):
    return f'{n} (0x{n:x})'

def dumpraw(p, b, pos):
    savepos = b.tell()
    b.seek(pos)
    i = 0
    per_line = 16
    s = binary_to_pretty_string(b.read(256), per_line=per_line, line_prefix='')
    for line in s.splitlines():
        p.rint_v(hex(pos + i) + ':  ' + line)
        i += per_line
    b.seek(savepos)

def file_header_decode(p, b):
    # block.h
    h = btree_format.BlockFileHeader.parse(b)
    p.rint('magic: ' + str(h.magic))
    p.rint('major: ' + str(h.major))
    p.rint('minor: ' + str(h.minor))
    p.rint('checksum: ' + str(h.checksum))
    if h.magic != btree_format.BlockFileHeader.WT_BLOCK_MAGIC:
        p.rint('bad magic number')
        return
    if h.major != btree_format.BlockFileHeader.WT_BLOCK_MAJOR_VERSION:
        p.rint('bad major number')
        return
    if h.minor != btree_format.BlockFileHeader.WT_BLOCK_MINOR_VERSION:
        p.rint('bad minor number')
        return
    if h.unused != 0:
        p.rint('garbage in unused bytes')
        return
    p.rint('')

def process_timestamps(p, cell: btree_format.Cell, pagestats: PageStats):
    if cell.extra_descriptor == 0:
        return

    p.rint_v('cell has timestamps:')
    if cell.prepared:
        p.rint_v(' prepared')

    if cell.start_ts is not None:
        pagestats.start_ts_sz += cell.size_start_ts
        pagestats.num_start_ts += 1
        p.rint_v(' start ts: ' + ts(cell.start_ts))
    if cell.start_txn is not None:
        pagestats.start_txn_sz += cell.size_start_txn
        pagestats.num_start_txn += 1
        p.rint_v(' start txn: ' + txn(cell.start_txn))
    if cell.durable_start_ts is not None:
        pagestats.d_start_ts_sz += cell.size_durable_start_ts
        pagestats.num_d_start_ts += 1
        p.rint_v(' durable start ts: ' + ts(cell.durable_start_ts))

    if cell.stop_ts is not None:
        pagestats.stop_ts_sz += cell.size_stop_ts
        pagestats.num_stop_ts += 1
        p.rint_v(' stop ts: ' + ts(cell.stop_ts))
    if cell.stop_txn is not None:
        pagestats.stop_txn_sz += cell.size_stop_txn
        pagestats.num_stop_txn += 1
        p.rint_v(' stop txn: ' + txn(cell.stop_txn))
    if cell.durable_stop_ts is not None:
        pagestats.d_stop_ts_sz += cell.size_durable_stop_ts
        pagestats.num_d_stop_ts += 1
        p.rint_v(' durable stop ts: ' + ts(cell.durable_stop_ts))

def block_decode(p, b, nbytes, opts):
    disk_pos = b.tell()
    disagg_delta = False

    # Switch the printer and the binary stream to work on the page data as opposed to working on
    # the file itself. We need to do this to support compressed blocks. As a consequence, offsets
    # printed in the split mode are relative to a (potentially uncompressed) page, rather than
    # the file.
    if opts.disagg:
        # Size of WT_PAGE_HEADER
        page_data = bytearray(b.read(28))
        if page_data[0] == 0xdd:
            disagg_delta = True
         # Add 16 for block header + 28 bytes for page header, total of 44
        page_data += bytearray(b.read(16))
    else:
        # Size of WT_PAGE_HEADER + size of WT_BLOCK_HEADER
        page_data = bytearray(b.read(40))
    b.saved_bytes()
    b_page = binary_data.BinaryFile(io.BytesIO(page_data))
    p = Printer(b_page, opts)

    if disagg_delta:
        # WT_BLOCK_HEADER in block.h (44 bytes)
        blockhead = btree_format.BlockHeader.parse(b_page, disagg=opts.disagg)
        # WT_PAGE_HEADER in btmem.h (28 bytes)
        pagehead = btree_format.PageHeader.parse(b_page)
    else:
        # WT_PAGE_HEADER in btmem.h (28 bytes)
        pagehead = btree_format.PageHeader.parse(b_page)
        # WT_BLOCK_HEADER in block.h (12 bytes or 44 bytes)
        blockhead = btree_format.BlockHeader.parse(b_page, disagg=opts.disagg)

        if pagehead.unused != 0:
            p.rint('? garbage in unused bytes')
            return
        if pagehead.type == btree_format.PageType.WT_PAGE_INVALID:
            p.rint('? invalid page')
            return

        p.rint('Page Header:')
        p.rint('  recno: ' + str(pagehead.recno))
        p.rint('  writegen: ' + str(pagehead.write_gen))
        p.rint('  memsize: ' + str(pagehead.mem_size))
        p.rint('  ncells (oflow len): ' + str(pagehead.entries))
        p.rint('  page type: ' + str(pagehead.type.value) + ' (' + pagehead.type.name + ')')
        p.rint('  page flags: ' + hex(pagehead.flags))
        p.rint('  version: ' + str(pagehead.version))

    if blockhead.unused != 0:
        p.rint('garbage in unused bytes')
        return

    if opts.disagg and nbytes > 0:
        blockhead.disk_size = nbytes

    if blockhead.disk_size > 17 * 1024 * 1024:
        # The maximum document size in MongoDB is 16MB. Larger block sizes are suspect.
        p.rint('the block is too big')
        return
    if blockhead.disk_size < 40 and not opts.disagg:
        # The disk size is too small
        return

    p.rint('Block Header:')
    p.rint('  disk_size: ' + str(blockhead.disk_size))
    p.rint('  checksum: ' + hex(blockhead.checksum))
    p.rint('  block flags: ' + hex(blockhead.flags))

    if disagg_delta:
        p.rint('Delta Page Header:')
        p.rint('  writegen: ' + str(pagehead.write_gen))
        p.rint('  memsize: ' + str(pagehead.mem_size))
        p.rint('  ncells (oflow len): ' + str(pagehead.entries))
        p.rint('  page type: ' + str(pagehead.type.value) + ' (' + pagehead.type.name + ')')
        p.rint('  page flags: ' + hex(pagehead.flags))
        p.rint('  version: ' + str(pagehead.version))

    pagestats = PageStats()

    # Verify the checksum
    if have_crc32c:
        savepos = b.tell()
        b.seek(disk_pos)
        if blockhead.flags & btree_format.BlockHeader.WT_BLOCK_DATA_CKSUM != 0:
            check_size = blockhead.disk_size
        else:
            check_size = 64
        data = bytearray(b.read(check_size))
        b.seek(savepos)
        # Zero-out the checksum field
        data[32] = data[33] = data[34] = data[35] = 0
        if len(data) < check_size:
            p.rint('? reached EOF before the end of the block')
            return
        checksum = crc32c.crc32c(data)
        if checksum != blockhead.checksum:
            p.rint(f'? the calculated checksum {hex(checksum)} does not match header checksum {blockhead.checksum}')
            if (not opts.cont):
                return

    # Skip the rest if we don't want to display the data
    skip_data = opts.skip_data
    if opts.disagg and blockhead.flags & btree_format.BlockHeader.WT_BLOCK_DISAGG_COMPRESSED:
        p.rint(f'? the block is compressed, skipping payload')
        skip_data = True
    if opts.disagg and blockhead.flags & btree_format.BlockHeader.WT_BLOCK_DISAGG_ENCRYPTED:
        p.rint(f'? the block is encrypted, skipping payload')
        skip_data = True

    if skip_data:
        b.seek(disk_pos + blockhead.disk_size)
        return

    # Read the block contents
    payload_pos = b.tell()
    header_length = payload_pos - disk_pos
    if pagehead.flags & btree_format.PageHeader.WT_PAGE_COMPRESSED != 0:
        if not have_snappy:
            p.rint('? the page is compressed (install python-snappy to parse)')
            return
        try:
            compress_skip = 64
            # The first few bytes are uncompressed
            payload_data = bytearray(b.read(compress_skip - header_length))
            # Read the length of the remaining data
            length = min(b.read_uint64(), blockhead.disk_size - compress_skip - 8)
            # Read the compressed data, seek to the end of the block, and uncompress
            compressed_data = b.read(length)
            b.seek(disk_pos + blockhead.disk_size)
            payload_data.extend(snappy.uncompress(compressed_data))
        except:
            p.rint('? the page failed to uncompress')
            if opts.debug:
                traceback.print_exception(*sys.exc_info())
            return
    else:
        payload_data = b.read(pagehead.mem_size - header_length)
        b.seek(disk_pos + blockhead.disk_size)

    # Add the payload to the page data & reinitialize the stream and the printer
    page_data.extend(payload_data)
    b_page = binary_data.BinaryFile(io.BytesIO(page_data))
    b_page.seek(header_length)
    p = Printer(b_page, opts)

    # Parse the block contents
    if pagehead.type == btree_format.PageType.WT_PAGE_INVALID:
        pass    # a blank page: TODO maybe should check that it's all zeros?
    elif pagehead.type == btree_format.PageType.WT_PAGE_BLOCK_MANAGER:
        extlist_decode(p, b_page, pagehead, blockhead, pagestats)
    elif pagehead.type == btree_format.PageType.WT_PAGE_COL_VAR:
        p.rint_v('? unimplemented decode for page type WT_PAGE_COLUMN_VARIABLE')
        p.rint_v(binary_to_pretty_string(payload_data))
    elif pagehead.type == btree_format.PageType.WT_PAGE_ROW_INT or \
        pagehead.type == btree_format.PageType.WT_PAGE_ROW_LEAF:
        row_decode(p, b_page, pagehead, blockhead, pagestats, opts)
    elif pagehead.type == btree_format.PageType.WT_PAGE_OVFL:
        # Use b_page.read() so that we can also print the raw bytes in the split mode
        p.rint_v(raw_bytes(b_page.read(len(payload_data))))
    else:
        p.rint_v('? unimplemented decode for page type {}'.format(pagehead.type))
        p.rint_v(binary_to_pretty_string(payload_data))

    outfile_stats_end(opts, pagehead, blockhead, pagestats)

# Hacking this up so we count timestamps and txns
def row_decode(p, b, pagehead, blockhead, pagestats, opts):
    for cellnum in range(0, pagehead.entries):
        cellpos = b.tell()
        if cellpos >= pagehead.mem_size:
            p.rint_v('** OVERFLOW memsize **')
            return
        p.begin_cell(cellnum)

        try:
            cell = btree_format.Cell.parse(b, True)

            desc_str = f'desc: 0x{cell.descriptor:x} '
            if cell.extra_descriptor != 0:
                p.rint_v(desc_str + f'extra: 0x{cell.extra_descriptor:x}')
                desc_str = ''
                process_timestamps(p, cell, pagestats)
            if cell.run_length is not None:
                p.rint_v(desc_str + f'runlength/addr: {d_and_h(cell.run_length)}')
                desc_str = ''

            s = '?'
            if cell.is_address:
                s = 'addr (leaf no-overflow) '
            elif cell.is_key:
                s = 'key '
            elif cell.is_value:
                s = 'val '
            elif cell.is_unsupported:
                p.rint(desc_str + ', celltype = {} ({}) not implemented' \
                       .format(cell.cell_type.value, cell.cell_type.name))
                desc_str = ''
            else:
                raise ValueError('Unexpected cell type')

            if cell.is_overflow:
                s = 'overflow ' + s
            if cell.is_short:
                s = 'short ' + s
            if cell.prefix is not None:
                s += 'prefix={}'.format(hex(cell.prefix))
            if not cell.is_unsupported:
                s += '{} bytes'.format(len(cell.data))

            if cell.is_key:
                pagestats.num_keys += 1
                pagestats.keys_sz += len(cell.data)

            try:
                if s != '?':
                    if (cell.is_value and opts.bson and have_bson):
                        if (bson.is_valid(cell.data)):
                            p.rint_v("cell is valid BSON")
                            decoded_data = bson.BSON(cell.data).decode()
                            p.rint_v(pprint.pformat(decoded_data, indent=2))
                        else:
                            p.rint_v("cannot decode cell as BSON")
                            p.rint_v(f'{desc_str}{s}:')
                            p.rint_v(raw_bytes(cell.data))
                    else:
                        p.rint_v(f'{desc_str}{s}:')
                        p.rint_v(raw_bytes(cell.data))
                else:
                    dumpraw(p, b, cellpos)
            except (IndexError, ValueError):
                # FIXME-WT-13000 theres a bug in raw_bytes
                pass

        finally:
            p.end_cell()

def extlist_decode(p, b, pagehead, blockhead, pagestats):
    WT_BLOCK_EXTLIST_MAGIC = 71002       # from block.h
    # Written by block_ext.c
    okay = True
    cellnum = -1
    lastoff = 0
    p.rint_ext('extent list follows:')
    while True:
        cellnum += 1
        cellpos = b.tell()
        if cellpos >= pagehead.mem_size:
            p.rint_ext(f'** OVERFLOW memsize ** memsize={pagehead.mem_size}, position={cellpos}')
            #return
        p.begin_cell(cellnum)

        try:
            off = b.read_packed_uint64()
            size = b.read_packed_uint64()
            extra_stuff = ''
            if cellnum == 0:
                extra_stuff += '  # magic number'
                if off != WT_BLOCK_EXTLIST_MAGIC or size != 0:
                    extra_stuff = f'  # ERROR: magic number did not match expected value=' + \
                        '{WT_BLOCK_EXTLIST_MAGIC}'
                    okay = False
            else:
                if off < lastoff:
                    extra_stuff = f'  # ERROR: list out of order'
                    okay = False

                # We expect sizes and positions to be multiples of
                # this number, it is conservative.
                multiple = 256
                if off % multiple != 0:
                    extra_stuff = f'  # ERROR: offset is not a multiple of {multiple}'
                    okay = False
                if off != 0 and size % multiple != 0:
                    extra_stuff = f'  # ERROR: size is not a multiple of {multiple}'
                    okay = False

            # A zero offset is written as an end of list marker,
            # in that case, the size is a version number.
            # For version 0, this is truly the end of the list.
            # For version 1, additional entries may be appended to this (avail) list.
            #
            # See __wti_block_extlist_write() in block_ext.c, and calls
            # to that function in block_ckpt.c.
            if off == 0:
                extra_stuff += '  # end of list'
                if size == 0:
                    extra_stuff += ', version 0'
                elif size == 1:
                    extra_stuff += ', version 1,' + \
                    ' any following entries are not yet in this (incomplete) checkpoint'
                else:
                    extra_stuff += f' -- ERROR unexpected size={size} has no meaning here'
                    okay = False
            p.rint_ext(f'  {off}, {size}{extra_stuff}')
        finally:
            p.end_cell()
        if off == 0 or not okay:
            break

def outfile_header(opts):
    if opts.output != None:
        fields = [
            "block id",

            # page head
            "writegen",
            "memsize",
            "ncells",
            "page type",

            # block head
            "disk size",

            # page stats
            *PageStats.csv_cols(),
        ]
        opts.output.write(",".join(fields))

def outfile_stats_start(opts, blockid):
    if opts.output != None:
        opts.output.write("\n" + blockid + ",")

def outfile_stats_end(opts, pagehead, blockhead, pagestats):
    if opts.output != None:
        line = [
            # page head
            pagehead.write_gen,
            pagehead.entries,
            pagehead.type.name,

            # block header
            blockhead.disk_size,

            # page_stats
            *pagestats.to_csv_cols(),
        ]
        opts.output.write(",".join(str(x) for x in line))

def wtdecode_file_object(b, opts, nbytes):
    p = Printer(b, opts)
    pagecount = 0
    if opts.offset == 0 and not opts.fragment:
        file_header_decode(p, b)
        startblock = (b.tell() + 0x1ff) & ~(0x1FF)
    else:
        startblock = opts.offset

    outfile_header(opts)

    while (nbytes == 0 or startblock < nbytes) and (opts.pages == 0 or pagecount < opts.pages):
        d_h = d_and_h(startblock)
        outfile_stats_start(opts, d_h)
        print('Decode at ' + d_h)
        b.seek(startblock)
        try:
            block_decode(p, b, nbytes, opts)
        except BrokenPipeError:
            break
        except Exception:
            p.rint(f'ERROR decoding block at {d_and_h(startblock)}')
            if opts.debug:
                traceback.print_exception(*sys.exc_info())
        p.rint('')
        pos = b.tell()
        pos = (pos + 0x1FF) & ~(0x1FF)
        if startblock == pos:
            startblock += 0x200
        else:
            startblock = pos
        pagecount += 1
    p.rint('')

def encode_bytes(f):
    lines = f.readlines()
    allbytes = bytearray()
    for line in lines:
        if ':' in line:
            (_, _, line) = line.rpartition(':')
        # Keep anything that looks like it could be hexadecimal,
        # remove everything else.
        nospace = re.sub(r'[^a-fA-F\d]', '', line)
        print('LINE (len={}): {}'.format(len(nospace), nospace))
        if len(nospace) > 0:
            print('first={}, last={}'.format(nospace[0], nospace[-1]))
            b = codecs.decode(nospace, 'hex')
            #b = bytearray.fromhex(line).decode()
            allbytes += b
    return allbytes

def wtdecode(opts):
    if opts.dumpin:
        opts.fragment = True
        if opts.filename == '-':
            allbytes = encode_bytes(sys.stdin)
        else:
            with open(opts.filename, "r") as infile:
                allbytes = encode_bytes(infile)
        b = binary_data.BinaryFile(io.BytesIO(allbytes))
        wtdecode_file_object(b, opts, len(allbytes))
    elif opts.filename == '-':
        nbytes = 0      # unknown length
        print('stdin, position ' + hex(opts.offset) + ', pagelimit ' +  str(opts.pages))
        wtdecode_file_object(binary_data.BinaryFile(sys.stdin.buffer), opts, nbytes)
    else:
        nbytes = os.path.getsize(opts.filename)
        print(opts.filename + ', position ' + hex(opts.offset) + '/' + hex(nbytes) + ', pagelimit ' +  str(opts.pages))
        with open(opts.filename, "rb") as fileobj:
            wtdecode_file_object(binary_data.BinaryFile(fileobj), opts, nbytes)

def get_arg_parser():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('-b', '--bytes', help="show bytes alongside decoding", action='store_true')
    parser.add_argument('--bson', help="decode cell values as bson data", action='store_true')
    parser.add_argument("-c", "--csv", type=argparse.FileType('w'), dest='output', help="output filename for summary of page statistics in CSV format", action='store')
    parser.add_argument('--continue', help="continue on checksum failure", dest='cont', action='store_true')
    parser.add_argument('-D', '--debug', help="debug this tool", action='store_true')
    parser.add_argument('-d', '--dumpin', help="input is hex dump (may be embedded in log messages)", action='store_true')
    parser.add_argument('--disagg', help="input comes from disaggregated storage", action='store_true')
    parser.add_argument('--ext', help="dump only the extent lists", action='store_true')
    parser.add_argument('-f', '--fragment', help="input file is a fragment, does not have a WT file header", action='store_true')
    parser.add_argument('-o', '--offset', help="seek offset before decoding", type=int, default=0)
    parser.add_argument('-p', '--pages', help="number of pages to decode", type=int, default=0)
    parser.add_argument('-v', '--verbose', help="print things about data, not just the headers", action='store_true')
    parser.add_argument('-s', '--split', help="split output to also show raw bytes", action='store_true')
    parser.add_argument('--skip-data', help="do not read/process data", action='store_true')
    parser.add_argument('-V', '--version', help="print version number of this program", action='store_true')
    parser.add_argument('filename', help="file name or '-' for stdin")
    return parser

# Only run the main code if this file is not imported.
if __name__ == '__main__':
    parser = get_arg_parser()
    opts = parser.parse_args()

    if opts.bson and not have_bson:
        print('ERROR: the pymongo bson library is required to decode bson data')
        sys.exit(1)

    if opts.version:
        print('wt_binary_decode version "{}"'.format(decode_version))
        sys.exit(0)

    try:
        wtdecode(opts)
    except KeyboardInterrupt:
        pass
    except BrokenPipeError:
        pass

    sys.exit(0)
