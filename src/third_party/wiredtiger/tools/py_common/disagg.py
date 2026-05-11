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

from dataclasses import dataclass
import enum
import io
import logging
from typing import List, Optional

from py_common import binary_data, btree_format
from py_common.decode_opts import DecodeOptions
from py_common.printer import Printer


logger = logging.getLogger(__name__)


class UpdateTypeFlags(enum.IntFlag):
    '''
    Update types as defined in page_service.proto
    '''
    UPDATE_TYPE_DELTA = 0
    UPDATE_TYPE_FULL_IMAGE = 1
    UPDATE_TYPE_TOMBSTONE = 2
    UPDATE_TYPE_CHECKPOINT_END = 3


@dataclass
class Metadata:
    lsn: int
    page_id: int
    table_id: int
    base_lsn: int = 0
    backlink_lsn: int = 0
    delta: bool = False

    def is_delta(self):
        return self.delta

    def is_metadata_page(self):
        return self.table_id == 1

    def __str__(self):
        return (
            f'Disagg Page Metadata:\n'
            f'  page_id: {self.page_id}\n'
            f'  table_id: {self.table_id}\n'
            f'  lsn: {self.lsn}\n'
            f'  base_lsn: {self.base_lsn}\n'
            f'  backlink_lsn: {self.backlink_lsn}\n'
        )


@dataclass
class DisaggTableSummary:
    total_pages: int = 0
    delta_pages: int = 0
    full_pages: int = 0

    def update_with_page(self, page: Metadata):
        self.total_pages += 1
        if page.is_delta():
            self.delta_pages += 1
        else:
            self.full_pages += 1


@dataclass
class DisaggPage:
    metadata: Metadata
    page_bytes: bytes


def process_disagg_pages(disagg_pages, opts: DecodeOptions) -> DisaggTableSummary:
    table_summary = DisaggTableSummary()

    for pages in disagg_pages:
        delta_chain: List[btree_format.WTPage] = []

        for disagg_page in pages:
            metadata = disagg_page.metadata
            page_bytes = disagg_page.page_bytes
            b = binary_data.BinaryFile(io.BytesIO(page_bytes))
            p = Printer(b, split=opts.split)

            p.rint(metadata)

            if metadata.is_metadata_page():
                p.rint('Disagg Metadata File:')
                page_string = page_bytes.decode('ascii')
                p.rint(f'  {page_string}')

                p.rint('Metadata Table Root Page:')
                addr_string = page_string.split('addr="')[1].split('"')[0]
                addr = btree_format.DisaggAddr.parse(bytes.fromhex(addr_string))
                p.rint(addr)
                p.rint('')
                continue

            page = btree_format.WTPage()
            page = page.parse(b, len(page_bytes),
                              disagg=True,
                              skip_data=opts.skip_data,
                              cont=opts.cont)

            if metadata.is_delta():
                if page.block_header.magic != btree_format.BlockDisaggHeader.WT_BLOCK_DISAGG_MAGIC_DELTA:
                    logger.error(
                        f'Delta page has incorrect block flag: '
                        f'{page.block_header.magic}'
                    )

                if delta_chain:
                    prev_page = delta_chain[-1]
                    prev_checksum = prev_page.block_header.previous_checksum
                    if prev_checksum != page.block_header.checksum:
                        logger.error(
                            'Checksum mismatch between delta pages. '
                            f'Previous checksum: {prev_checksum}, '
                            f'current checksum: {page.block_header.checksum}'
                        )

                    if prev_page.page_header.write_gen <= page.page_header.write_gen:
                        logger.error(
                            'Write generation detected out-of-order between '
                            f'pages: page {page}, previous page: {prev_page}'
                        )

                delta_chain.append(page)
            else:
                if page.block_header.magic != btree_format.BlockDisaggHeader.WT_BLOCK_DISAGG_MAGIC_BASE:
                    logger.error(
                        'Full page has incorrect block flag: '
                        f'{page.block_header.magic}'
                    )

                delta_chain = []

            table_summary.update_with_page(metadata)
            if opts.skip_data:
                p.rint(page.page_header)
                p.rint(page.block_header)
            else:
                page.print_page(split=opts.split,
                                decode_as_bson=opts.bson,
                                disagg=True)
            p.rint('')

    return table_summary
