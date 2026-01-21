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
import pprint
import enum
import io
import json
import logging
from dataclasses import dataclass
from typing import Optional, List, Union, Final

# Tools and data structures for reading and decoding the on-disk format of WiredTiger's files.
from py_common import binary_data
from py_common.stats import PageStats
from py_common.printer import Printer, binary_to_pretty_string, raw_bytes, dumpraw
from py_common.snappy_util import snappy_decompress_page

logger = logging.getLogger(__name__)

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
    WT_BLOCK_MAGIC: Final[int] = 120897
    WT_BLOCK_MAJOR_VERSION: Final[int] = 1
    WT_BLOCK_MINOR_VERSION: Final[int] = 0

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
    WT_PAGE_COL_INT = 3
    WT_PAGE_COL_VAR = 4
    WT_PAGE_OVFL = 5
    WT_PAGE_ROW_INT = 6
    WT_PAGE_ROW_LEAF = 7

class PageFlags(enum.IntFlag):
    '''
    Page flags from btmem.h.
    '''
    WT_PAGE_COMPRESSED = 0x01
    WT_PAGE_EMPTY_V_ALL = 0x02
    WT_PAGE_EMPTY_V_NONE = 0x04
    WT_PAGE_ENCRYPTED = 0x08
    WT_PAGE_UNUSED = 0x10
    WT_PAGE_FT_UPDATE = 0x20

class PageHeader(object):
    '''
    A page header (WT_PAGE_HEADER).
    '''
    recno: int
    write_gen: int
    mem_size: int
    entries: int # Or: overflow data length
    type: PageType
    flags: PageFlags
    unused: int
    version: int

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
        h.flags = PageFlags(b.read_uint8())
        h.unused = b.read_uint8()
        h.version = b.read_uint8()
        return h

    def __str__(self):
        header_string = (
            f"Page Header:\n"
            f"  recno: {str(self.recno)}\n"
            f"  writegen: {str(self.write_gen)}\n"
            f"  memsize: {str(self.mem_size)}\n"
            f"  ncells (overflow len): {str(self.entries)}\n"
            f"  page type: {str(self.type.value)} ({self.type.name})\n"
            f"  page flags: {str(self.flags)}\n"
            f"  version: {str(self.version)}"
        )
        
        return header_string
#
# Block
#

class BlockFlags(enum.IntFlag):
    '''
    Block flags from block.h
    '''
    WT_BLOCK_DATA_CKSUM = 0x1

class BlockHeader(object):
    '''
    A block header (WT_BLOCK_HEADER).
    '''
    disk_size: int
    checksum: int
    flags: BlockFlags
    unused: int

    def __init__(self) -> None:
        '''
        Initialize the instance with default values.
        '''
        self.disk_size = 0
        self.checksum = 0
        self.flags = 0
        self.unused = 0

    @staticmethod
    def parse(b: binary_data.BinaryFile) -> 'BlockHeader':
        '''
        Parse a block header.
        '''
        # WT_BLOCK_HEADER in block.h (12 bytes)
        h = BlockHeader()
        h.disk_size = b.read_uint32()
        h.checksum = b.read_uint32()
        h.flags = BlockFlags(b.read_uint8())
        h.unused = int.from_bytes(b.read(3), byteorder='little')
        return h
    
    def __str__(self):
        header_string = (
            f"Block Header:\n"
            f"  disk_size: {str(self.disk_size)}\n"
            f"  checksum: {str(self.checksum)}\n"
            f"  flags: {str(self.flags)}"
        )
        return header_string

class BlockDisaggFlags(enum.IntFlag):
    '''
    Disagg block flags from block.h
    '''
    WT_BLOCK_DISAGG_DATA_CKSUM = 0x1
    WT_BLOCK_DISAGG_ENCRYPTED = 0x2
    WT_BLOCK_DISAGG_COMPRESSED = 0x4

