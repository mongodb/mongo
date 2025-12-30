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

import codecs, io, os, re, sys, traceback, pprint, json
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
    # Try to install it automatically
    print('python-snappy not found, attempting to install...')
    try:
        import subprocess
        subprocess.check_call([sys.executable, '-m', 'pip', 'install', 'python-snappy'])
        import snappy
        have_snappy = True
        print('Successfully installed python-snappy')
    except Exception as e:
        print(f'Warning: Failed to install python-snappy: {e}')
        print('Compressed pages will not be readable.')

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
        print(pfx + str(s))

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
        result += f'<packed {binary_data.d_and_h(val)}>'
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

def decode_snappy_varint(data):
    """
    Decode the uncompressed length from snappy-compressed data.
    Snappy uses a variable-length encoding for the uncompressed size at the start.
    Returns (uncompressed_length, bytes_used) or (None, 0) if invalid.
    """
    if len(data) == 0:
        return (None, 0)

    result = 0
    shift = 0
    for i, byte in enumerate(data[:5]):  # Varint is max 5 bytes for 32-bit length
        result |= (byte & 0x7f) << shift
        if (byte & 0x80) == 0:
            return (result, i + 1)
        shift += 7
    return (None, 0)  # Invalid varint

def print_snappy_diagnostics(p, compressed_data, stored_length, pagehead, compress_skip):
    """Print detailed diagnostics about invalid compressed data."""
    p.rint('? Decompression of the block failed, analyzing compressed data:')
    p.rint(f'??  Compressed data length: {len(compressed_data)} bytes')
    p.rint(f'??  Stored length from WiredTiger prefix: {stored_length} (0x{stored_length:x})')

    # Analyze the snappy header
    uncompressed_len, varint_bytes = decode_snappy_varint(compressed_data)
    if uncompressed_len:
        p.rint(f'??  Snappy header claims: {uncompressed_len} bytes uncompressed (varint: {varint_bytes} bytes)')
        expected_uncompressed = pagehead.mem_size - compress_skip
        p.rint(f'??  Page header expects: {expected_uncompressed} bytes uncompressed')

        if abs(uncompressed_len - expected_uncompressed) > 100:
            p.rint(f'??  WARNING: size mismatch of {abs(uncompressed_len - expected_uncompressed)} bytes!')
    else:
        p.rint(f'??  ERROR: could not decode snappy varint header')

    # Try the full decompression first to get detailed error message
    try:
        snappy.uncompress(compressed_data)
        # If we get here, decompression succeeded (shouldn't happen if we're in diagnostics)
        p.rint(f'??  WARNING: Full decompression unexpectedly succeeded')
    except snappy.UncompressError as e:
        # Try to extract the underlying error message from the exception chain
        error_details = ""
        if e.__cause__ is not None:
            error_details = str(e.__cause__)
        elif e.__context__ is not None:
            error_details = str(e.__context__)
        else:
            # Fallback to the exception itself
            error_details = str(e)

        # Parse error message for corruption details
        import re
        dst_match = re.search(r'dst position:\s*(\d+)', error_details)
        offset_match = re.search(r'offset\s+(\d+)', error_details)

        if dst_match:
            dst_pos = int(dst_match.group(1))
            p.rint(f'??  Error at output position: {dst_pos} bytes')
            if uncompressed_len:
                percent = (dst_pos / uncompressed_len) * 100
                p.rint(f'??  Successfully decompressed: {dst_pos} / {uncompressed_len} bytes ({percent:.1f}%)')
            else:
                p.rint(f'??  Successfully decompressed: {dst_pos} bytes before failure')

        if offset_match:
            bad_offset = int(offset_match.group(1))
            p.rint(f'??  Invalid backreference: Snappy tried to copy from output offset {bad_offset}')
            if dst_match:
                if bad_offset > dst_pos:
                    p.rint(f'??  ERROR: backreference offset {bad_offset} exceeds decompressed data {dst_pos} by {bad_offset - dst_pos} bytes')
                p.rint(f'??  Corruption occurred in compressed stream while decompressing bytes 0-{dst_pos}')

            # Note: The backreference offset may exceed compressed data length - that's expected
            # because it refers to a position in the OUTPUT buffer, not the input stream
            if bad_offset > len(compressed_data):
                p.rint(f'??  Note: backreference offset ({bad_offset}) > compressed size ({len(compressed_data)}) is expected')
                p.rint(f'??       (offset refers to output buffer position, not input position)')

        if error_details and not (dst_match or offset_match):
            p.rint(f'??  Error details: {error_details}')

    # Show first bytes for debugging
    if len(compressed_data) >= 32:
        p.rint(f'??  first 32 bytes: {compressed_data[:32].hex(" ")}')

