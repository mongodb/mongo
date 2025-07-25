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

# Tools and data structures for reading and decoding the on-disk format of WiredTiger's files.

import enum, typing
from py_common import binary_data


#
# Block File Header
#

class BlockFileHeader(object):
    '''
    A .wt block file header (WT_BLOCK_DESC).
    '''
    magic: int
    major: int
    minor: int
    checksum: int
    unused: int

    # Constants
    WT_BLOCK_MAGIC: typing.Final[int] = 120897
    WT_BLOCK_MAJOR_VERSION: typing.Final[int] = 1
    WT_BLOCK_MINOR_VERSION: typing.Final[int] = 0

    def __init__(self) -> None:
        '''
        Initialize the instance with default values.
        '''
        self.magic = 0
        self.major = 0
        self.minor = 0
        self.checksum = 0
        self.unused = 0

    @staticmethod
    def parse(b: binary_data.BinaryFile) -> 'BlockFileHeader':
        '''
        Parse the file header.
        '''
        # WT_BLOCK_DESC in block.h (16 bytes)
        h = BlockFileHeader()
        h.magic = b.read_uint32()
        h.major = b.read_uint16()
        h.minor = b.read_uint16()
        h.checksum = b.read_uint32()
        h.unused = b.read_uint32()
        return h


#
# Page
#

class PageType(enum.Enum):
    '''
    Page types from btmem.h.
    '''
    WT_PAGE_INVALID = 0
    WT_PAGE_BLOCK_MANAGER = 1
    WT_PAGE_COL_FIX = 2
    WT_PAGE_COL_INT = 3
    WT_PAGE_COL_VAR = 4
    WT_PAGE_OVFL = 5
    WT_PAGE_ROW_INT = 6
    WT_PAGE_ROW_LEAF = 7


class PageHeader(object):
    '''
    A page header (WT_PAGE_HEADER).
    '''
    recno: int
    write_gen: int
    memsize: int
    entries: int # Or: overflow data length
    type: PageType
    flags: int
    unused: int
    version: int

    # Flags
    WT_PAGE_COMPRESSED: typing.Final[int] = 0x01
    WT_PAGE_EMPTY_V_ALL: typing.Final[int] = 0x02
    WT_PAGE_EMPTY_V_NONE: typing.Final[int] = 0x04
    WT_PAGE_ENCRYPTED: typing.Final[int] = 0x08
    WT_PAGE_UNUSED: typing.Final[int] = 0x10
    WT_PAGE_FT_UPDATE: typing.Final[int] = 0x20

    def __init__(self) -> None:
        '''
        Initialize the instance with default values.
        '''
        self.recno = 0
        self.write_gen = 0
        self.mem_size = 0
        self.entries = 0
        self.type = PageType.WT_PAGE_INVALID
        self.flags = 0
        self.unused = 0
        self.version = 0

    @staticmethod
    def parse(b: binary_data.BinaryFile) -> 'PageHeader':
        '''
        Parse a page header.
        '''
        # WT_PAGE_HEADER in btmem.h (28 bytes)
        h = PageHeader()
        h.recno = b.read_uint64()
        h.write_gen = b.read_uint64()
        h.mem_size = b.read_uint32()
        h.entries = b.read_uint32()
        h.type = PageType(b.read_uint8())
        h.flags = b.read_uint8()
        h.unused = b.read_uint8()
        h.version = b.read_uint8()
        return h

#
# Block
#

class BlockHeader(object):
    '''
    A block header (WT_BLOCK_HEADER).
    '''
    disk_size: int
    checksum: int
    flags: int
    unused: int

    # Flags
    WT_BLOCK_DATA_CKSUM: typing.Final[int] = 0x1
    WT_BLOCK_DISAGG_ENCRYPTED: typing.Final[int] = 0x2  # disagg only
    WT_BLOCK_DISAGG_COMPRESSED: typing.Final[int] = 0x4 # disagg only

    def __init__(self) -> None:
        '''
        Initialize the instance with default values.
        '''
        self.disk_size = 0
        self.checksum = 0
        self.flags = 0
        self.unused = 0

    @staticmethod
    def parse(b: binary_data.BinaryFile, disagg = False) -> 'BlockHeader':
        '''
        Parse a block header.
        '''
        # WT_BLOCK_HEADER in block.h (12 bytes)
        h = BlockHeader()
        if disagg:
            # Disagg sets additional fields.  If they are examined
            # by non-disagg code, an exception will be thrown (by design).
            h.disagg_magic = b.read_uint8()
            h.disagg_version = b.read_uint8()
            h.disagg_compatible_version = b.read_uint8()
            h.disagg_header_size = b.read_uint8()
            h.checksum = b.read_uint32()
            h.disagg_previous_checksum = b.read_uint32()
            h.disagg_reconciliation_id = b.read_uint8()
            h.flags = b.read_uint8()
            h.unused = int.from_bytes(b.read(2), byteorder='little')
        else:
            h.disk_size = b.read_uint32()
            h.checksum = b.read_uint32()
            h.flags = b.read_uint8()
            h.unused = int.from_bytes(b.read(3), byteorder='little')
        return h