class BlockDisaggHeader(object):
    '''
    A block header (WT_BLOCK_DISAGG_HEADER). Disagg uses additional header fields in the block 
    header in comparison to standard WiredTiger blocks. This class should only be used for disagg 
    blocks.
    '''
    magic: int
    version: int
    compatible_version: int
    header_size: int
    checksum: int
    previous_checksum: int
    flags: BlockDisaggFlags
    unused: int

    # Block types (magic byte)
    WT_BLOCK_DISAGG_MAGIC_BASE: Final[int] = 0xdb
    WT_BLOCK_DISAGG_MAGIC_DELTA: Final[int] = 0xdd

    def __init__(self) -> None:
        '''
        Initialize the instance with default values.
        '''
        self.magic = 0
        self.version = 0
        self.compatible_version = 0
        self.header_size = 0
        self.checksum = 0
        self.previous_checksum = 0
        self.flags = 0
        self.unused = 0

    @staticmethod
    def parse(b: binary_data.BinaryFile, disagg = False) -> 'BlockDisaggHeader':
        '''
        Parse a block header.
        '''
        # WT_BLOCK_DISAGG_HEADER in block.h (16 bytes)
        h = BlockDisaggHeader()
        h.magic = b.read_uint8()
        h.version = b.read_uint8()
        h.compatible_version = b.read_uint8()
        h.header_size = b.read_uint8()
        h.checksum = b.read_uint32()
        h.previous_checksum = b.read_uint32()
        h.flags = BlockDisaggFlags(b.read_uint8())
        h.unused = int.from_bytes(b.read(2), byteorder='little')
        return h
    
    def __str__(self):
        header_string = (
            f"Block Disagg Header:\n"
            f"  magic: {hex(self.magic)} ({'delta' if self.magic == self.WT_BLOCK_DISAGG_MAGIC_DELTA else 'full image'})\n"
            f"  version: {str(self.version)}\n"
            f"  compatible_version: {str(self.compatible_version)}\n"
            f"  header_size: {str(self.header_size)}\n"
            f"  checksum: {str(self.checksum)}\n"
            f"  previous_checksum: {str(self.previous_checksum)}\n"
            f"  flags: {str(self.flags)}"
        )
        
        return header_string
    
#
# Extent List
#

