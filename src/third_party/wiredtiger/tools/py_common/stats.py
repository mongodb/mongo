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
import typing

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
        
    @staticmethod
    def outfile_stats_start(opts, blockid):
        if opts.output != None:
            opts.output.write("\n" + blockid + ",")
    
    @staticmethod
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

    def process_timestamps(self, cell) -> None:
        """
        Update this PageStats instance with timestamp/transaction information
        from a parsed `cell`.
        """
        # If the cell has no extra descriptor, nothing to do.
        if getattr(cell, 'extra_descriptor', 0) == 0:
            return

        if getattr(cell, 'start_ts', None) is not None:
            self.start_ts_sz += getattr(cell, 'size_start_ts', 0)
            self.num_start_ts += 1
        if getattr(cell, 'start_txn', None) is not None:
            self.start_txn_sz += getattr(cell, 'size_start_txn', 0)
            self.num_start_txn += 1
        if getattr(cell, 'durable_start_ts', None) is not None:
            self.d_start_ts_sz += getattr(cell, 'size_durable_start_ts', 0)
            self.num_d_start_ts += 1

        if getattr(cell, 'stop_ts', None) is not None:
            self.stop_ts_sz += getattr(cell, 'size_stop_ts', 0)
            self.num_stop_ts += 1
        if getattr(cell, 'stop_txn', None) is not None:
            self.stop_txn_sz += getattr(cell, 'size_stop_txn', 0)
            self.num_stop_txn += 1
        if getattr(cell, 'durable_stop_ts', None) is not None:
            self.d_stop_ts_sz += getattr(cell, 'size_durable_stop_ts', 0)
            self.num_d_stop_ts += 1
