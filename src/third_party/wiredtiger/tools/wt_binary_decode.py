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

import logging, os, sys
from py_common import mdb_log_parse
from py_common import binary_data, page_service
from py_common import file_format

logger = logging.getLogger(__name__)

decode_version = "2023-03-03.0"


_python3 = (sys.version_info >= (3, 0, 0))

if not _python3:
    raise Exception('This script requires Python 3')

################################################################

def wtdecode(opts):
    if opts.dumpin:
        opts.fragment = True
        if opts.filename == '-':
            mdb_log_parse.process_logs(sys.stdin, opts)
        else:
            with open(opts.filename, "r") as infile:
                mdb_log_parse.process_logs(infile, opts)
    elif opts.disagg_table:
        opts.disagg = True
        opts.fragment = True
        if opts.filename == '-':
            page_service.process_disagg_table(sys.stdin, opts)
        else:
            with open(opts.filename, "r") as infile:
                page_service.process_disagg_table(infile, opts)
        
    elif opts.filename == '-':
        nbytes = 0      # unknown length
        print('stdin, position ' + hex(opts.offset) + ', pagelimit ' +  str(opts.pages))
        file_format.wtdecode_file_object(binary_data.BinaryFile(sys.stdin.buffer), opts, nbytes)
    else:
        nbytes = os.path.getsize(opts.filename)
        print(opts.filename + ', position ' + hex(opts.offset) + '/' + hex(nbytes) + ', pagelimit ' +  str(opts.pages))
        with open(opts.filename, "rb") as fileobj:
            file_format.wtdecode_file_object(binary_data.BinaryFile(fileobj), opts, nbytes)

def get_arg_parser():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('-b', '--bytes', help="show bytes alongside decoding", action='store_true')
    parser.add_argument('--bson', help="decode cell values as bson data", action='store_true')
    parser.add_argument("-c", "--csv", type=argparse.FileType('w'), dest='output', help="output filename for summary of page statistics in CSV format", action='store')
    parser.add_argument('--continue', help="continue on checksum failure", dest='cont', action='store_true')
    parser.add_argument('-D', '--debug', help="debug this tool", action='store_true')
    parser.add_argument('-d', '--dumpin', help="input is hex dump (may be embedded in log messages)", action='store_true')
    parser.add_argument('--disagg_table', help="input is a full disagg table from the GetTableAtLSN endpoint on the Object Read Proxy (ORP). \
        The table can be downloaded from S3 as a jsonl file containing all of the pages linked to a table_id. ", action='store_true')
    parser.add_argument('--disagg', help="input comes from disaggregated storage", action='store_true')
    parser.add_argument('--ext', help="dump only the extent lists", action='store_true')
    parser.add_argument('-f', '--fragment', help="input file is a fragment, does not have a WT file header", action='store_true')
    parser.add_argument('--keyfile', help="Keyfile path used for mongodb encryption", type=str)
    parser.add_argument('--log', help="Debug logs for decoding logic", choices=['DEBUG', 'INFO', 'WARNING', 'ERROR', 'CRITICAL'], default='INFO')
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

    logging.basicConfig(level=opts.log.upper(), format='[%(levelname)s] %(message)s')
    
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