class ExtentItem(object):
    '''
    An extent list item from a block manager page (written by block_ext.c).
    Each item consists of an offset and size, both packed uint64 values.
    '''
    offset: int
    size: int
    extra_stuff: str

    # Constants
    WT_BLOCK_EXTLIST_MAGIC: Final[int] = 71002

    def __init__(self) -> None:
        '''
        Initialize the instance with default values.
        '''
        self.offset = 0
        self.size = 0

    @staticmethod
    def parse(b: binary_data.BinaryFile) -> 'ExtentItem':
        '''
        Parse an extent list item.
        '''
        item = ExtentItem()
        item.offset = b.read_packed_uint64()
        item.size = b.read_packed_uint64()
        return item

    def is_magic(self) -> bool:
        '''
        Check if this is the magic number entry (first entry in the list).
        '''
        return self.offset == ExtentItem.WT_BLOCK_EXTLIST_MAGIC and self.size == 0

    def is_end_of_list(self) -> bool:
        '''
        Check if this is an end of list marker (offset == 0).
        '''
        return self.offset == 0

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

    cell_type: Optional[CellType]
    prefix: Optional[int]
    run_length: Optional[int]

    is_address: bool
    is_key: bool
    is_overflow: bool
    is_short: bool
    is_unsupported: bool
    is_value: bool

    # Timestamps & transactions
    durable_start_ts: Optional[int]
    durable_stop_ts: Optional[int]
    start_ts: Optional[int]
    stop_ts: Optional[int]
    start_txn: Optional[int]
    stop_txn: Optional[int]

    # Sizes of the various timestamp & transaction fields (track this for statistics)
    size_durable_start_ts: int
    size_durable_stop_ts: int
    size_start_ts: int
    size_stop_ts: int
    size_start_txn: int
    size_stop_txn: int

    # Constants and flags for the descriptor byte
    WT_CELL_KEY_SHORT: Final[int] = 0x01
    WT_CELL_KEY_SHORT_PFX: Final[int] = 0x02
    WT_CELL_VALUE_SHORT: Final[int] = 0x03
    WT_CELL_64V: Final[int] = 0x04
    WT_CELL_SECOND_DESC: Final[int] = 0x08

    # Flags for the extra descriptor byte
    WT_CELL_PREPARE: Final[int] = 0x01
    WT_CELL_TS_DURABLE_START: Final[int] = 0x02
    WT_CELL_TS_DURABLE_STOP: Final[int] = 0x04
    WT_CELL_TS_START: Final[int] = 0x08
    WT_CELL_TS_STOP: Final[int] = 0x10
    WT_CELL_TXN_START: Final[int] = 0x20
    WT_CELL_TXN_STOP: Final[int] = 0x40

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

    def has_timestamps(self) -> bool:
        return self.extra_descriptor != 0
    
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
    
    def descriptor_string(self) -> str:
        desc_str = f'desc: 0x{self.descriptor:x} '
        if self.extra_descriptor != 0:
            desc_str += f'extra: 0x{self.extra_descriptor:x} '
            # process_timestamps(p, cell, pagestats)
        if self.run_length is not None:
            desc_str += f'runlength/addr: {binary_data.d_and_h(self.run_length)} '
        
        return desc_str
    
    def type_string(self) -> str:
        type_str = '? unknown type'
        if self.is_address:
            type_str = 'addr (leaf no-overflow) '
        elif self.is_key:
            type_str = 'key '
        elif self.is_value:
            type_str = 'val '
        elif self.is_unsupported and self.cell_type != None:
            type_str = f'celltype = {self.cell_type.value}, cellname = {self.cell_type.name} not implemented'
            
        if self.is_overflow:
            type_str = f'overflow {type_str}'
        if self.is_short:
            type_str = f'short {type_str}'
        if self.prefix is not None:
            type_str += f'prefix={hex(self.prefix)}'
        if not self.is_unsupported:
            type_str += f'{len(self.data)} bytes'
        
        return type_str
    
    def is_valid_type(self) -> bool:
        if self.is_address or self.is_key or self.is_value or self.is_unsupported:
            return True
        return False
    
    
    def print_timestamps(self, p):
        if self.extra_descriptor == 0:
            return

        p.rint_v('cell has timestamps:')
        if self.prepared:
            p.rint_v(' prepared')

        if self.start_ts is not None:
            p.rint_v(' start ts: ' + binary_data.ts(self.start_ts))
        if self.start_txn is not None:
            p.rint_v(' start txn: ' + binary_data.txn(self.start_txn))
        if self.durable_start_ts is not None:
            p.rint_v(' durable start ts: ' + binary_data.ts(self.durable_start_ts))

        if self.stop_ts is not None:
            p.rint_v(' stop ts: ' + binary_data.ts(self.stop_ts))
        if self.stop_txn is not None:
            p.rint_v(' stop txn: ' + binary_data.txn(self.stop_txn))
        if self.durable_stop_ts is not None:
            p.rint_v(' durable stop ts: ' + binary_data.ts(self.durable_stop_ts))
    
    def process_timestamps(self, pagestats: PageStats):
        pagestats.process_timestamps(self)
        

class DisaggAddrFlags(enum.IntFlag):
    '''
    Flags for address cookies in disaggregated storage from block.h.
    '''
    WT_BLOCK_DISAGG_ADDR_FLAG_DELTA = 0x1