def block_decode(p, b, nbytes, opts):
    disk_pos = b.tell()

    # Switch the printer and the binary stream to work on the page data as opposed to working on
    # the file itself. We need to do this to support compressed blocks. As a consequence, offsets
    # printed in the split mode are relative to a (potentially uncompressed) page, rather than
    # the file.
    if opts.disagg:
        # Size of WT_PAGE_HEADER
        page_data = bytearray(b.read(44))
    else:
        # Size of WT_PAGE_HEADER + size of WT_BLOCK_HEADER
        page_data = bytearray(b.read(40))
    b.saved_bytes()
    b_page = binary_data.BinaryFile(io.BytesIO(page_data))
    p = Printer(b_page, opts)

    # WT_PAGE_HEADER in btmem.h (28 bytes)
    pagehead = btree_format.PageHeader.parse(b_page)
    # WT_BLOCK_HEADER in block.h (12 bytes or 44 bytes)
    if opts.disagg:
        blockhead = btree_format.BlockDisaggHeader.parse(b_page)
    else:
        blockhead = btree_format.BlockHeader.parse(b_page)

    if pagehead.unused != 0:
        p.rint('? garbage in unused bytes')
        return
    if pagehead.type == btree_format.PageType.WT_PAGE_INVALID:
        p.rint('? invalid page')
        return

    p.rint(pagehead)

    if blockhead.unused != 0:
        p.rint('garbage in unused bytes')
        return

    disk_size = nbytes if opts.disagg else blockhead.disk_size

    if disk_size > 17 * 1024 * 1024:
        # The maximum document size in MongoDB is 16MB. Larger block sizes are suspect.
        p.rint('the block is too big')
        return
    if disk_size < 40 and not opts.disagg:
        # The disk size is too small
        return

    p.rint(blockhead)

    pagestats = PageStats()

    # Verify the checksum
    if have_crc32c:
        savepos = b.tell()
        b.seek(disk_pos)
        if blockhead.flags & btree_format.BlockFlags.WT_BLOCK_DATA_CKSUM != 0:
            check_size = disk_size
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

    if skip_data:
        b.seek(disk_pos + disk_size)
        return

    # Read the block contents
    payload_pos = b.tell()
    header_length = payload_pos - disk_pos
    if pagehead.flags & btree_format.PageFlags.WT_PAGE_COMPRESSED:
        if not have_snappy:
            raise ModuleNotFoundError('python-snappy is required to decode compressed pages')
        try:
            compress_skip = 64
            # The first few bytes are uncompressed
            payload_data = bytearray(b.read(compress_skip - header_length))
            # Read the length of the remaining data
            compressed_byte_count = b.read_uint64()
            calculated_length = disk_size - compress_skip - 8
            lengths_match = (compressed_byte_count == calculated_length)

            # Read the maximum possible amount of compressed data
            compressed_data_full = b.read(max(calculated_length, compressed_byte_count))
            b.seek(disk_pos + disk_size)

            # Try decompression with both sizes, preferring the stored length first
            decompressed = None

            # Try stored length first (most likely to be correct)
            if compressed_byte_count <= len(compressed_data_full):
                p.rint_v(f'Trying to decompress using stored length: {compressed_byte_count} bytes')
                compressed_data = compressed_data_full[:compressed_byte_count]
                if snappy.isValidCompressed(compressed_data):
                    try:
                        decompressed = snappy.uncompress(compressed_data)
                        if not lengths_match:
                            p.rint_v(f'  Successfully decompressed using stored length ({compressed_byte_count} bytes)')
                    except:
                        pass

            # If that failed and lengths differ, try calculated length
            if decompressed is None and not lengths_match and calculated_length <= len(compressed_data_full):
                p.rint_v(f'Trying to decompress using calculated length: {calculated_length} bytes')
                compressed_data = compressed_data_full[:calculated_length]
                if snappy.isValidCompressed(compressed_data):
                    try:
                        decompressed = snappy.uncompress(compressed_data)
                        p.rint_v(f'  Successfully decompressed using calculated length ({calculated_length} bytes)')
                    except:
                        pass

            # If any attempt succeeded, use the result
            if decompressed is not None:
                payload_data.extend(decompressed)
            else:
                # Both failed - print diagnostics and stop processing this block
                # Use the stored length for diagnostics as it's more likely to be correct
                compressed_data = compressed_data_full[:min(compressed_byte_count, len(compressed_data_full))]
                print_snappy_diagnostics(p, compressed_data, compressed_byte_count, pagehead, compress_skip)
                return  # Stop processing this corrupted block
        except:
            p.rint('? The page failed to uncompress')
            if opts.debug:
                traceback.print_exception(*sys.exc_info())
            return
    else:
        payload_data = b.read(pagehead.mem_size - header_length)
        b.seek(disk_pos + disk_size)

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
        row_decode(p, b_page, pagehead, pagestats, opts)
    elif pagehead.type == btree_format.PageType.WT_PAGE_OVFL:
        # Use b_page.read() so that we can also print the raw bytes in the split mode
        p.rint_v(raw_bytes(b_page.read(len(payload_data))))
    else:
        p.rint_v('? unimplemented decode for page type {}'.format(pagehead.type))
        p.rint_v(binary_to_pretty_string(payload_data))

    outfile_stats_end(opts, pagehead, blockhead, pagestats)

