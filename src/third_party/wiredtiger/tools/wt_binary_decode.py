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

import codecs, io, logging, os, re, sys, traceback
from py_common import binary_data, btree_format, page_service
from py_common.printer import Printer
from py_common.stats import PageStats
from py_common.input import encode_bytes

logger = logging.getLogger(__name__)

decode_version = "2023-03-03.0"


_python3 = (sys.version_info >= (3, 0, 0))

if not _python3:
    raise Exception('This script requires Python 3')

################################################################

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
        PageStats.outfile_stats_start(opts, d_h)
        print('Decode at ' + d_h)
        b.seek(startblock)
        try:
            page = btree_format.WTPage.parse(b, nbytes, opts)
            if page.success:
                page.print_page(opts)
            p.rint('')
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

def extract_mongodb_log_hex(f, opts):
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
            # If we don't have a JSON log line, then this isn't a MongoDB log. Reset the file 
            # pointer to the start to read all the bytes again.
            f.seek(0)
            return encode_bytes(f, opts)
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
            allbytes = extract_mongodb_log_hex(sys.stdin, opts)
        else:
            with open(opts.filename, "r") as infile:
                allbytes = extract_mongodb_log_hex(infile, opts)
        b = binary_data.BinaryFile(io.BytesIO(allbytes))
        wtdecode_file_object(b, opts, len(allbytes))
    elif opts.disagg_table:
        opts.disagg = True
        opts.fragment = True
        if opts.filename == '-':
            page_service.extract_disagg_pages(sys.stdin, opts)
        else:
            with open(opts.filename, "r") as infile:
                page_service.extract_disagg_pages(infile, opts)
        
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