class DisaggAddr(object):
    '''
    A disaggregated storage address cookie (WT_BLOCK_DISAGG_ADDRESS_COOKIE).
    '''
    version: int
    min_version: int
    page_id: int
    flags: DisaggAddrFlags
    lsn: int
    base_lsn: int
    size: int
    checksum: int
    
    def __init__(self) -> None:
        self.version = 0
        self.min_version = 0
        self.page_id = 0
        self.flags = 0
        self.lsn = 0
        self.base_lsn = 0
        self.size = 0
        self.checksum = 0
        
    @staticmethod
    def parse(b: bytes) -> 'DisaggAddr':
        '''
        Parse a packed address cookie.
        '''
        addr = DisaggAddr()
        
        # The first byte contains the version and min_version packed into 4b chunks.
        # See block_disagg_addr.c and int4bitpack_inline.h for implementation details.
        version_array = binary_data.unpack_4b_array((b[:1]), 2)
        addr.version = version_array[0]
        addr.min_version = version_array[1]
        
        b = b[1:]
        
        addr.page_id, b = binary_data.unpack_int(b)
        flags, b = binary_data.unpack_int(b)
        addr.flags = DisaggAddrFlags(flags)
        addr.lsn, b = binary_data.unpack_int(b)
        addr.base_lsn, b = binary_data.unpack_int(b)
        addr.size, b = binary_data.unpack_int(b)
        addr.checksum = int.from_bytes(b, 'little')
        
        return addr
    
    def __str__(self):
        addr_string = (
            f"Disagg Page Address:\n"
            f"  version: {str(self.version)}\n"
            f"  min_version: {str(self.min_version)}\n"
            f"  page_id: {str(self.page_id)}\n"
            f"  flags: {str(self.flags)}\n"
            f"  lsn: {str(self.lsn)}\n"
            f"  size: {str(self.size)}\n"
            f"  checksum: {hex(self.checksum)}\n"
        )
        return addr_string