# Decode the contents of a cell 
def row_decode(p, b, pagehead, pagestats, opts):
    for cellnum in range(0, pagehead.entries):
        cellpos = b.tell()
        if cellpos >= pagehead.mem_size:
            p.rint_v('** OVERFLOW memsize **')
            return
        p.begin_cell(cellnum)

        try:
            cell = btree_format.Cell.parse(b, True)
            
            p.rint_v(cell.descriptor_string())
            if cell.has_timestamps():
                process_timestamps(p, cell, pagestats)

            if cell.is_key:
                pagestats.num_keys += 1
                pagestats.keys_sz += len(cell.data)
            
            # If the cell cannot be decoded as a valid type, dump the raw bytes and raise an error.
            if not cell.is_valid_type():
                dumpraw(p, b, cellpos)
                raise ValueError('Unexpected cell type')

            p.rint_v(cell.type_string())
            
            # Print the contents of the cell.
            try:
                # Attempt the decode the cell as BSON.
                if (cell.is_value and opts.bson and have_bson):
                    decoded_data = bson.BSON(cell.data).decode()
                    p.rint_v(pprint.pformat(decoded_data, indent=2))
                # If the cell is an address and we're in disagg mode, print the cell as a DisaggAddr
                # type.
                elif cell.is_address and opts.disagg:
                    addr = btree_format.DisaggAddr.parse(cell.data)
                    p.rint(json.dumps(addr.__dict__))
                else:
                    p.rint_v(raw_bytes(cell.data))
            except bson.InvalidBSON as e:
                p.rint_v(f"cannot decode cell as BSON: {e}")
                p.rint_v(raw_bytes(cell.data))
            except (IndexError, ValueError):
                # FIXME-WT-13000 theres a bug in raw_bytes
                pass

        finally:
            p.end_cell()

