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

import argparse
import logging
import os
import sys
import collections

from contextlib import nullcontext
from py_common import mdb_log_parse
from py_common import binary_data
from py_common.decode_opts import DecodeOptions
from py_common import btree_format
from py_common import snappy_util
from py_common import file_format
from py_common import page_service
from py_common import sqlite_format

logger = logging.getLogger(__name__)

decode_version = '2026-03-01.0'

################################################################

def open_input_file(filename, mode):
    if filename == '-':
        stream = sys.stdin if 'b' not in mode else sys.stdin.buffer
        return nullcontext(stream)
    return open(filename, mode)


def open_output_file(filename, mode):
    return open(filename, mode) if filename else nullcontext()


def decode_dumpin_input(filename, opts: DecodeOptions):
    with open_input_file(filename, 'r') as infile:
        mdb_log_parse.process_logs(infile, opts)


def decode_disagg_table_input(filename, opts: DecodeOptions):
    with open_input_file(filename, 'r') as infile:
        page_service.process_disagg_table(infile, opts)


def decode_sqlite_input(filename, opts: DecodeOptions):
    logger.info('Detected SQLite3 input format.')
    sqlite_format.process_sqlite_file(filename, opts)


def decode_wt_binary_input(filename, opts: DecodeOptions):
    nbytes = 0 if filename == '-' else os.path.getsize(filename)
    input_name = 'stdin' if filename == '-' else filename
    input_size = 'unknown' if filename == '-' else hex(nbytes)
    print(f'{input_name}, position {hex(opts.offset)}, size {input_size}, '
          f'pagelimit {opts.pages}')
    with open_input_file(filename, 'rb') as fileobj:
        file_format.wtdecode_file_object(binary_data.BinaryFile(fileobj), nbytes, opts)


def wtdecode(filename, opts: DecodeOptions):
    if opts.dumpin:
        decode_dumpin_input(filename, opts)
    elif opts.disagg_table:
        decode_disagg_table_input(filename, opts)
    elif sqlite_format.is_sqlite3_file(filename):
        decode_sqlite_input(filename, opts)
    else:
        decode_wt_binary_input(filename, opts)


def feature_check(*, bson: bool = False):
    Feature = collections.namedtuple('Feature',
                                     ['available', 'requested', 'message'])
    features = [
        Feature(btree_format.HAVE_BSON, bson,
                'BSON decoding (--bson) is not available. '
                'BSON-encoded cell values will be shown as raw bytes. '
                'Please install the bson library (pip install pymongo).'),
        Feature(snappy_util.HAVE_SNAPPY, True,
                'Snappy decompression is not available. '
                'Compressed pages will not be decompressed. '
                'Please install the snappy library (pip install python-snappy).'),
        Feature(btree_format.HAVE_CRC32C, True,
                'CRC32C library is not available. '
                'Checksums will not be verified. '
                'Please install the crc32c library (pip install crc32c).')
    ]

    for feature in features:
        if not feature.available and feature.requested:
            logger.warning(feature.message)


def get_arg_parser():
    parser = argparse.ArgumentParser(description='Decode WiredTiger binary data')

    # Misc arguments
    parser.add_argument('-V', '--version',
        action='version',
        version=f'wt_binary_decode version: {decode_version}',
        help='print version number of this program')

    # Positional argument for input file (or '-' for stdin)
    parser.add_argument('filename',
        help="input file name or '-' for stdin")

    # Arguments that control input
    inargs = parser.add_argument_group('input options')
    inargs.add_argument('-d', '--dumpin',
        action='store_true',
        help='input is hex dump (may be embedded in log messages)')
    inargs.add_argument('--bson',
        action='store_true',
        help='decode cell values as BSON data (requires bson library)')
    inargs.add_argument('--disagg-table',
        action='store_true',
        help=('input is a full disagg table from the GetTableAtLSN endpoint '
              'on the Object Read Proxy (ORP). '
              'The table can be downloaded from S3 as a jsonl file '
              'containing all of the pages linked to a table_id.'))
    inargs.add_argument('--disagg',
        action='store_true',
        help='input comes from disaggregated storage')
    inargs.add_argument('-o', '--offset',
        type=int,
        default=0,
        help='seek offset before decoding')
    inargs.add_argument('--page-id',
        type=int,
        default=None,
        help=('for sqlite3 input, decode only the selected page_id; if '
              '--lsn is not set, the most recent lsn is used'))
    inargs.add_argument('--lsn',
        type=int,
        default=None,
          help=('for sqlite3 input, decode only the record with this lsn; '
                'when set, delta-chain traversal is disabled'))
    inargs.add_argument('-p', '--pages',
        type=int,
        default=0,
        help='number of pages to decode')
    inargs.add_argument('--skip-data',
        action='store_true',
        help='do not read/process data')
    inargs.add_argument('--keyfile',
        type=str,
        help='Keyfile path used for mongodb encryption')

    # Arguments that control output
    outargs = parser.add_argument_group('output options')
    outargs.add_argument('-v', '--verbose',
        action='count',
        default=0,
        help='verbose logging output (repeat for more verbosity: -v, -vv)')
    outargs.add_argument('-b', '--bytes',
        action='store_true',
        help='show bytes alongside decoding')
    outargs.add_argument('-c', '--csv',
        type=str,
        dest='output',
        action='store',
        help='output filename for summary of page statistics in CSV format')
    outargs.add_argument('--continue',
        dest='cont',
        action='store_true',
        help='continue on checksum failure')
    outargs.add_argument('-s', '--split',
        action='store_true',
        help='split output to also show raw bytes')

    return parser


# Only run the main code if this file is not imported.
if __name__ == '__main__':
    parser = get_arg_parser()
    args = parser.parse_args()

    log_levels = [logging.WARNING, logging.INFO, logging.DEBUG]
    level = log_levels[min(args.verbose, len(log_levels) - 1)]
    logging.basicConfig(level=level, format='[%(levelname)s] %(message)s')

    feature_check(bson=args.bson)

    try:
        with open_output_file(args.output, 'w') as output_file:
            opts = DecodeOptions(
                dumpin=args.dumpin,
                disagg_table=args.disagg_table,
                disagg=args.disagg,
                skip_data=args.skip_data,
                cont=args.cont,
                split=args.split,
                bson=args.bson,
                output=output_file,
                offset=args.offset,
                pages=args.pages,
                keyfile=getattr(args, 'keyfile', None),
                lsn=args.lsn,
                page_id=args.page_id,
            )
            wtdecode(args.filename, opts)
    except (KeyboardInterrupt, BrokenPipeError):
        pass