@dataclass
class WTPage:
    """
    Representation of a decoded WT page.
    """

    success: bool = False

    page_header: Optional[PageHeader] = None
    block_header: Optional[Union[BlockHeader, BlockDisaggHeader]] = None
    cells: Optional[List[Cell]] = None
    extents: Optional[List[ExtentItem]] = None

    raw_bytes: binary_data.BinaryFile = None

    @staticmethod
    def parse(b: binary_data.BinaryFile, nbytes: int, opts) -> 'WTPage':
        page = WTPage(success=False)

        page.raw_bytes = b

        disk_pos = b.tell()

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
        page.page_header = PageHeader.parse(b_page)
        # WT_BLOCK_HEADER in block.h (12 bytes or 44 bytes)
        if opts.disagg:
            page.block_header = BlockDisaggHeader.parse(b_page)
        else:
            page.block_header = BlockHeader.parse(b_page)

        if page.page_header.unused != 0:
            logger.error('? garbage in unused bytes')
            return page
        if page.page_header.type == PageType.WT_PAGE_INVALID:
            logger.error('? invalid page')
            return page

        if page.block_header.unused != 0:
            logger.error('garbage in unused bytes')
            return page

        disk_size = nbytes if opts.disagg else page.block_header.disk_size

        if disk_size > 17 * 1024 * 1024:
            # The maximum document size in MongoDB is 16MB. Larger block sizes are suspect.
            logger.error('the block is too big')
            return page
        if disk_size < 40 and not opts.disagg:
            # The disk size is too small
            return page

        pagestats = PageStats()

        # Optional dependency: crc32c
        have_crc32c = False
        try:
            import crc32c
            have_crc32c = True
        except:
            pass

        # Verify the checksum
        if have_crc32c:
            savepos = b.tell()
            b.seek(disk_pos)
            if (opts.disagg and page.block_header.flags & BlockDisaggFlags.WT_BLOCK_DISAGG_DATA_CKSUM) \
                or (not opts.disagg and page.block_header.flags & BlockFlags.WT_BLOCK_DATA_CKSUM):
                check_size = disk_size
            else:
                check_size = 64
            data = bytearray(b.read(check_size))
            b.seek(savepos)
            # Zero-out the checksum field
            data[32] = data[33] = data[34] = data[35] = 0
            if len(data) < check_size:
                logger.error('? reached EOF before the end of the block')
                return page
            checksum = crc32c.crc32c(data)
            if checksum != page.block_header.checksum:
                logger.error(f'? the calculated checksum {hex(checksum)} does not match header checksum {page.block_header.checksum}')
                if (not opts.cont):
                    return page

        # Skip the rest if we don't want to display the data
        skip_data = opts.skip_data

        if skip_data:
            b.seek(disk_pos + disk_size)
            page.success = True
            return page

        # Read the block contents
        payload_pos = b.tell()
        header_length = payload_pos - disk_pos
        if page.page_header.flags & PageFlags.WT_PAGE_COMPRESSED:
            payload_data = snappy_decompress_page(b, page.page_header, header_length, disk_size, disk_pos, opts)
        else:
            payload_data = b.read(page.page_header.mem_size - header_length)
            b.seek(disk_pos + disk_size)

        # Add the payload to the page data & reinitialize the stream and the printer
        page_data.extend(payload_data)
        b_page = binary_data.BinaryFile(io.BytesIO(page_data))
        b_page.seek(header_length)
        p = Printer(b_page, opts)

        # Parse the block contents
        if page.page_header.type == PageType.WT_PAGE_INVALID:
            pass    # a blank page: TODO maybe should check that it's all zeros?
        elif page.page_header.type == PageType.WT_PAGE_BLOCK_MANAGER:
            extents = page.decode_extlist(b_page)
            page.extents = extents
        elif page.page_header.type == PageType.WT_PAGE_ROW_INT or \
            page.page_header.type == PageType.WT_PAGE_ROW_LEAF:
            cells = page.decode_rows(b_page, p, pagestats)
            page.cells = cells
        else:
            logger.warning('? unimplemented decode for page type {}'.format(page.page_header.type))

        PageStats.outfile_stats_end(opts, page.page_header, page.block_header, pagestats)
        page.success = True
        return page

    def print_page(self, opts):
        p = Printer(self.raw_bytes, opts)
        p.rint(self.page_header)
        p.rint(self.block_header)
        
        # Don't print the cell data unless configured.
        if not opts.verbose:
            return
        
        if self.page_header.type == PageType.WT_PAGE_INVALID:
            pass    # a blank page: TODO maybe should check that it's all zeros?
        elif self.page_header.type == PageType.WT_PAGE_BLOCK_MANAGER:
            self.print_extents(p, opts)
        elif self.page_header.type == PageType.WT_PAGE_ROW_INT or \
            self.page_header.type == PageType.WT_PAGE_ROW_LEAF:
            self.print_cells(p, opts)
        elif self.page_header.type == PageType.WT_PAGE_OVFL:
            # Use b_page.read() so that we can also print the raw bytes in the split mode
            b_page = self.raw_bytes
            p.rint_v(raw_bytes(b_page.read(len(self.raw_bytes))))
        else:
            logger.warning(f'? unimplemented decode for page type {self.page_header.type}')
            p.rint_v(binary_to_pretty_string(self.raw_bytes))
        
        return

    def print_cells(self, p, opts):
        # Optional dependency: bson
        have_bson = False
        bson = None
        if opts.bson:
            try:
                import bson
                have_bson = True
            except ImportError as e:
                logger.error(f'Failed to import bson: {e}')

        for cellnum, cell in enumerate(self.cells):
            p.begin_cell(cellnum)
            p.rint_v(cell.descriptor_string())
            p.rint_v(cell.type_string())
            cell.print_timestamps(p)

            # Print the contents of the cell.
            try:
                # Attempt the decode the cell as BSON.
                if (cell.is_value and opts.bson and have_bson):
                    decoded_data = bson.BSON(cell.data).decode()
                    p.rint_v(pprint.pformat(decoded_data, indent=2))
                # If the cell is an address and we're in disagg mode, print the cell as a DisaggAddr
                # type.
                elif cell.is_address and opts.disagg:
                    addr = DisaggAddr.parse(cell.data)
                    p.rint(json.dumps(addr.__dict__))
                else:
                    p.rint_v(raw_bytes(cell.data))
            except bson.InvalidBSON as e:
                p.rint_v(f"cannot decode cell as BSON: {e}")
                p.rint_v(raw_bytes(cell.data))
            except (IndexError, ValueError):
                # FIXME-WT-13000 theres a bug in raw_bytes
                pass
            
            p.end_cell()
            
    def print_extents(self, p, opts):
        p.rint_ext('extent list follows:')
        for extnum, extent in enumerate(self.extents):
            p.begin_cell(extnum)
            p.rint_ext(f'  {extent.offset}, {extent.size}{extent.extra_stuff}')

        
    def decode_rows(self, b, p , pagestats) -> List[Cell]:
        cells = []
        for cellnum in range(0, self.page_header.entries):
            cellpos = b.tell()
            if cellpos >= self.page_header.mem_size:
                logger.warning('** OVERFLOW memsize **')
                return cells

            # try:
            cell = Cell.parse(b, True)
            cells.append(cell)
            
            if cell.has_timestamps():
                cell.process_timestamps(pagestats)

            if cell.is_key:
                pagestats.num_keys += 1
                pagestats.keys_sz += len(cell.data)
            
            # If the cell cannot be decoded as a valid type, dump the raw bytes and raise an error.
            if not cell.is_valid_type():
                dumpraw(p, b, cellpos)
                raise ValueError('Unexpected cell type')
        
        return cells
        
    def decode_extlist(self, b) -> List[ExtentItem]:
        # Written by block_ext.c
        extents = []
        okay = True
        cellnum = -1
        lastoff = 0
        # p.rint_ext('extent list follows:')
        while True:
            cellnum += 1
            cellpos = b.tell()
            if cellpos >= self.page_header.mem_size:
                logger.warning(f'** OVERFLOW memsize ** memsize={self.page_header.mem_size}, position={cellpos}')
                return extents

            extent = ExtentItem.parse(b)
            extents.append(extent)
            extra_stuff = ''
            
            if cellnum == 0:
                extra_stuff += '  # magic number'
                if not extent.is_magic():
                    logger.error(f'Magic number did not match expected value=\
                        {ExtentItem.WT_BLOCK_EXTLIST_MAGIC}')
                    okay = False
            else:
                if extent.offset < lastoff and not extent.is_end_of_list():
                    logger.error(f'Extent list out of order')
                    okay = False

                # We expect sizes and positions to be multiples of
                # this number, it is conservative.
                multiple = 256
                if extent.offset % multiple != 0:
                    logger.error(f'Offset is not a multiple of {multiple}')
                    okay = False
                if extent.offset != 0 and extent.size % multiple != 0:
                    logger.error(f'Size is not a multiple of {multiple}')
                    okay = False

            # A zero offset is written as an end of list marker,
            # in that case, the size is a version number.
            # For version 0, this is truly the end of the list.
            # For version 1, additional entries may be appended to this (avail) list.
            #
            # See __wti_block_extlist_write() in block_ext.c, and calls
            # to that function in block_ckpt.c.
            if extent.is_end_of_list():
                extra_stuff += '  # end of list'
                if extent.size == 0:
                    extra_stuff += ', version 0'
                elif extent.size == 1:
                    extra_stuff += ', version 1,' + \
                    ' any following entries are not yet in this (incomplete) checkpoint'
                else:
                    logger.error(f'Unexpected size={extent.size} has no meaning here')
                    okay = False
            
            extent.extra_stuff = extra_stuff
            if not extent.is_magic():
                lastoff = extent.offset

            if extent.is_end_of_list() or not okay:
                break
        
        return extents