def extlist_decode(p, b, pagehead):
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
        d_h = binary_data.d_and_h(startblock)
        outfile_stats_start(opts, d_h)
        print('Decode at ' + d_h)
        b.seek(startblock)
        try:
            block_decode(p, b, nbytes, opts)
        except BrokenPipeError:
            break
        except ModuleNotFoundError as e:
            # We're missing snappy compression support. No point continuing from here.
            p.rint('ERROR: ' + str(e))
            exit(1)
        except Exception:
            p.rint(f'ERROR decoding block at {binary_data.d_and_h(startblock)}')
            if opts.debug:
                traceback.print_exception(*sys.exc_info())
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
        if opts.debug:
            print('LINE (len={}): {}'.format(len(nospace), nospace))
        if len(nospace) > 0:
            if opts.debug:
                print('first={}, last={}'.format(nospace[0], nospace[-1]))
            b = codecs.decode(nospace, 'hex')
            #b = bytearray.fromhex(line).decode()
            allbytes += b
    return allbytes

def extract_mongodb_log_hex(f):
    """
    Extract hex dump from MongoDB log file containing checksum mismatch errors.
    Looks for __bm_corrupt_dump messages and extracts all hex chunks.
    Returns the bytes from the __first__ complete checksum mismatch found.
    """
    import json

    lines = f.readlines()
    current_chunks = []
    block_info = None

    for line_num, line in enumerate(lines):
        try:
            log_entry = json.loads(line)
            msg = log_entry.get('attr', {}).get('message', {})

            # Check if this is a corrupt dump message
            if isinstance(msg, dict) and '__bm_corrupt_dump' in msg.get('msg', ''):
                # Extract block info and hex data
                msg_text = msg.get('msg', '')

                # Parse the block info: {offset, size, checksum}: (chunk N of M): hexdata
                match = re.search(r'\{0:\s*(\d+),\s*(\d+),\s*(0x[0-9a-f]+)\}:\s*\(chunk\s+(\d+)\s+of\s+(\d+)\):\s*([0-9a-f\s]+)', msg_text)
                if match:
                    offset, size, checksum, chunk_num, total_chunks, hexdata = match.groups()
                    chunk_num = int(chunk_num)
                    total_chunks = int(total_chunks)

                    if chunk_num == 1:
                        # Start of a new block
                        if current_chunks and len(current_chunks) == block_info[4]:
                            # We have a complete previous block, return it
                            if (opts.debug):
                                print(f'Found complete checksum mismatch block: offset={block_info[0]}, size={block_info[1]}, checksum={block_info[2]}')
                            return b''.join(current_chunks)

                        # Reset for new block
                        current_chunks = []
                        block_info = (offset, size, checksum, chunk_num, total_chunks)
                        if (opts.debug):
                            print(f'Found checksum mismatch at line: {line_num} for block with address: offset {offset}, size {size}, checksum {checksum} ({total_chunks} chunks)')

                    # Add this chunk
                    hexdata_clean = re.sub(r'[^0-9a-f]', '', hexdata.lower())
                    if hexdata_clean:
                        current_chunks.append(codecs.decode(hexdata_clean, 'hex'))

                    # Check if block is complete
                    if len(current_chunks) == total_chunks:
                        if (opts.debug):
                            print(f'Complete block collected: {len(current_chunks)} chunks')
                        return b''.join(current_chunks)
        except json.JSONDecodeError:
            # If we don't have a JSON log line, then this isn't a MongoDB log.
            return encode_bytes(f)
        except Exception as e:
            if opts.debug:
                print(f'Error parsing line {line_num}: {e}')

    # Return any incomplete block we collected
    if current_chunks:
        print(f'Warning: Returning incomplete block with {len(current_chunks)} chunks (expected {block_info[4] if block_info else "unknown"})')
        return b''.join(current_chunks)

    # No checksum mismatch found
    print('Error: No checksum mismatch found in log file')
    return bytearray()

def wtdecode(opts):
    if opts.dumpin:
        opts.fragment = True
        if opts.filename == '-':
            allbytes = extract_mongodb_log_hex(sys.stdin)
        else:
            with open(opts.filename, "r") as infile:
                allbytes = extract_mongodb_log_hex(infile)
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