#
# Cell
#

class CellType(enum.Enum):
    '''
    Cell types from cell.h.
    '''
    WT_CELL_ADDR_DEL = 0
    WT_CELL_ADDR_INT = 1
    WT_CELL_ADDR_LEAF = 2
    WT_CELL_ADDR_LEAF_NO = 3
    WT_CELL_DEL = 4
    WT_CELL_KEY = 5
    WT_CELL_KEY_OVFL = 6
    WT_CELL_KEY_OVFL_RM = 12
    WT_CELL_KEY_PFX = 7
    WT_CELL_VALUE = 8
    WT_CELL_VALUE_COPY = 9
    WT_CELL_VALUE_OVFL = 10
    WT_CELL_VALUE_OVFL_RM = 11


class Cell(object):
    '''
    A cell in a WiredTiger table.
    '''
    # Cell format from cell.h (maximum of 71 bytes):
    #  1: cell descriptor byte
    #  1: prefix compression count
    #  1: secondary descriptor byte
    # 36: 4 timestamps (uint64_t encoding, max 9 bytes)
    # 18: 2 transaction IDs (uint64_t encoding, max 9 bytes)
    #  9: associated 64-bit value (uint64_t encoding, max 9 bytes)
    #  5: data length (uint32_t encoding, max 5 bytes)
    descriptor: int
    prefix_compression_count: int
    extra_descriptor: int
    data: bytes

    cell_type: typing.Optional[CellType]
    prefix: typing.Optional[int]
    run_length: typing.Optional[int]

    is_address: bool
    is_key: bool
    is_overflow: bool
    is_short: bool
    is_unsupported: bool
    is_value: bool

    # Timestamps & transactions
    durable_start_ts: typing.Optional[int]
    durable_stop_ts: typing.Optional[int]
    start_ts: typing.Optional[int]
    stop_ts: typing.Optional[int]
    start_txn: typing.Optional[int]
    stop_txn: typing.Optional[int]

    # Sizes of the various timestamp & transaction fields (track this for statistics)
    size_durable_start_ts: int
    size_durable_stop_ts: int
    size_start_ts: int
    size_stop_ts: int
    size_start_txn: int
    size_stop_txn: int

    # Constants and flags for the descriptor byte
    WT_CELL_KEY_SHORT: typing.Final[int] = 0x01
    WT_CELL_KEY_SHORT_PFX: typing.Final[int] = 0x02
    WT_CELL_VALUE_SHORT: typing.Final[int] = 0x03
    WT_CELL_64V: typing.Final[int] = 0x04
    WT_CELL_SECOND_DESC: typing.Final[int] = 0x08

    # Flags for the extra descriptor byte
    WT_CELL_PREPARE: typing.Final[int] = 0x01
    WT_CELL_TS_DURABLE_START: typing.Final[int] = 0x02
    WT_CELL_TS_DURABLE_STOP: typing.Final[int] = 0x04
    WT_CELL_TS_START: typing.Final[int] = 0x08
    WT_CELL_TS_STOP: typing.Final[int] = 0x10
    WT_CELL_TXN_START: typing.Final[int] = 0x20
    WT_CELL_TXN_STOP: typing.Final[int] = 0x40

    def __init__(self) -> None:
        '''
        Initialize the instance with default values.
        '''
        self.descriptor = 0
        self.prefix_compression_count = 0
        self.extra_descriptor = 0
        self.data = bytes()

        self.cell_type = None
        self.prefix = None
        self.run_length = None

        self.is_address = False
        self.is_key = False
        self.is_overflow = False
        self.is_short = False
        self.is_unsupported = False
        self.is_value = False

        self.durable_start_ts = None
        self.durable_stop_ts = None
        self.start_ts = None
        self.stop_ts = None
        self.start_txn = None
        self.stop_txn = None

    def _parse_timestamps(self, b: binary_data.BinaryFile):
        '''
        Parse timestamps.
        '''
        if self.extra_descriptor == 0:
            return

        if self.extra_descriptor & Cell.WT_CELL_TS_START != 0:
            self.start_ts, self.size_start_ts = b.read_packed_uint64_with_size()
        if self.extra_descriptor & Cell.WT_CELL_TXN_START != 0:
            self.start_txn, self.size_start_txn = b.read_packed_uint64_with_size()
        if self.extra_descriptor & Cell.WT_CELL_TS_DURABLE_START != 0:
            self.durable_start_ts, self.size_durable_start_ts = b.read_packed_uint64_with_size()

        if self.extra_descriptor & Cell.WT_CELL_TS_STOP != 0:
            self.stop_ts, self.size_stop_ts = b.read_packed_uint64_with_size()
        if self.extra_descriptor & Cell.WT_CELL_TXN_STOP != 0:
            self.stop_txn, self.size_stop_txn = b.read_packed_uint64_with_size()
        if self.extra_descriptor & Cell.WT_CELL_TS_DURABLE_STOP != 0:
            self.durable_stop_ts, self.size_durable_stop_ts = b.read_packed_uint64_with_size()

        if self.durable_start_ts is not None:
            self.durable_start_ts += self.start_ts if self.start_ts is not None else 0
        if self.stop_ts is not None:
            self.stop_ts += self.start_ts if self.start_ts is not None else 0
        if self.stop_txn is not None:
            self.stop_txn += self.start_txn if self.start_txn is not None else 0
        if self.durable_stop_ts is not None:
            self.durable_stop_ts += self.stop_ts if self.stop_ts is not None else 0

        if self.extra_descriptor & 0x80:
            raise ValueError('Junk in extra descriptor: ' + hex(self.extra_descriptor))

    @staticmethod
    def parse(b: binary_data.BinaryFile, ignore_unsupported: bool = False) -> 'Cell':
        '''
        Parse a cell.
        '''
        cell = Cell()
        cell.descriptor = b.read_uint8()

        short = cell.descriptor & 0x3
        if short == 0:
            if cell.descriptor & Cell.WT_CELL_SECOND_DESC != 0:
                # Bit 4 marks a value with an additional descriptor byte. If this flag is set,
                # the next byte after the initial cell byte is an additional description byte.
                # The bottom bit in this additional byte indicates that the cell is part of a
                # prepared, and not yet committed transaction. The next 6 bits describe a validity
                # and durability window of timestamp/transaction IDs. The top bit is currently
                # unused.
                cell.extra_descriptor = b.read_uint8()
                cell._parse_timestamps(b)

            if cell.descriptor & Cell.WT_CELL_64V != 0:
                # Bit 3 marks an 8B packed, uint64_t value following the cell description byte.
                # (A run-length counter or a record number for variable-length column store.)
                cell.run_length = b.read_packed_uint64()

            # Bits 5-8 are cell "types".
            cell.cell_type = CellType((cell.descriptor & 0xf0) >> 4)

            if cell.cell_type == CellType.WT_CELL_VALUE:
                if cell.extra_descriptor != 0:
                    # If there is an extra descriptor byte, the length is a regular encoded int.
                    l = b.read_packed_uint64()
                else:
                    l = b.read_long_length()
                cell.is_value = True
            elif cell.cell_type == CellType.WT_CELL_KEY:
                # 64 is WT_CELL_SIZE_ADJUST. If the size was less than that, we would have used the
                # "short" packing.
                l = b.read_long_length()
                cell.is_key = True
            elif cell.cell_type == CellType.WT_CELL_ADDR_LEAF_NO:
                l = b.read_packed_uint64()
                cell.is_address = True
            elif cell.cell_type == CellType.WT_CELL_KEY_PFX:
                #TODO: not right...
                cell.prefix = b.read_uint8()
                l = b.read_long_length()
                cell.is_key = True
            elif cell.cell_type == CellType.WT_CELL_KEY_OVFL:
                #TODO: support RLE
                #TODO: decode the address cookie
                l = b.read_packed_uint64()
                cell.is_key = True
                cell.is_overflow = True
            elif cell.cell_type == CellType.WT_CELL_VALUE_OVFL:
                #TODO: support SECOND DESC and RLE
                #TODO: decode the address cookie
                l = b.read_packed_uint64()
                cell.is_overflow = True
                cell.is_value = True
            else:
                l = 0
                cell.is_unsupported = True
                if not ignore_unsupported:
                        raise ValueError('celltype = {} ({}) not implemented' \
                                         .format(cell.cell_type.value, cell.cell_type.name))
        elif short == Cell.WT_CELL_KEY_SHORT:
            l = (cell.descriptor & 0xfc) >> 2
            cell.is_key = True
            cell.is_short = True
        elif short == Cell.WT_CELL_KEY_SHORT_PFX:
            l = (cell.descriptor & 0xfc) >> 2
            cell.is_key = True
            cell.is_short = True
            cell.prefix = b.read_uint8()
        elif short == Cell.WT_CELL_VALUE_SHORT:
            l = (cell.descriptor & 0xfc) >> 2
            cell.is_short = True
            cell.is_value = True
        else:
            assert(False)

        cell.data = b.read(l)
        return cell

    @property
    def prepared(self) -> bool:
        '''
        Check if this cell belongs to a prepared transaction.
        '''
        return self.extra_descriptor & Cell.WT_CELL_PREPARE != 0

